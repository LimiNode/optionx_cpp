#include <gtest/gtest.h>
#include "support/trading_system/SyntheticHost.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>

namespace {
namespace ex = optionx::test_support;
namespace ts = optionx::trading_system;
constexpr std::int64_t noon = 12 * 60 * 60 * 1000;

struct Database {
    static std::atomic<unsigned> next;
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("optionx_batch6_" + std::to_string(std::chrono::high_resolution_clock::now()
            .time_since_epoch().count()) + "_" + std::to_string(next++) + ".mdbx");
    ~Database() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-lck", error);
    }
    mdbxc::Config config() const {
        mdbxc::Config result;
        result.pathname = path.string();
        result.no_subdir = true;
        return result;
    }
};
std::atomic<unsigned> Database::next{0};

ex::SyntheticConfig configuration() {
    ex::SyntheticConfig config;
    config.accounts[1000] = {ts::RiskAvailability::SUPPORTED, ex::usd(1000), noon, 1};
    return config;
}

using Broker = ts::SimulatedExecutionAdapter;

std::size_t submissions(const std::shared_ptr<Broker::State>& broker) {
    std::size_t count = 0;
    for (const auto& entry : broker->submission_counts) count += entry.second;
    return count;
}

ts::SettlementRecord stored_settlement(ts::SettlementId id) {
    ts::TradeSettlement settlement;
    settlement.settlement_id = id;
    settlement.intent_id = 1;
    settlement.execution_group_id = 1;
    settlement.trade_id = 1;
    settlement.kind = ts::OutcomeKind::WIN;
    settlement.evidence = ts::ExecutionEvidence::EXECUTED;
    settlement.net_profit = ex::usd(1);
    return {settlement, ex::usd(1), std::nullopt, true,
            ts::trading_storage_schema_version};
}

ts::ReservationRecord scope_reservation(
    std::uint64_t id, ts::RiskScopeKind kind = ts::RiskScopeKind::ACCOUNT,
    std::uint64_t scope = 42) {
    ts::ReservationRecord value;
    value.reservation_id = id;
    value.risk_scope_id = scope;
    value.risk_scope_kind = kind;
    value.intent_id = 1;
    value.execution_group = {1, 1};
    value.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
    value.stake = ex::usd(1);
    value.potential_risk = ex::usd(1);
    value.idempotency_key = "risk-" + std::to_string(id);
    return value;
}

void expect_no_execution(ex::SyntheticHost& host, const std::shared_ptr<Broker::State>& broker) {
    EXPECT_TRUE(host.storage().reservations().values.empty());
    EXPECT_TRUE(host.storage().journal_events().values.empty());
    EXPECT_EQ(host.storage().intent(1).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(host.storage().roster(1).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(host.storage().physical_trade(1).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(host.series().series.state_revision, 0U);
    EXPECT_EQ(host.series().series.next_step, 0U);
    EXPECT_EQ(host.mm_state().state_revision, 0U);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(submissions(broker), 0U);
}

TEST(TradingSystemBatch6, SuccessfulBuyPersistsWinAndAdvancesOnce) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), configuration(), noon, broker);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    EXPECT_EQ(host.series().status, ts::SeriesStatus::WAITING_OUTCOME);
    EXPECT_EQ(host.series().series.state_revision, 1U);
    EXPECT_EQ(host.reserved(1000), 100);
    const auto intent = ex::read(host.storage().intent(1));
    EXPECT_EQ(intent.request.order_type, optionx::OrderType::BUY);
    EXPECT_EQ(intent.request.amount, 1.0);
    ASSERT_TRUE(host.submit(group).succeeded());
    host.broker_result(group, 0, 80);
    ASSERT_TRUE(host.observe(group).succeeded());
    EXPECT_EQ(host.series().series.next_step, 0U);
    ASSERT_TRUE(host.complete(group).succeeded());
    EXPECT_EQ(host.series().series.next_step, 1U);
    EXPECT_EQ(host.series().series.state_revision, 2U);
    EXPECT_EQ(host.series().current_outcome.kind, ts::OutcomeKind::WIN);
    EXPECT_EQ(host.mm_state().state_revision, 1U);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(host.complete(group).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(host.mm_state().state_revision, 1U);
    EXPECT_EQ(submissions(broker), 1U);
}

TEST(TradingSystemBatch6, SymbolRejectionHasNoExecutionSideEffects) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), configuration(), noon, broker);
    auto signal = ex::synthetic_buy();
    signal.symbol = "GBP/USD";
    EXPECT_FALSE(host.accept(signal).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, TimeWindowRejectionHasNoExecutionSideEffects) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), configuration(), 18 * 60 * 60 * 1000, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, InsufficientRiskHasNoPartialPreparation) {
    Database db;
    auto config = configuration();
    config.accounts[1000].available_balance = ex::usd(99);
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, MissingAccountSnapshotHasNoPartialPreparation) {
    Database db;
    auto config = configuration();
    config.accounts[1000].availability = ts::RiskAvailability::UNAVAILABLE;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, StaleAccountSnapshotHasNoPartialPreparation) {
    Database db;
    auto config = configuration();
    config.accounts[1000].captured_at_ms = noon - 30'001;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, LaterScopeRejectionRollsBackEarlierOwnerAdmission) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 2000}};
    config.accounts[2000] = {ts::RiskAvailability::SUPPORTED, ex::usd(99), noon, 1};
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
    EXPECT_EQ(host.reserved(2000), 0);
}

TEST(TradingSystemBatch6, OneLossStepSurvivesReopenAndWinResetsStake) {
    Database db;
    auto config = configuration();
    config.loss_step = true;
    auto broker = std::make_shared<Broker::State>();
    {
        ex::SyntheticHost host(db.config(), config, noon, broker);
        ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
        const auto group = host.series().current_group_id;
        ASSERT_TRUE(host.submit(group).succeeded());
        host.broker_result(group, 0, -100);
        ASSERT_TRUE(host.observe(group).succeeded());
        ASSERT_TRUE(host.complete(group).succeeded());
        EXPECT_TRUE(host.mm_state().payload.at("after_loss").get<bool>());
    }
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_NE(host.mm_state().last_settlement_id, 1U);
    EXPECT_NE((host.mm_state().last_settlement_id & (std::uint64_t{1} << 63)), 0U);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    EXPECT_EQ(ex::read(host.storage().roster(group)).members.front().stake.units, 200);
    EXPECT_EQ(host.reserved(1000), 200);
    ASSERT_TRUE(host.submit(group).succeeded());
    host.broker_result(group, 0, 160);
    ASSERT_TRUE(host.observe(group).succeeded());
    ASSERT_TRUE(host.complete(group).succeeded());
    EXPECT_EQ(host.series().series.next_step, 2U);
    EXPECT_EQ(host.series().series.state_revision, 4U);
    EXPECT_EQ(host.mm_state().state_revision, 2U);
    EXPECT_FALSE(host.mm_state().payload.at("after_loss").get<bool>());
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    EXPECT_EQ(host.reserved(1000), 100);
}

TEST(TradingSystemBatch6, IncreasedStakeStillRequiresRiskAdmission) {
    Database db;
    auto config = configuration();
    config.loss_step = true;
    config.accounts[1000].available_balance = ex::usd(150);
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    ASSERT_TRUE(host.submit(group).succeeded());
    host.broker_result(group, 0, -100);
    ASSERT_TRUE(host.observe(group).succeeded());
    ASSERT_TRUE(host.complete(group).succeeded());
    const auto journal_size = host.storage().journal_events().values.size();
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    EXPECT_EQ(host.storage().journal_events().values.size(), journal_size);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(host.mm_state().state_revision, 1U);
    EXPECT_EQ(submissions(broker), 1U);
}

TEST(TradingSystemBatch6, AllRejectedGroupReleasesRiskAndCompletesWithoutProgression) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), configuration(), noon, broker);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    ASSERT_TRUE(host.submit(group, ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION).succeeded());
    EXPECT_EQ(host.reserved(1000), 0);
    ASSERT_TRUE(host.observe(group).succeeded());
    ASSERT_TRUE(host.complete(group).succeeded());
    EXPECT_EQ(host.series().series.next_step, 0U);
    EXPECT_EQ(host.series().current_outcome.kind, ts::OutcomeKind::REJECTED);
    const auto reservations = host.storage().reservations();
    ASSERT_EQ(reservations.values.size(), 1U);
    EXPECT_EQ(reservations.values.front().state, ts::RiskReservationState::RELEASED);
    EXPECT_EQ(host.complete(group).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(submissions(broker), 1U);
    const auto evidence = host.storage().settlements_for_group(group);
    ASSERT_EQ(evidence.values.size(), 1U);
    EXPECT_EQ(evidence.values.front().settlement.kind, ts::OutcomeKind::REJECTED);
    EXPECT_NE(evidence.values.front().settlement.settlement_id, 700U);
}

TEST(TradingSystemBatch6, MixedExecutionAndRejectionUsesPerMemberReservationStates) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 1000}};
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    ASSERT_TRUE(host.submit_plans(group, {
        ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION,
        ts::SimulatedSubmissionPlan::SUCCESS}).succeeded());
    host.broker_result(group, 1, 80);
    ASSERT_TRUE(host.observe(group).succeeded());
    ASSERT_TRUE(host.complete(group).succeeded());
    EXPECT_EQ(host.series().current_outcome.kind, ts::OutcomeKind::WIN);
    EXPECT_EQ(host.series().current_outcome.net_profit->units, 80);
    const auto reservations = host.storage().reservations();
    ASSERT_EQ(reservations.values.size(), 2U);
    EXPECT_EQ(reservations.values[0].state, ts::RiskReservationState::RELEASED);
    EXPECT_EQ(reservations.values[1].state, ts::RiskReservationState::SETTLED);
    EXPECT_EQ(host.reserved(1000), 0);
}

TEST(TradingSystemBatch6, BrokerAndLocalSettlementNamespacesDoNotCollide) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 1000}};
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto first_group = host.series().current_group_id;
    ASSERT_TRUE(host.submit_plans(first_group, {
        ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION,
        ts::SimulatedSubmissionPlan::SUCCESS}).succeeded());
    const auto broker_settlement = host.broker_result(first_group, 1, 80);
    ASSERT_TRUE(host.observe(first_group).succeeded());
    ASSERT_TRUE(host.complete(first_group).succeeded());

    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto second_group = host.series().current_group_id;
    ASSERT_TRUE(host.submit(second_group, ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION).succeeded());
    ASSERT_TRUE(host.observe(second_group).succeeded());
    ASSERT_TRUE(host.complete(second_group).succeeded());

    const auto first_evidence = host.storage().settlements_for_group(first_group);
    const auto second_evidence = host.storage().settlements_for_group(second_group);
    ASSERT_EQ(first_evidence.values.size(), 2U);
    ASSERT_EQ(second_evidence.values.size(), 2U);
    EXPECT_NE(first_evidence.values[0].settlement.settlement_id,
              first_evidence.values[1].settlement.settlement_id);
    EXPECT_EQ(first_evidence.values[1].settlement.settlement_id, broker_settlement.settlement_id);
    for (const auto& evidence : second_evidence.values) {
        EXPECT_LT(evidence.settlement.settlement_id, ts::external_settlement_namespace_bit);
        EXPECT_NE(evidence.settlement.settlement_id, broker_settlement.settlement_id);
    }
}

TEST(TradingSystemBatch6, SettlementSequenceRebuildIgnoresExternalNamespace) {
    Database db;
    {
        ts::TradingSystemStorage storage(db.config());
        auto unit = storage.begin();
        ASSERT_TRUE(unit.put_settlement(stored_settlement(
            ts::external_settlement_namespace_bit + 1)).succeeded());
        ASSERT_TRUE(unit.put_settlement(stored_settlement(1)).succeeded());
        ASSERT_TRUE(unit.commit().succeeded());
    }
    {
        ts::TradingSystemStorage storage(db.config());
        auto unit = storage.begin();
        ASSERT_TRUE(unit.rebuild_settlement_sequence().succeeded());
        const auto next = unit.next_id("settlement");
        ASSERT_EQ(next.kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(next.value, 2U);
        ASSERT_TRUE(unit.put_settlement(stored_settlement(next.value)).succeeded());
        ASSERT_TRUE(unit.commit().succeeded());
    }
    {
        ts::TradingSystemStorage reopened(db.config());
        auto unit = reopened.begin();
        ASSERT_TRUE(unit.rebuild_settlement_sequence().succeeded());
        const auto next = unit.next_id("settlement");
        EXPECT_EQ(next.kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(next.value, 3U);
        ASSERT_TRUE(unit.commit().succeeded());
    }
}

TEST(TradingSystemBatch6, ExternalOnlyHistoryRebuildsLocalSequence) {
    Database db;
    ts::TradingSystemStorage storage(db.config());
    {
        auto unit = storage.begin();
        ASSERT_TRUE(unit.put_settlement(stored_settlement(
            ts::external_settlement_namespace_bit + 7)).succeeded());
        ASSERT_TRUE(unit.commit().succeeded());
    }
    auto unit = storage.begin();
    ASSERT_TRUE(unit.rebuild_settlement_sequence().succeeded());
    const auto next = unit.next_id("settlement");
    EXPECT_EQ(next.kind, ts::StorageResultKind::APPLIED);
    EXPECT_EQ(next.value, 1U);
    ASSERT_TRUE(unit.commit().succeeded());
}

TEST(TradingSystemBatch6, SettlementSequencePreservesHigherLocalCounterAndHandlesExhaustion) {
    Database db;
    ts::TradingSystemStorage storage(db.config());
    {
        auto unit = storage.begin();
        ASSERT_TRUE(unit.put_settlement(stored_settlement(1)).succeeded());
        ASSERT_EQ(unit.next_id("settlement").value, 2U);
        ASSERT_TRUE(unit.commit().succeeded());
    }
    {
        auto unit = storage.begin();
        ASSERT_TRUE(unit.rebuild_settlement_sequence().succeeded());
        const auto next = unit.next_id("settlement");
        EXPECT_EQ(next.kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(next.value, 3U);
        ASSERT_TRUE(unit.commit().succeeded());
    }

    Database boundary;
    ts::TradingSystemStorage exhausted(boundary.config());
    {
        auto unit = exhausted.begin();
        ASSERT_TRUE(unit.put_settlement(stored_settlement(
            ts::external_settlement_namespace_bit - 1)).succeeded());
        ASSERT_TRUE(unit.commit().succeeded());
    }
    auto unit = exhausted.begin();
    ASSERT_TRUE(unit.rebuild_settlement_sequence().succeeded());
    EXPECT_NE(unit.next_id("settlement").kind, ts::StorageResultKind::APPLIED);
    unit.rollback();
}

TEST(TradingSystemBatch6, SimulatedBrokerSettlementAllocatorStopsAtExternalBoundary) {
    auto state = std::make_shared<Broker::State>();
    ts::SimulatedExecutionAdapter adapter(state);
    state->next_settlement_id = ts::external_settlement_namespace_bit - 2;
    EXPECT_EQ(adapter.next_settlement_id(), (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_EQ(state->next_settlement_id, ts::external_settlement_namespace_bit - 1);
    EXPECT_EQ(adapter.next_settlement_id(), 0U);
    EXPECT_EQ(state->next_settlement_id, ts::external_settlement_namespace_bit - 1);
    EXPECT_EQ(adapter.next_settlement_id(), 0U);

    state->next_settlement_id = ts::external_settlement_namespace_bit - 1;
    EXPECT_EQ(adapter.next_settlement_id(), 0U);
    EXPECT_EQ(state->next_settlement_id, ts::external_settlement_namespace_bit - 1);
}

TEST(TradingSystemBatch6, ConflictingSimulatedTerminalEvidenceIsRejected) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), configuration(), noon, broker);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    ASSERT_TRUE(host.submit(group).succeeded());
    const auto first = host.broker_result(group, 0, 80);
    ASSERT_THROW(host.broker_result(group, 0, -80), std::runtime_error);
    ASSERT_EQ(broker->history.size(), 1U);
    ASSERT_TRUE(broker->history.front().settlement.has_value());
    EXPECT_EQ(broker->history.front().settlement->settlement_id, first.settlement_id);
    ASSERT_TRUE(host.observe(group).succeeded());
    ASSERT_TRUE(host.complete(group).succeeded());
}

TEST(TradingSystemBatch6, ReorderedBrokerResultsReplayCanonically) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 1000}};
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    ASSERT_TRUE(host.submit(group).succeeded());
    const auto second = host.broker_result(group, 1, -20);
    const auto first = host.broker_result(group, 0, 80);
    EXPECT_LT(second.settlement_id, first.settlement_id);
    ASSERT_TRUE(host.observe(group).succeeded());
    ASSERT_TRUE(host.complete(group).succeeded());
    EXPECT_EQ(host.series().current_outcome.kind, ts::OutcomeKind::WIN);
    EXPECT_EQ(host.series().current_outcome.net_profit->units, 60);
}

TEST(TradingSystemBatch6, OperationsFollowRosterOrderAcrossRestart) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 1000}};
    auto broker = std::make_shared<Broker::State>();
    ts::ExecutionGroupId group = 0;
    {
        ex::SyntheticHost host(db.config(), config, noon, broker);
        for (int index = 0; index < 4; ++index) {
            ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
            const auto completed_group = host.series().current_group_id;
            if (index == 0) {
                ASSERT_TRUE(host.submit_plans(completed_group, {
                    ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION,
                    ts::SimulatedSubmissionPlan::SUCCESS}).succeeded());
            } else {
                ASSERT_TRUE(host.submit(completed_group).succeeded());
            }
            if (index != 0) host.broker_result(completed_group, 0, 80);
            host.broker_result(completed_group, 1, 80);
            ASSERT_TRUE(host.observe(completed_group).succeeded());
            ASSERT_TRUE(host.complete(completed_group).succeeded());
        }

        ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
        group = host.series().current_group_id;
        const auto submitted = host.submit_plans(group, {
            ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION,
            ts::SimulatedSubmissionPlan::SUCCESS});
        ASSERT_TRUE(submitted.succeeded())
            << ts::to_string(submitted.kind) << ": " << submitted.message;
    }

    ex::SyntheticHost reopened(db.config(), config, noon, broker);
    const auto roster = ex::read(reopened.storage().roster(group));
    ASSERT_EQ(roster.members.size(), 2U);
    EXPECT_EQ(roster.members[0].trade_id, 9U);
    EXPECT_EQ(roster.members[1].trade_id, 10U);
    const auto operations = reopened.operations(group);
    ASSERT_EQ(operations.size(), 2U);
    EXPECT_EQ(operations[0].trade_id, 9U);
    EXPECT_EQ(operations[0].state, ts::ExecutionOperationState::REJECTED);
    EXPECT_EQ(operations[1].trade_id, 10U);
    EXPECT_EQ(operations[1].state, ts::ExecutionOperationState::BROKER_ID_PERSISTED);

    const auto terminal = reopened.broker_result(group, 1, 80);
    EXPECT_EQ(terminal.trade_id, 10U);
    ASSERT_TRUE(reopened.observe(group).succeeded());
    ASSERT_TRUE(reopened.complete(group).succeeded());
    EXPECT_EQ(ex::read(reopened.storage().physical_trade(9)).lifecycle_state, "REJECTED");
    EXPECT_EQ(ex::read(reopened.storage().physical_trade(10)).lifecycle_state, "SETTLED");
}

TEST(TradingSystemBatch6, DurableRiskScopeIdentityRejectsMixedKinds) {
    Database db;
    {
        ts::TradingSystemStorage storage(db.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.put_reservation(scope_reservation(1), 0).kind,
                  ts::StorageResultKind::APPLIED);
        EXPECT_EQ(unit.put_reservation(scope_reservation(2, ts::RiskScopeKind::STRATEGY), 0).kind,
                  ts::StorageResultKind::INVALID_STATE);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    ts::TradingSystemStorage reopened(db.config());
    EXPECT_EQ(reopened.reservation(1).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(reopened.reservation(2).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_TRUE(reopened.admission_scope_index_verified());
    // The aborted claim must not prevent a subsequent valid binding.
    auto unit = reopened.begin();
    ASSERT_TRUE(unit.put_reservation(scope_reservation(2, ts::RiskScopeKind::STRATEGY), 0).succeeded());
    EXPECT_TRUE(unit.commit().succeeded());
}

TEST(TradingSystemBatch6, TerminalScopeIdentitySurvivesReopenAndRejectsOtherKinds) {
    for (const auto state : {ts::RiskReservationState::SETTLED, ts::RiskReservationState::RELEASED}) {
        Database db;
        {
            ts::TradingSystemStorage storage(db.config());
            auto unit = storage.begin();
            auto history = scope_reservation(1);
            history.state = state;
            ASSERT_TRUE(unit.put_reservation(history, 0).succeeded());
            ASSERT_TRUE(unit.commit().succeeded());
        }
        ts::TradingSystemStorage reopened(db.config());
        ASSERT_TRUE(reopened.admission_scope_index_verified());
        auto unit = reopened.begin();
        ASSERT_TRUE(unit.put_reservation(scope_reservation(3), 0).succeeded());
        EXPECT_EQ(unit.put_reservation(scope_reservation(2, ts::RiskScopeKind::GROUP), 0).kind,
                  ts::StorageResultKind::INVALID_STATE);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
        EXPECT_EQ(reopened.reservation(3).kind, ts::StorageResultKind::NOT_FOUND);
        EXPECT_EQ(reopened.reservations().values.size(), 1U);
    }
}

TEST(TradingSystemBatch6, CorruptScopeIdentityAbortsEarlierWritesAndCanBeRebuilt) {
    using Kind = ts::StorageResultKind;
    const std::vector<std::pair<std::string, Kind>> corruptions{
        {"not-json", Kind::STORAGE_ERROR}, {"{}", Kind::STORAGE_ERROR},
        {R"({"schema_version":2,"risk_scope_kind":0})", Kind::UNSUPPORTED_SCHEMA},
        {R"({"schema_version":1,"risk_scope_kind":999})", Kind::STORAGE_ERROR},
        {R"({"schema_version":1,"risk_scope_kind":0.5})", Kind::STORAGE_ERROR},
        {R"({"schema_version":1,"risk_scope_kind":"ACCOUNT"})", Kind::STORAGE_ERROR}
    };
    for (const auto& corruption : corruptions) {
        SCOPED_TRACE(corruption.first);
        Database db;
        {
            ts::TradingSystemStorage storage(db.config());
            {
                auto unit = storage.begin();
                ASSERT_TRUE(unit.put_reservation(scope_reservation(1), 0).succeeded());
                ASSERT_TRUE(unit.commit().succeeded());
            }
            // Deliberately corrupt a quiescent projection after its startup audit.
            mdbxc::KeyValueTable<std::string, std::string> index(storage.connection(), "ts_admission_scope_index");
            index.insert_or_assign(ts::detail::risk_scope_identity_key(42), corruption.first);
            auto unit = storage.begin();
            ASSERT_TRUE(unit.put_reservation(scope_reservation(3, ts::RiskScopeKind::ACCOUNT, 43), 0).succeeded());
            EXPECT_EQ(unit.put_reservation(scope_reservation(2), 0).kind, corruption.second);
            EXPECT_EQ(unit.commit().kind, Kind::INVALID_STATE);
        }
        {
            ts::TradingSystemStorage reopened(db.config());
            EXPECT_FALSE(reopened.admission_scope_index_verified());
            EXPECT_EQ(reopened.reservation(2).kind, Kind::NOT_FOUND);
            EXPECT_EQ(reopened.reservation(3).kind, Kind::NOT_FOUND);
            auto unit = reopened.begin();
            ASSERT_TRUE(unit.rebuild_admission_scope_index().succeeded());
            ASSERT_TRUE(unit.commit().succeeded());
        }
        ts::TradingSystemStorage repaired(db.config());
        EXPECT_TRUE(repaired.admission_scope_index_verified());
    }
}

TEST(TradingSystemBatch6, FailedScopeIdentityRebuildCannotCommitPartialProjection) {
    for (const bool malformed : {false, true}) {
        Database db;
        std::vector<std::pair<std::string, std::string>> original;
        {
            ts::TradingSystemStorage storage(db.config());
            {
                auto unit = storage.begin();
                ASSERT_TRUE(unit.put_reservation(scope_reservation(1), 0).succeeded());
                ASSERT_TRUE(unit.commit().succeeded());
            }
            mdbxc::KeyValueTable<std::string, std::string> index(storage.connection(), "ts_admission_scope_index");
            index.load(original);
            mdbxc::KeyValueTable<std::string, std::string> reservations(storage.connection(), "ts_reservations");
            auto bad = ts::detail::reservation_to_json(scope_reservation(2, ts::RiskScopeKind::STRATEGY));
            if (malformed) bad["risk_scope_kind"] = 999;
            reservations.insert_or_assign("2", bad.dump());
            auto unit = storage.begin();
            ASSERT_TRUE(unit.put_settlement(stored_settlement(900)).succeeded());
            const auto failed = unit.rebuild_admission_scope_index();
            EXPECT_EQ(failed.kind, malformed ? ts::StorageResultKind::STORAGE_ERROR
                                            : ts::StorageResultKind::INVALID_STATE);
            EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
        }
        ts::TradingSystemStorage reopened(db.config());
        EXPECT_FALSE(reopened.admission_scope_index_verified());
        EXPECT_EQ(reopened.settlement(900).kind, ts::StorageResultKind::NOT_FOUND);
        mdbxc::KeyValueTable<std::string, std::string> index(reopened.connection(), "ts_admission_scope_index");
        std::vector<std::pair<std::string, std::string>> after;
        index.load(after);
        EXPECT_EQ(after, original);
    }
}

TEST(TradingSystemBatch6, PreviousScopeProjectionVersionRequiresExplicitRebuild) {
    Database db;
    {
        ts::TradingSystemStorage storage(db.config());
        {
            auto unit = storage.begin();
            ASSERT_TRUE(unit.put_reservation(scope_reservation(1), 0).succeeded());
            ASSERT_TRUE(unit.commit().succeeded());
        }
        mdbxc::KeyValueTable<std::string, std::string> index(storage.connection(), "ts_admission_scope_index");
        index.erase(ts::detail::risk_scope_identity_key(42));
        index.insert_or_assign("@schema", R"({"schema_version":1})");
        auto unit = storage.begin();
        EXPECT_EQ(unit.put_reservation(scope_reservation(2), 0).kind, ts::StorageResultKind::UNSUPPORTED_SCHEMA);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    {
        ts::TradingSystemStorage reopened(db.config());
        EXPECT_FALSE(reopened.admission_scope_index_verified());
        {
            auto unit = reopened.begin();
            EXPECT_EQ(unit.put_reservation(scope_reservation(2), 0).kind, ts::StorageResultKind::INVALID_STATE);
            EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
        }
        auto unit = reopened.begin();
        ASSERT_TRUE(unit.rebuild_admission_scope_index().succeeded());
        EXPECT_EQ(unit.reservation_history_visits(), 1U);
        ASSERT_TRUE(unit.commit().succeeded());
    }
    ts::TradingSystemStorage upgraded(db.config());
    EXPECT_TRUE(upgraded.admission_scope_index_verified());
    auto unit = upgraded.begin();
    EXPECT_EQ(unit.put_reservation(scope_reservation(2, ts::RiskScopeKind::STRATEGY), 0).kind,
              ts::StorageResultKind::INVALID_STATE);
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
}

TEST(TradingSystemBatch6, MissingTerminalScopeClaimFailsStartupAuditDespiteCurrentMarker) {
    Database db;
    {
        ts::TradingSystemStorage storage(db.config());
        {
            auto unit = storage.begin();
            auto history = scope_reservation(1);
            history.state = ts::RiskReservationState::SETTLED;
            ASSERT_TRUE(unit.put_reservation(history, 0).succeeded());
            ASSERT_TRUE(unit.commit().succeeded());
        }
        mdbxc::KeyValueTable<std::string, std::string> index(storage.connection(), "ts_admission_scope_index");
        index.erase(ts::detail::risk_scope_identity_key(42));
    }
    ts::TradingSystemStorage reopened(db.config());
    EXPECT_FALSE(reopened.admission_scope_index_verified());
    {
        auto unit = reopened.begin();
        EXPECT_EQ(unit.put_reservation(scope_reservation(2, ts::RiskScopeKind::STRATEGY), 0).kind,
                  ts::StorageResultKind::INVALID_STATE);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    auto unit = reopened.begin();
    ASSERT_TRUE(unit.rebuild_admission_scope_index().succeeded());
    ASSERT_TRUE(unit.commit().succeeded());
    EXPECT_TRUE(reopened.admission_scope_index_verified());
}

TEST(TradingSystemBatch6, OrdinaryReservationWritesNeverVisitCompletedHistory) {
    for (const std::uint64_t count : {32U, 2048U}) {
        SCOPED_TRACE(count);
        Database db;
        {
            ts::TradingSystemStorage storage(db.config());
            auto unit = storage.begin();
            for (std::uint64_t id = 1; id <= count; ++id) {
                auto history = scope_reservation(id, ts::RiskScopeKind::ACCOUNT, id + 1000);
                history.state = ts::RiskReservationState::SETTLED;
                ASSERT_TRUE(unit.put_reservation(history, 0).succeeded());
            }
            EXPECT_EQ(unit.reservation_history_visits(), 0U);
            EXPECT_EQ(unit.risk_scope_identity_checks(), count);
            ASSERT_TRUE(unit.commit().succeeded());
        }
        ts::TradingSystemStorage reopened(db.config());
        ASSERT_TRUE(reopened.admission_scope_index_verified());
        // Every write gets a fresh UOW, including an update to completed history.
        for (const auto id : {count + 1, std::uint64_t{1}}) {
            auto value = scope_reservation(id, ts::RiskScopeKind::ACCOUNT, id + 1000);
            value.state = ts::RiskReservationState::SETTLED;
            if (id == 1) ++value.state_revision;
            auto unit = reopened.begin();
            ASSERT_TRUE(unit.put_reservation(value, 0).succeeded());
            EXPECT_EQ(unit.reservation_history_visits(), 0U);
            EXPECT_EQ(unit.risk_scope_identity_checks(), 1U);
            EXPECT_EQ(unit.admission_scope_index_visits(), 0U);
            ASSERT_TRUE(unit.commit().succeeded());
        }
        auto rebuild = reopened.begin();
        ASSERT_TRUE(rebuild.rebuild_admission_scope_index().succeeded());
        EXPECT_EQ(rebuild.reservation_history_visits(), count + 1);
        EXPECT_EQ(rebuild.risk_scope_identity_checks(), count + 1);
        ASSERT_TRUE(rebuild.commit().succeeded());
    }
}

TEST(TradingSystemBatch6, SimulatedBrokerRejectsForeignHistoryIdentity) {
    Broker adapter;
    Broker::HistoryEntry foreign;
    foreign.operation_id = "operation-1";
    foreign.client_operation_id = "client-1";
    foreign.account_id = 11;
    foreign.trade_id = 7;
    foreign.intent_id = 99;
    foreign.execution_group_id = 90;
    adapter.add_history_entry(foreign);

    ts::TradeSettlement settlement;
    settlement.settlement_id = 1;
    settlement.intent_id = 1;
    settlement.execution_group_id = 80;
    settlement.trade_id = 7;
    settlement.kind = ts::OutcomeKind::WIN;
    settlement.evidence = ts::ExecutionEvidence::EXECUTED;
    settlement.net_profit = ex::usd(1);
    EXPECT_FALSE(adapter.attach_settlement("operation-1", settlement));

    ts::ExecutionOperationRecord operation;
    operation.operation_id = "operation-1";
    operation.client_operation_id = "client-1";
    operation.intent_id = 1;
    operation.group_id = 80;
    operation.trade_id = 7;
    operation.account_id = 11;
    operation.risk_scope_id = 1000;
    EXPECT_EQ(adapter.reconcile(operation).kind, ts::ReconciliationKind::EXECUTION_UNKNOWN);
}

TEST(TradingSystemBatch6, CompletionUsesTerminalEvidenceAsDurableAnchor) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ts::ExecutionGroupId group;
    ts::SettlementId terminal_id;
    std::uint64_t series_revision, mm_revision, physical_revision, reservation_revision;
    std::uint64_t trade_id, reservation_id;
    {
        ex::SyntheticHost host(db.config(), configuration(), noon, broker);
        ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
        group = host.series().current_group_id;
        ASSERT_TRUE(host.submit(group).succeeded());
        const auto operation = host.operations(group).front();
        trade_id = operation.trade_id;
        reservation_id = ex::read(host.storage().roster(group)).reservation_bindings.front().reservation_id;

        ts::TradeSettlement unknown;
        unknown.settlement_id = 1;
        unknown.intent_id = operation.intent_id;
        unknown.execution_group_id = group;
        unknown.trade_id = trade_id;
        unknown.kind = ts::OutcomeKind::UNKNOWN;
        unknown.evidence = ts::ExecutionEvidence::MAY_HAVE_EXECUTED;
        const auto physical = ex::read(host.storage().physical_trade(trade_id));
        {
            auto unit = host.storage().begin();
            const auto event_id = unit.next_journal_event_id_for_append();
            ASSERT_EQ(event_id.kind, ts::StorageResultKind::APPLIED);
            ts::SettlementTransition transition;
            transition.journal = {event_id.value, "EXECUTION_SETTLED", "trade:" + std::to_string(trade_id),
                0, ts::trading_storage_schema_version, "settlement:1", noon, {}};
            transition.settlement = {unknown, ex::usd(0), std::nullopt, true,
                                     ts::trading_storage_schema_version};
            transition.physical_trade = physical;
            transition.physical_trade->lifecycle_state = "EXECUTION_UNKNOWN";
            ++transition.physical_trade->state_revision;
            transition.expected_physical_trade_revision = physical.state_revision;
            const auto recorded = unit.record_physical_settlement(transition);
            ASSERT_TRUE(recorded.succeeded()) << recorded.message;
            ASSERT_TRUE(unit.commit().succeeded());
        }

        const auto terminal = host.broker_result(group, 0, 80);
        terminal_id = terminal.settlement_id;
        ASSERT_TRUE(host.observe(group).succeeded());
        auto records = host.storage().settlements_for_group(group);
        ASSERT_TRUE(records.succeeded());
        ASSERT_EQ(records.values.size(), 2U);
        std::stable_sort(records.values.begin(), records.values.end(),
            [](const ts::SettlementRecord& lhs, const ts::SettlementRecord& rhs) {
                return ts::canonical_settlement_before(lhs.settlement, rhs.settlement);
            });
        EXPECT_EQ(records.values[0].settlement.settlement_id, unknown.settlement_id);
        EXPECT_FALSE(records.values[0].settlement.terminal());
        EXPECT_EQ(records.values[0].settlement.kind, ts::OutcomeKind::UNKNOWN);
        EXPECT_EQ(records.values[1].settlement.settlement_id, terminal_id);
        EXPECT_TRUE(records.values[1].settlement.terminal());
        EXPECT_LT(unknown.settlement_id, terminal_id);
        ASSERT_TRUE(host.complete(group).succeeded());
        EXPECT_EQ(host.mm_state().last_settlement_id, terminal_id);
        const auto completions = host.storage().journal_events("GROUP_COMPLETION");
        ASSERT_EQ(completions.values.size(), 1U);
        EXPECT_EQ(completions.values.front().idempotency_key,
                  "settlement:" + std::to_string(terminal_id) + ":logical");
        series_revision = host.series().series.state_revision;
        mm_revision = host.mm_state().state_revision;
        physical_revision = ex::read(host.storage().physical_trade(trade_id)).state_revision;
        reservation_revision = ex::read(host.storage().reservation(reservation_id)).state_revision;
    }
    ex::SyntheticHost reopened(db.config(), configuration(), noon, broker);
    EXPECT_EQ(reopened.storage().settlements_for_group(group).values.size(), 2U);
    EXPECT_EQ(reopened.complete(group).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(reopened.series().series.state_revision, series_revision);
    EXPECT_EQ(reopened.mm_state().state_revision, mm_revision);
    EXPECT_EQ(reopened.mm_state().last_settlement_id, terminal_id);
    EXPECT_EQ(ex::read(reopened.storage().physical_trade(trade_id)).state_revision, physical_revision);
    EXPECT_EQ(ex::read(reopened.storage().reservation(reservation_id)).state_revision, reservation_revision);
    EXPECT_EQ(reopened.storage().journal_events("GROUP_COMPLETION").values.size(), 1U);
    EXPECT_EQ(submissions(broker), 1U);
}

TEST(TradingSystemBatch6, MixedScopeStatesHydrateConservativelyAfterRestart) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 1000}, {13, 1000}};
    auto broker = std::make_shared<Broker::State>();
    ts::ExecutionGroupId group;
    {
        ex::SyntheticHost host(db.config(), config, noon, broker);
        ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
        group = host.series().current_group_id;
        ASSERT_TRUE(host.submit_plans(group, {
            ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION,
            ts::SimulatedSubmissionPlan::SUCCESS,
            ts::SimulatedSubmissionPlan::ACCEPTED_RESPONSE_LOST}).succeeded());
        host.broker_result(group, 1, 80);
        ASSERT_TRUE(host.observe(group).succeeded());
    }
    std::size_t submissions_before_reopen = 0;
    {
        ex::SyntheticHost reopened(db.config(), config, noon, broker);
        EXPECT_EQ(reopened.reserved(1000), 200);
        EXPECT_EQ(reopened.series().status, ts::SeriesStatus::WAITING_OUTCOME);
        const auto durable = reopened.storage().reservations();
        ASSERT_EQ(durable.values.size(), 3U);
        EXPECT_EQ(durable.values[0].state, ts::RiskReservationState::RELEASED);
        EXPECT_EQ(durable.values[1].state, ts::RiskReservationState::RESERVED);
        EXPECT_EQ(durable.values[2].state, ts::RiskReservationState::RESERVED);
        reopened.broker_result(group, 2, 80);
        ASSERT_TRUE(reopened.observe(group).succeeded());
        ASSERT_TRUE(reopened.complete(group).succeeded());
        EXPECT_EQ(reopened.reserved(1000), 0);
        EXPECT_EQ(reopened.series().series.next_step, 1U);
        EXPECT_EQ(reopened.mm_state().state_revision, 1U);
        const auto completed = reopened.storage().reservations();
        ASSERT_EQ(completed.values.size(), 3U);
        EXPECT_EQ(completed.values[0].state, ts::RiskReservationState::RELEASED);
        EXPECT_EQ(completed.values[1].state, ts::RiskReservationState::SETTLED);
        EXPECT_EQ(completed.values[2].state, ts::RiskReservationState::SETTLED);
        submissions_before_reopen = submissions(broker);
        EXPECT_EQ(submissions_before_reopen, 3U);
    }
    {
        ex::SyntheticHost reopened_again(db.config(), config, noon, broker);
        EXPECT_EQ(reopened_again.reserved(1000), 0);
        EXPECT_EQ(reopened_again.mm_state().state_revision, 1U);
        EXPECT_EQ(reopened_again.complete(group).kind, ts::StorageResultKind::DUPLICATE);
        EXPECT_EQ(reopened_again.mm_state().state_revision, 1U);
        EXPECT_EQ(submissions(broker), submissions_before_reopen);
    }
}

TEST(TradingSystemBatch6, LossStepPolicyRejectsInvalidMultiplierAndOverflow) {
    ts::TradingDecisionContext context;
    for (const auto multiplier : {-1, 0, 1}) {
        EXPECT_EQ(ts::LossStepStake(ex::usd(100), multiplier).evaluate(context, false).reason,
                  ts::SizingReason::INVALID_CONFIGURATION);
        EXPECT_FALSE(ts::LossStepStake(ex::usd(100), multiplier).evaluate(context, true).amount);
    }
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    EXPECT_EQ(ts::LossStepStake(ex::usd(maximum), 2).evaluate(context, true).reason,
              ts::SizingReason::NUMERIC_OVERFLOW);
    EXPECT_EQ(ts::LossStepStake(ex::usd(maximum), 2).evaluate(context, false).amount->units, maximum);
    EXPECT_EQ(ts::LossStepStake(ex::usd(maximum / 2), 2).evaluate(context, true).amount->units,
              (maximum / 2) * 2);
    EXPECT_FALSE(ts::LossStepStake(ex::usd(0), 2).evaluate(context, true).amount);
}

enum class CrashBoundary { PREPARED, RESPONSE_LOST, PHYSICAL_SETTLED, LOGICAL_COMMITTED };
class SyntheticRestart : public testing::TestWithParam<CrashBoundary> {};

TEST_P(SyntheticRestart, ReopenHydratesNewOwnersAndNeverResendsPossibleExecution) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ts::ExecutionGroupId group = 0;
    std::uint64_t journal_revision = 0;
    std::string operation_id;
    {
        ex::SyntheticHost host(db.config(), configuration(), noon, broker);
        ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
        group = host.series().current_group_id;
        operation_id = host.operations(group).front().operation_id;
        if (GetParam() != CrashBoundary::PREPARED) {
            ASSERT_TRUE(host.submit(group, ts::SimulatedSubmissionPlan::ACCEPTED_RESPONSE_LOST).succeeded());
            host.broker_result(group, 0, 80);
        }
        if (GetParam() == CrashBoundary::PHYSICAL_SETTLED || GetParam() == CrashBoundary::LOGICAL_COMMITTED) {
            ASSERT_TRUE(host.observe(group).succeeded());
        }
        if (GetParam() == CrashBoundary::LOGICAL_COMMITTED) ASSERT_TRUE(host.complete(group).succeeded());
        journal_revision = host.operations(group).front().state_revision;
    } // Every OptionX owner, manager and MDBX connection is destroyed here.
    ex::SyntheticHost host(db.config(), configuration(), noon, broker);
    ASSERT_TRUE(host.storage().admission_scope_index_verified());
    auto operation = host.operations(group).front();
    EXPECT_EQ(operation.operation_id, operation_id);
    EXPECT_EQ(operation.account_id, 11U);
    EXPECT_EQ(operation.state_revision, journal_revision);
    const auto roster = ex::read(host.storage().roster(group));
    EXPECT_EQ(roster.members.front().trade_id, operation.trade_id);
    EXPECT_EQ(roster.members.front().account_id, operation.account_id);
    EXPECT_EQ(host.reserved(1000), GetParam() == CrashBoundary::LOGICAL_COMMITTED ? 0 : 100);
    if (GetParam() == CrashBoundary::PREPARED) {
        EXPECT_EQ(submissions(broker), 0U);
        ASSERT_TRUE(host.submit(group).succeeded());
        host.broker_result(group, 0, 80);
    } else {
        EXPECT_FALSE(host.submit(group).succeeded());
        EXPECT_EQ(submissions(broker), 1U);
    }
    ASSERT_TRUE(host.observe(group).succeeded());
    const auto completed = host.complete(group);
    ASSERT_TRUE(completed.succeeded()) << completed.message;
    EXPECT_EQ(host.series().series.next_step, 1U);
    EXPECT_EQ(host.series().series.state_revision, 2U);
    EXPECT_EQ(host.mm_state().state_revision, 1U);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(host.storage().settlements_for_group(group).values.size(), 1U);
    EXPECT_EQ(host.complete(group).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(host.storage().journal_events("GROUP_COMPLETION").values.size(), 1U);
    EXPECT_EQ(submissions(broker), 1U);
    EXPECT_GE(host.operations(group).front().state_revision, journal_revision);
}

INSTANTIATE_TEST_SUITE_P(CrashBoundaries, SyntheticRestart, testing::Values(
    CrashBoundary::PREPARED, CrashBoundary::RESPONSE_LOST,
    CrashBoundary::PHYSICAL_SETTLED, CrashBoundary::LOGICAL_COMMITTED));

TEST(TradingSystemBatch6, MultiAccountSharedScopeWaitsForLateMemberAcrossRestart) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 1000}, {13, 2000}};
    config.accounts[2000] = config.accounts[1000];
    auto broker = std::make_shared<Broker::State>();
    ts::ExecutionGroupId group;
    {
        ex::SyntheticHost host(db.config(), config, noon, broker);
        ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
        group = host.series().current_group_id;
        EXPECT_EQ(host.reserved(1000), 200);
        EXPECT_EQ(host.reserved(2000), 100);
        ASSERT_TRUE(host.submit(group).succeeded());
        host.broker_result(group, 0, 80);
        host.broker_result(group, 1, -100);
        ASSERT_TRUE(host.observe(group).succeeded());
        EXPECT_FALSE(host.complete(group).succeeded());
        EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
        EXPECT_EQ(host.series().series.next_step, 0U);
        EXPECT_EQ(host.mm_state().state_revision, 0U);
        EXPECT_EQ(host.reserved(1000), 200);
    }
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_EQ(host.reserved(1000), 200);
    EXPECT_EQ(host.reserved(2000), 100);
    host.broker_result(group, 2, 80);
    ASSERT_TRUE(host.observe(group).succeeded());
    const auto result = host.complete(group);
    ASSERT_TRUE(result.succeeded()) << result.message;
    EXPECT_EQ(host.series().series.next_step, 1U);
    EXPECT_EQ(host.series().series.state_revision, 2U);
    EXPECT_EQ(host.series().current_outcome.net_profit->units, 60);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(host.reserved(2000), 0);
    EXPECT_EQ(host.complete(group).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(submissions(broker), 3U);
    for (const auto& reservation : host.storage().reservations().values) {
        EXPECT_EQ(reservation.state, ts::RiskReservationState::SETTLED);
        EXPECT_EQ(reservation.state_revision, 1U);
    }
    const auto roster = ex::read(host.storage().roster(group));
    for (const auto& member : roster.members) {
        EXPECT_EQ(ex::read(host.storage().physical_trade(member.trade_id)).account_id, member.account_id);
    }
}

TEST(TradingSystemBatch6, SharedScopeBudgetIsSumNotOneMemberStake) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 1000}};
    config.accounts[1000].available_balance = ex::usd(150);
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, ChangedDurablePolicyOrAccountBindingFailsClosed) {
    Database db;
    auto config = configuration();
    auto broker = std::make_shared<Broker::State>();
    { ex::SyntheticHost host(db.config(), config, noon, broker); }
    auto changed = config;
    changed.loss_multiplier = 3;
    EXPECT_THROW(ex::SyntheticHost(db.config(), changed, noon, broker), std::runtime_error);
    changed = config;
    changed.targets.front().account_id = 99;
    EXPECT_THROW(ex::SyntheticHost(db.config(), changed, noon, broker), std::runtime_error);
    EXPECT_EQ(submissions(broker), 0U);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
