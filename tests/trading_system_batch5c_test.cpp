#include <gtest/gtest.h>
#include <optionx_cpp/trading_system.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace ts = optionx::trading_system;
using optionx::CurrencyType;

struct TestDatabase {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("optionx_batch5c_" + std::to_string(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()) + ".mdbx");

    ~TestDatabase() { std::error_code error; std::filesystem::remove(path, error); }

    mdbxc::Config config() const {
        mdbxc::Config result;
        result.pathname = path.string();
        result.max_dbs = 16;
        result.no_subdir = true;
        return result;
    }
};

ts::MoneyAmount money(std::int64_t units) { return {units, 2, CurrencyType::USD}; }

ts::TradeIntent intent() {
    ts::TradeIntent result;
    result.intent_id = 41;
    result.execution_group = {80, 1};
    result.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
    result.status = ts::IntentStatus::ADMITTED;
    result.idempotency_key = "intent-41";
    return result;
}

ts::ExecutionRosterRecord roster() {
    return {80, 41, 1,
            {{700, 11, 1000, money(50)}},
            {{700, 500, 1000, money(50), money(50), "risk-41"}},
            ts::trading_storage_schema_version};
}

ts::ReservationRecord reservation() {
    return {500, 1000, ts::RiskScopeKind::ACCOUNT, 41, {80, 1},
            ts::ExecutionCardinality::EXACTLY_ONE, money(50),
            ts::RiskReservationState::RESERVED, 0, 7, money(50), "risk-41",
            ts::trading_storage_schema_version};
}

ts::PhysicalTradeRecord physical_trade() {
    return {700, 41, 80, 11, 1000, "ADMITTED", {}, 0,
            ts::trading_storage_schema_version};
}

ts::ExecutionOperationRecord operation_from(
    const ts::TradingSystemStorage& storage) {
    const auto operations = ts::ExecutionRecoveryManager::recover(storage).operations;
    EXPECT_EQ(operations.size(), 1U);
    return operations.begin()->second;
}

ts::TradeSettlement terminal_settlement() {
    ts::TradeSettlement result;
    result.settlement_id = 900;
    result.intent_id = 41;
    result.execution_group_id = 80;
    result.trade_id = 700;
    result.kind = ts::OutcomeKind::WIN;
    result.evidence = ts::ExecutionEvidence::EXECUTED;
    result.net_profit = money(100);
    return result;
}

ts::ExecutionOperationRecord operation_for_trade(
    const ts::TradingSystemStorage& storage, std::uint64_t trade_id) {
    const auto operations = ts::ExecutionRecoveryManager::recover(storage).operations;
    const auto found = std::find_if(
        operations.begin(), operations.end(),
        [trade_id](const auto& item) { return item.second.trade_id == trade_id; });
    EXPECT_NE(found, operations.end());
    return found->second;
}

ts::TradeSettlement group_evidence(
    std::uint64_t settlement_id, std::uint64_t trade_id,
    ts::OutcomeKind kind, ts::ExecutionEvidence evidence,
    std::int64_t profit = 0) {
    ts::TradeSettlement result;
    result.settlement_id = settlement_id;
    result.intent_id = 41;
    result.execution_group_id = 80;
    result.trade_id = trade_id;
    result.kind = kind;
    result.evidence = evidence;
    if (kind == ts::OutcomeKind::WIN || kind == ts::OutcomeKind::LOSS ||
        kind == ts::OutcomeKind::DRAW) {
        result.net_profit = money(profit);
    }
    return result;
}

struct AuthoritativeOwners {
    std::vector<std::unique_ptr<ts::RiskAdmission>> owners;
    std::vector<const ts::RiskAdmission*> references;
    bool valid = true;
    std::string message;
};

AuthoritativeOwners authoritative_owners(
    const std::vector<ts::ReservationRecord>& reservations,
    std::int64_t captured_at_ms = 1000) {
    AuthoritativeOwners result;
    result.references.resize(reservations.size());
    using ScopeKey = std::pair<int, std::uint64_t>;
    std::map<ScopeKey, std::vector<std::size_t>> by_scope;
    for (std::size_t index = 0; index < reservations.size(); ++index) {
        by_scope[{static_cast<int>(reservations[index].risk_scope_kind),
                  reservations[index].risk_scope_id}].push_back(index);
    }

    for (const auto& scope : by_scope) {
        const auto& indexes = scope.second;
        const auto& first = reservations[indexes.front()];
        std::int64_t total_risk = 0;
        std::int64_t total_stake = 0;
        std::uint64_t minimum_id = (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t maximum_id = 0;
        for (const auto index : indexes) {
            const auto& reservation = reservations[index];
            if (reservation.potential_risk.currency != first.potential_risk.currency ||
                reservation.potential_risk.precision != first.potential_risk.precision ||
                reservation.potential_risk.units <= 0 ||
                total_risk > (std::numeric_limits<std::int64_t>::max)() -
                    reservation.potential_risk.units) {
                result.valid = false;
                result.message = "test reservations have incompatible risk metadata";
                return result;
            }
            if (reservation.stake.currency != first.stake.currency ||
                reservation.stake.precision != first.stake.precision ||
                reservation.stake.units <= 0 ||
                total_stake > (std::numeric_limits<std::int64_t>::max)() -
                    reservation.stake.units) {
                result.valid = false;
                result.message = "test reservations have incompatible stake metadata";
                return result;
            }
            total_risk += reservation.potential_risk.units;
            total_stake += reservation.stake.units;
            minimum_id = (std::min)(minimum_id, reservation.reservation_id);
            maximum_id = (std::max)(maximum_id, reservation.reservation_id);
        }

        auto token = first.to_risk_reservation();
        token.reservation_id = minimum_id;
        token.idempotency_key = "intent-" + std::to_string(first.intent_id);
        token.stake.units = total_stake;
        token.potential_risk.units = total_risk;
        const std::vector<ts::RiskReservation> tokens{token};

        ts::RiskAdmissionConfig config;
        config.scope = {first.risk_scope_kind, first.risk_scope_id};
        config.max_snapshot_age_ms = 60'000;
        config.max_reserved_risk = first.potential_risk;
        config.max_reserved_risk->units = total_risk;
        auto owner = std::make_unique<ts::RiskAdmission>(config);
        ts::RiskAccountSnapshot snapshot;
        snapshot.availability = ts::RiskAvailability::SUPPORTED;
        snapshot.available_balance = first.potential_risk;
        snapshot.available_balance.units = total_risk;
        snapshot.captured_at_ms = captured_at_ms;
        snapshot.revision = 7;
        const auto next_id = maximum_id == (std::numeric_limits<std::uint64_t>::max)()
            ? 0 : maximum_id + 1;
        const auto hydrated = owner->hydrate(snapshot, tokens, next_id);
        if (hydrated.kind != ts::RiskAdmissionKind::APPLIED) {
            result.valid = false;
            result.message = "test RiskAdmission hydration failed";
            return result;
        }
        const auto owner_ptr = owner.get();
        result.owners.push_back(std::move(owner));
        for (const auto index : indexes) result.references[index] = owner_ptr;
    }
    return result;
}

void prepare(ts::TradingSystemStorage& storage) {
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    const std::vector<ts::ReservationRecord> reservations{reservation()};
    auto owners = authoritative_owners(reservations);
    ASSERT_TRUE(owners.valid) << owners.message;
    ASSERT_EQ(manager.prepare(storage, intent(), roster(), reservations,
                              {physical_trade()}, owners.references).kind,
              ts::StorageResultKind::APPLIED);
}

void record_physical_evidence(
    ts::TradingSystemStorage& storage, const ts::TradeSettlement& settlement) {
    ASSERT_EQ(storage.journal_by_key(
                  "settlement:" + std::to_string(settlement.settlement_id)).kind,
              ts::StorageResultKind::NOT_FOUND);
    ASSERT_EQ(storage.settlement(settlement.settlement_id).kind,
              ts::StorageResultKind::NOT_FOUND);
    const auto physical = storage.physical_trade(settlement.trade_id);
    ASSERT_TRUE(physical.value);
    auto unit = storage.begin();
    const auto event_id = unit.next_journal_event_id_for_append();
    ASSERT_EQ(event_id.kind, ts::StorageResultKind::APPLIED);
    ts::SettlementTransition transition;
    transition.journal = {event_id.value, "EXECUTION_SETTLED",
                          "trade:" + std::to_string(settlement.trade_id), 0,
                          ts::trading_storage_schema_version,
                          "settlement:" + std::to_string(settlement.settlement_id),
                          1000, {}};
    transition.settlement = {settlement, money(0), std::nullopt, true,
                             ts::trading_storage_schema_version};
    transition.physical_trade = *physical.value;
    transition.physical_trade->lifecycle_state = "EXECUTION_UNKNOWN";
    ++transition.physical_trade->state_revision;
    transition.expected_physical_trade_revision = physical.value->state_revision;
    const auto recorded = unit.record_physical_settlement(transition);
    ASSERT_EQ(recorded.kind, ts::StorageResultKind::APPLIED) << recorded.message;
    ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
}

TEST(TradingSystemBatch5C, PreparationRequiresAuthoritativeRiskAdmission) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::TradingSystemStorage storage(database.config());
    const std::vector<ts::ReservationRecord> reservations{reservation()};
    const std::vector<ts::PhysicalTradeRecord> physical{physical_trade()};

    EXPECT_EQ(manager.prepare(storage, intent(), roster(), reservations, physical).kind,
              ts::StorageResultKind::INVALID_STATE);

    auto owners = authoritative_owners(reservations);
    ASSERT_TRUE(owners.valid) << owners.message;
    auto forged_reservation = reservations.front();
    forged_reservation.potential_risk = money(75);
    auto forged_roster = roster();
    forged_roster.reservation_bindings.front().potential_risk = money(75);
    TestDatabase forged_database;
    ts::TradingSystemStorage forged_storage(forged_database.config());
    EXPECT_EQ(manager.prepare(
        forged_storage, intent(), forged_roster,
        {forged_reservation}, physical, owners.references).kind,
        ts::StorageResultKind::INVALID_STATE);
}

TEST(TradingSystemBatch5C, PreparationSurvivesRestartAndJsonRoundTrip) {
    TestDatabase database;
    {
        ts::TradingSystemStorage storage(database.config());
        prepare(storage);
    }
    ts::TradingSystemStorage reopened(database.config());
    const auto operation = operation_from(reopened);
    EXPECT_EQ(operation.state, ts::ExecutionOperationState::PREPARED);
    EXPECT_EQ(operation.state_revision, 0U);
    EXPECT_EQ(nlohmann::json(operation).get<ts::ExecutionOperationRecord>().operation_id,
              operation.operation_id);
}

TEST(TradingSystemBatch5C, UnknownExecutionCannotBeResubmitted) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::SimulatedExecutionAdapter adapter;
    {
        ts::TradingSystemStorage storage(database.config());
        prepare(storage);
        auto operation = operation_from(storage);
        adapter.set_plan(operation.operation_id, ts::SimulatedSubmissionPlan::TIMEOUT);
        EXPECT_EQ(manager.submit(storage, operation, adapter).kind,
                  ts::StorageResultKind::APPLIED);
        EXPECT_EQ(operation_from(storage).state, ts::ExecutionOperationState::EXECUTION_UNKNOWN);
        EXPECT_EQ(manager.submit(storage, operation_from(storage), adapter).kind,
                  ts::StorageResultKind::INVALID_STATE);
        EXPECT_EQ(adapter.submission_count(operation.operation_id), 1U);
    }
}

TEST(TradingSystemBatch5C, RestartKeepsBrokerSideEffectOutsideOptionxLifetime) {
    TestDatabase database;
    auto broker_state = std::make_shared<ts::SimulatedExecutionAdapter::State>();
    std::string operation_id;
    {
        ts::FakeExecutionClock clock(1000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::TradingSystemStorage storage(database.config());
        prepare(storage);
        const auto operation = operation_from(storage);
        operation_id = operation.operation_id;
        ts::SimulatedExecutionAdapter adapter(broker_state);
        adapter.set_plan(operation_id, ts::SimulatedSubmissionPlan::ACCEPTED_RESPONSE_LOST);
        ASSERT_EQ(manager.submit(storage, operation, adapter).kind,
                  ts::StorageResultKind::APPLIED);
        EXPECT_EQ(broker_state->history.size(), 1U);
    }
    {
        ts::FakeExecutionClock clock(2000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::TradingSystemStorage reopened(database.config());
        ts::SimulatedExecutionAdapter adapter(broker_state);
        const auto operation = operation_from(reopened);
        EXPECT_EQ(operation.state, ts::ExecutionOperationState::EXECUTION_UNKNOWN);
        EXPECT_EQ(manager.reconcile(reopened, operation, adapter).kind,
                  ts::ReconciliationKind::MATCH_CONFIRMED);
        EXPECT_EQ(adapter.submission_count(operation_id), 1U);
        EXPECT_EQ(operation_from(reopened).state,
                  ts::ExecutionOperationState::BROKER_ID_PERSISTED);
    }
}

TEST(TradingSystemBatch5C, LostResponseIsReconciledWithoutResend) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::SimulatedExecutionAdapter adapter;
    ts::TradingSystemStorage storage(database.config());
    prepare(storage);
    const auto operation = operation_from(storage);
    adapter.set_plan(operation.operation_id, ts::SimulatedSubmissionPlan::ACCEPTED_RESPONSE_LOST);
    ASSERT_EQ(manager.submit(storage, operation, adapter).kind,
              ts::StorageResultKind::APPLIED);
    const auto result = manager.reconcile(storage, operation_from(storage), adapter);
    EXPECT_EQ(result.kind, ts::ReconciliationKind::MATCH_CONFIRMED);
    EXPECT_EQ(operation_from(storage).state, ts::ExecutionOperationState::BROKER_ID_PERSISTED);
    const auto revision = operation_from(storage).state_revision;
    EXPECT_EQ(manager.reconcile(storage, operation_from(storage), adapter).kind,
              ts::ReconciliationKind::MATCH_CONFIRMED);
    EXPECT_EQ(operation_from(storage).state_revision, revision);
    EXPECT_EQ(adapter.submission_count(operation.operation_id), 1U);
}

TEST(TradingSystemBatch5C, AmbiguousAndUnavailableHistoryRemainFailClosed) {
    {
        TestDatabase database;
        ts::FakeExecutionClock clock(1000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::SimulatedExecutionAdapter adapter;
        ts::TradingSystemStorage storage(database.config());
        prepare(storage);
        const auto operation = operation_from(storage);
        adapter.add_history_entry({operation.operation_id, operation.client_operation_id,
                                   "broker-a", operation.account_id, operation.trade_id,
                                   std::nullopt});
        adapter.add_history_entry({operation.operation_id, operation.client_operation_id,
                                   "broker-b", operation.account_id, operation.trade_id,
                                   std::nullopt});
        ASSERT_EQ(manager.mark_submission_started(storage, operation).kind,
                  ts::StorageResultKind::APPLIED);
        const auto unknown = operation_from(storage);
        const auto result = manager.reconcile(storage, unknown, adapter);
        EXPECT_EQ(result.kind, ts::ReconciliationKind::AMBIGUOUS);
        EXPECT_EQ(operation_from(storage).state,
                  ts::ExecutionOperationState::RECONCILIATION_REQUIRED);
        EXPECT_FALSE(manager.admission_readiness(storage, true).scope_admission_allowed(1000));
    }
    {
        TestDatabase database;
        ts::FakeExecutionClock clock(1000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::SimulatedExecutionAdapter adapter;
        adapter.set_history_available(false);
        ts::TradingSystemStorage storage(database.config());
        prepare(storage);
        const auto operation = operation_from(storage);
        ASSERT_EQ(manager.mark_submission_started(storage, operation).kind,
                  ts::StorageResultKind::APPLIED);
        EXPECT_EQ(manager.reconcile(storage, operation_from(storage), adapter).kind,
                  ts::ReconciliationKind::HISTORY_UNAVAILABLE);
        EXPECT_EQ(operation_from(storage).state,
                  ts::ExecutionOperationState::RECONCILIATION_REQUIRED);
    }
}

TEST(TradingSystemBatch5C, ProvenPreSubmitRejectionReleasesRiskButNotGroupReadiness) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::SimulatedExecutionAdapter adapter;
    ts::TradingSystemStorage storage(database.config());
    prepare(storage);
    const auto operation = operation_from(storage);
    adapter.set_plan(operation.operation_id, ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION);
    ASSERT_EQ(manager.submit(storage, operation, adapter).kind,
              ts::StorageResultKind::APPLIED);
    EXPECT_EQ(operation_from(storage).state, ts::ExecutionOperationState::REJECTED);
    EXPECT_EQ(storage.reservation(500).value->state, ts::RiskReservationState::RELEASED);
    EXPECT_EQ(storage.physical_trade(700).value->lifecycle_state, "REJECTED");
    EXPECT_FALSE(manager.admission_readiness(storage, true).scope_admission_allowed(1000));
}

TEST(TradingSystemBatch5C, RecoveryRejectsCorruptJournalAndBlocksAdmission) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::TradingSystemStorage storage(database.config());
    prepare(storage);
    const auto operation = operation_from(storage);
    const auto key = ts::ExecutionRecoveryManager::operation_key(operation);
    const auto event = storage.journal_by_key(key);
    ASSERT_TRUE(event.value);
    auto corrupted = *event.value;
    corrupted.payload["state"] = "INVALID_STATE_NAME";
    mdbxc::KeyValueTable<std::string, std::string> journal(
        storage.connection(), "ts_journal");
    journal.insert_or_assign(key, ts::detail::journal_to_json(corrupted).dump());

    const auto recovery = manager.recover(storage);
    EXPECT_EQ(recovery.kind, ts::RecoveryResultKind::CORRUPTED);
    EXPECT_TRUE(recovery.operations.empty());
    const auto readiness = manager.admission_readiness(storage);
    EXPECT_FALSE(readiness.journal_valid);
    EXPECT_TRUE(readiness.global_block);
    EXPECT_FALSE(readiness.scope_admission_allowed(1000));
}

TEST(TradingSystemBatch5C, PublicTransitionsCannotSkipRecoveryStates) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::TradingSystemStorage storage(database.config());
    prepare(storage);
    const auto prepared = operation_from(storage);
    EXPECT_EQ(manager.mark_unknown(storage, prepared).kind,
              ts::StorageResultKind::INVALID_STATE);
    ASSERT_EQ(manager.mark_submission_started(storage, prepared).kind,
              ts::StorageResultKind::APPLIED);
    EXPECT_EQ(manager.settle_reconciled(storage, operation_from(storage),
                                        terminal_settlement()).kind,
              ts::StorageResultKind::INVALID_STATE);

    auto overflow = prepared;
    overflow.state_revision = (std::numeric_limits<std::uint64_t>::max)();
    EXPECT_EQ(manager.mark_submission_started(storage, overflow).kind,
              ts::StorageResultKind::INVALID_ARGUMENT);
}

TEST(TradingSystemBatch5C, RecoveryRejectsInvalidTransitionAndIdentityChain) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::TradingSystemStorage storage(database.config());
    prepare(storage);
    const auto prepared = operation_from(storage);
    auto invalid = prepared;
    invalid.state = ts::ExecutionOperationState::SETTLED;
    invalid.state_revision = 1;
    invalid.broker_trade_id = "broker-forged";
    ts::JournalEvent event{
        9000, "EXECUTION_OPERATION", "operation:" + invalid.operation_id, 1,
        ts::trading_storage_schema_version,
        ts::ExecutionRecoveryManager::operation_key(invalid), 1000, invalid};
    mdbxc::KeyValueTable<std::string, std::string> journal(
        storage.connection(), "ts_journal");
    journal.insert_or_assign(event.idempotency_key,
                             ts::detail::journal_to_json(event).dump());

    const auto recovery = manager.recover(storage);
    EXPECT_EQ(recovery.kind, ts::RecoveryResultKind::CORRUPTED);
    EXPECT_FALSE(manager.admission_readiness(storage, true).scope_admission_allowed(1000));
}

TEST(TradingSystemBatch5C, ReconciledSettlementAndOperationAreCommittedTogether) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::SimulatedExecutionAdapter adapter;
    ts::TradingSystemStorage storage(database.config());
    prepare(storage);
    const auto operation = operation_from(storage);
    ASSERT_EQ(manager.mark_submission_started(storage, operation).kind,
              ts::StorageResultKind::APPLIED);
    const auto started = operation_from(storage);
    adapter.add_history_entry({started.operation_id, started.client_operation_id,
                               "broker-900", started.account_id, started.trade_id,
                               terminal_settlement()});
    EXPECT_EQ(manager.reconcile(storage, started, adapter).kind,
              ts::ReconciliationKind::MATCH_CONFIRMED);
    const auto identified = operation_from(storage);
    ASSERT_EQ(identified.state, ts::ExecutionOperationState::BROKER_ID_PERSISTED);
    ASSERT_EQ(manager.settle_reconciled(storage, identified, terminal_settlement()).kind,
              ts::StorageResultKind::APPLIED);
    EXPECT_EQ(operation_from(storage).state, ts::ExecutionOperationState::SETTLED);
    ASSERT_TRUE(storage.settlement(900).value);
    EXPECT_EQ(storage.settlement(900).value->settlement, terminal_settlement());
    EXPECT_EQ(storage.physical_trade(700).value->lifecycle_state, "SETTLED");
    EXPECT_EQ(manager.settle_reconciled(storage, operation_from(storage), terminal_settlement()).kind,
              ts::StorageResultKind::DUPLICATE);
}

TEST(TradingSystemBatch5C, RecoveryBlocksNewAdmissionButAllowsPreparedGroupMembers) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::SimulatedExecutionAdapter adapter;
    ts::TradingSystemStorage storage(database.config());

    auto admitted = intent();
    admitted.execution_group = {81, 2};
    admitted.cardinality = ts::ExecutionCardinality::MULTIPLE;
    const ts::ExecutionRosterRecord group_a{
        81, 41, 2,
        {{701, 11, 1000, money(25)}, {702, 12, 1000, money(25)}},
        {{701, 501, 1000, money(25), money(25), "risk-a1"},
         {702, 502, 1000, money(25), money(25), "risk-a2"}},
        ts::trading_storage_schema_version};
    auto reservation_a = reservation();
    reservation_a.reservation_id = 501;
    reservation_a.risk_scope_id = 1000;
    reservation_a.execution_group = {81, 2};
    reservation_a.cardinality = ts::ExecutionCardinality::MULTIPLE;
    reservation_a.stake = money(25);
    reservation_a.potential_risk = money(25);
    reservation_a.idempotency_key = "risk-a1";
    auto reservation_b = reservation_a;
    reservation_b.reservation_id = 502;
    reservation_b.idempotency_key = "risk-a2";
    const std::vector<ts::PhysicalTradeRecord> physical_trades{
        {701, 41, 81, 11, 1000, "ADMITTED", {}, 0,
         ts::trading_storage_schema_version},
        {702, 41, 81, 12, 1000, "ADMITTED", {}, 0,
         ts::trading_storage_schema_version}};
    const std::vector<ts::ReservationRecord> reservations_a{
        reservation_a, reservation_b};
    auto owners_a = authoritative_owners(reservations_a);
    ASSERT_TRUE(owners_a.valid) << owners_a.message;
    ASSERT_EQ(manager.prepare(storage, admitted, group_a,
                              reservations_a, physical_trades,
                              owners_a.references).kind,
              ts::StorageResultKind::APPLIED);

    const auto first = operation_for_trade(storage, 701);
    adapter.set_plan(first.operation_id, ts::SimulatedSubmissionPlan::TIMEOUT);
    ASSERT_EQ(manager.submit(storage, first, adapter).kind,
              ts::StorageResultKind::APPLIED);
    const auto second = operation_for_trade(storage, 702);
    EXPECT_EQ(manager.submit(storage, second, adapter).kind,
              ts::StorageResultKind::APPLIED);

    auto new_intent = intent();
    new_intent.execution_group = {82, 1};
    new_intent.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
    const ts::ExecutionRosterRecord group_b{
        82, 41, 1, {{703, 13, 1000, money(25)}},
        {{703, 600, 1000, money(25), money(25), "risk-b"}},
        ts::trading_storage_schema_version};
    auto reservation_c = reservation();
    reservation_c.reservation_id = 600;
    reservation_c.risk_scope_id = 1000;
    reservation_c.execution_group = {82, 1};
    reservation_c.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
    reservation_c.stake = money(25);
    reservation_c.potential_risk = money(25);
    reservation_c.idempotency_key = "risk-b";
    auto physical_c = physical_trade();
    physical_c.trade_id = 703;
    physical_c.group_id = 82;
    physical_c.account_id = 13;
    physical_c.risk_scope_id = 1000;
    const std::vector<ts::ReservationRecord> reservations_c{reservation_c};
    auto owners_c = authoritative_owners(reservations_c);
    ASSERT_TRUE(owners_c.valid) << owners_c.message;
    EXPECT_EQ(manager.prepare(storage, new_intent, group_b,
                              reservations_c, {physical_c},
                              owners_c.references).kind,
              ts::StorageResultKind::INVALID_STATE);
    EXPECT_EQ(storage.roster(82).kind, ts::StorageResultKind::NOT_FOUND);
}

TEST(TradingSystemBatch5C, SettledReplayUsesCanonicalTerminalEvidence) {
    TestDatabase database;
    auto terminal = terminal_settlement();
    terminal.settlement_id = 900;
    {
        ts::FakeExecutionClock clock(1000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::SimulatedExecutionAdapter adapter;
        ts::TradingSystemStorage storage(database.config());
        prepare(storage);
        auto setup = storage.begin();
        ts::SeriesCheckpoint waiting;
        waiting.series = {12, 7, 0, 0};
        waiting.status = ts::SeriesStatus::WAITING_OUTCOME;
        waiting.current_intent_id = 41;
        waiting.current_group_id = 80;
        waiting.current_step = 0;
        waiting.used_intents = {41};
        waiting.used_intent_keys = {"intent-41"};
        ASSERT_EQ(setup.put_series(waiting, 0).kind,
                  ts::StorageResultKind::APPLIED);
        ts::MoneyManagementStateRecord initial_mm{
            "mm.alpha", "test-state", ts::trading_storage_schema_version,
            0, 0, nlohmann::json{{"step", 0}}};
        ASSERT_EQ(setup.put_money_management(initial_mm, 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(setup.commit().kind, ts::StorageResultKind::APPLIED);
        const auto prepared = operation_from(storage);
        ASSERT_EQ(manager.mark_submission_started(storage, prepared).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(manager.mark_unknown(storage, operation_from(storage)).kind,
                  ts::StorageResultKind::APPLIED);

        const auto unknown = group_evidence(
            901, 700, ts::OutcomeKind::UNKNOWN,
            ts::ExecutionEvidence::MAY_HAVE_EXECUTED);
        record_physical_evidence(storage, unknown);
        adapter.add_history_entry({operation_from(storage).operation_id,
                                   operation_from(storage).client_operation_id,
                                   "broker-900", 11, 700, terminal});
        ASSERT_EQ(manager.reconcile(storage, operation_from(storage), adapter).kind,
                  ts::ReconciliationKind::MATCH_CONFIRMED);
        ASSERT_EQ(manager.settle_reconciled(
                      storage, operation_from(storage), terminal).kind,
                  ts::StorageResultKind::APPLIED);
        auto same_outcome_new_id = terminal;
        same_outcome_new_id.settlement_id = 902;
        ts::ExecutionGroupAggregator aggregate(
            41, {80, 1}, {700}, CurrencyType::USD);
        ASSERT_EQ(aggregate.apply(terminal), ts::ProgressionResult::APPLIED);
        ts::SettlementTransition logical;
        logical.journal = {0, "EXECUTION_SETTLED", "trade:700", 0,
                           ts::trading_storage_schema_version,
                           "settlement:902", 1000, {}};
        logical.settlement = {same_outcome_new_id, money(100), std::nullopt, true,
                              ts::trading_storage_schema_version};
        logical.logical_currency = CurrencyType::USD;
        logical.series = ts::SeriesCheckpoint{};
        logical.series->series = {12, 7, 1, 1};
        logical.series->status = ts::SeriesStatus::ACTIVE;
        logical.series->current_outcome = aggregate.outcome();
        logical.series->receipts = {unknown, terminal};
        logical.series->used_intents = {41};
        logical.series->used_intent_keys = {"intent-41"};
        logical.reservation = *storage.reservation(500).value;
        logical.reservation->state = ts::RiskReservationState::SETTLED;
        logical.reservation->state_revision = 1;
        logical.money_management = {
            "mm.alpha", "test-state", ts::trading_storage_schema_version,
            1, 902, nlohmann::json{{"step", 1}}};
        logical.expected_series_revision = 0;
        logical.expected_reservation_revision = 0;
        logical.expected_money_management_revision = 0;
        const auto late_completion = manager.settle_reconciled(
            storage, operation_from(storage), same_outcome_new_id, logical);
        ASSERT_EQ(late_completion.kind, ts::StorageResultKind::APPLIED)
            << late_completion.message;
        EXPECT_EQ(storage.series(12).value->series.state_revision, 1U);
        EXPECT_EQ(storage.money_management("mm.alpha").value->last_settlement_id, 900U);
        EXPECT_EQ(storage.reservation(500).value->state,
                  ts::RiskReservationState::SETTLED);
        EXPECT_EQ(manager.settle_reconciled(
                      storage, operation_from(storage), group_evidence(
                          903, 700, ts::OutcomeKind::WIN,
                          ts::ExecutionEvidence::EXECUTED, 100), logical).kind,
                  ts::StorageResultKind::DUPLICATE);
        const auto facts = storage.settlements_for_group(80);
        ASSERT_TRUE(facts.succeeded());
        EXPECT_EQ(facts.values.size(), 2U);
        EXPECT_EQ(storage.journal_events("GROUP_COMPLETION").values.size(), 1U);
    }

    {
        ts::FakeExecutionClock clock(2000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::TradingSystemStorage reopened(database.config());
        const auto operation = operation_from(reopened);
        EXPECT_EQ(manager.settle_reconciled(
                      reopened, operation, terminal).kind,
                  ts::StorageResultKind::DUPLICATE);
        const auto conflicting = group_evidence(
            903, 700, ts::OutcomeKind::LOSS,
            ts::ExecutionEvidence::EXECUTED, -100);
        EXPECT_EQ(manager.settle_reconciled(
                      reopened, operation, conflicting).kind,
                  ts::StorageResultKind::CONFLICT);
        const auto facts = reopened.settlements_for_group(80);
        ASSERT_TRUE(facts.succeeded());
        EXPECT_EQ(facts.values.size(), 2U);
        EXPECT_EQ(reopened.reservation(500).value->state,
                  ts::RiskReservationState::SETTLED);
        EXPECT_EQ(reopened.journal_events("GROUP_COMPLETION").values.size(), 1U);
    }
}

TEST(TradingSystemBatch5C, PreparationMatchesRosterByIdentityNotVectorPosition) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    auto admitted = intent();
    admitted.execution_group = {81, 2};
    admitted.cardinality = ts::ExecutionCardinality::MULTIPLE;
    const ts::ExecutionRosterRecord group{
        81, 41, 2,
        {{701, 11, 1000, money(25)}, {702, 12, 2000, money(25)}},
        {{701, 501, 1000, money(25), money(25), "risk-1"},
         {702, 502, 2000, money(25), money(25), "risk-2"}},
        ts::trading_storage_schema_version};
    auto first = reservation();
    first.reservation_id = 501;
    first.risk_scope_id = 1000;
    first.execution_group = {81, 2};
    first.cardinality = ts::ExecutionCardinality::MULTIPLE;
    first.stake = money(25);
    first.potential_risk = money(25);
    first.idempotency_key = "risk-1";
    auto second = first;
    second.reservation_id = 502;
    second.risk_scope_id = 2000;
    second.idempotency_key = "risk-2";
    const ts::PhysicalTradeRecord physical_first{
        701, 41, 81, 11, 1000, "ADMITTED", {}, 0,
        ts::trading_storage_schema_version};
    const ts::PhysicalTradeRecord physical_second{
        702, 41, 81, 12, 2000, "ADMITTED", {}, 0,
        ts::trading_storage_schema_version};
    const std::vector<ts::ReservationRecord> reversed_reservations{second, first};
    auto reversed_owners = authoritative_owners(reversed_reservations);
    ASSERT_TRUE(reversed_owners.valid) << reversed_owners.message;
    const std::vector<ts::ReservationRecord> ordered_reservations{first, second};
    {
        ts::TradingSystemStorage storage(database.config());
        ASSERT_EQ(manager.prepare(storage, admitted, group,
                                  reversed_reservations,
                                  {physical_second, physical_first},
                                  reversed_owners.references).kind,
                  ts::StorageResultKind::APPLIED);
    }
    {
        ts::FakeExecutionClock reopened_clock(500000);
        ts::ExecutionRecoveryManager reopened_manager(reopened_clock);
        ts::TradingSystemStorage reopened(database.config());
        auto ordered_owners = authoritative_owners(ordered_reservations);
        ASSERT_TRUE(ordered_owners.valid) << ordered_owners.message;
        const auto before_events = reopened.journal_events("EXECUTION_OPERATION");
        ASSERT_TRUE(before_events.succeeded());
        const auto reopened_result = reopened_manager.prepare(
            reopened, admitted, group, ordered_reservations,
            {physical_first, physical_second}, ordered_owners.references);
        EXPECT_EQ(reopened_result.kind, ts::StorageResultKind::APPLIED)
            << reopened_result.message;
        const auto after_events = reopened.journal_events("EXECUTION_OPERATION");
        ASSERT_TRUE(after_events.succeeded());
        EXPECT_EQ(after_events.values.size(), before_events.values.size());
        for (const auto& event : after_events.values) {
            EXPECT_EQ(event.occurred_at_ms, 1000);
        }
    }

    TestDatabase mismatch_database;
    ts::TradingSystemStorage mismatch_storage(mismatch_database.config());
    auto forged = physical_first;
    forged.account_id = 99;
    auto mismatch_owners = authoritative_owners(ordered_reservations);
    ASSERT_TRUE(mismatch_owners.valid) << mismatch_owners.message;
    EXPECT_EQ(manager.prepare(mismatch_storage, admitted, group,
                              ordered_reservations, {forged, physical_second},
                              mismatch_owners.references).kind,
              ts::StorageResultKind::CONFLICT);
    EXPECT_EQ(mismatch_storage.roster(81).kind, ts::StorageResultKind::NOT_FOUND);
}

TEST(TradingSystemBatch5C, ExternalSettlementIdsAdvanceLocalAllocation) {
    TestDatabase database;
    {
        ts::FakeExecutionClock clock(1000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::SimulatedExecutionAdapter adapter;
        ts::TradingSystemStorage storage(database.config());
        prepare(storage);

        const auto external = group_evidence(
            1, 700, ts::OutcomeKind::UNKNOWN,
            ts::ExecutionEvidence::MAY_HAVE_EXECUTED);
        record_physical_evidence(storage, external);

        const auto operation = operation_from(storage);
        adapter.set_plan(operation.operation_id,
                         ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION);
        ASSERT_EQ(manager.submit(storage, operation, adapter).kind,
                  ts::StorageResultKind::APPLIED);
        EXPECT_EQ(storage.settlement(1).kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(storage.settlement(2).kind, ts::StorageResultKind::APPLIED);
        const auto facts = storage.settlements_for_group(80);
        ASSERT_TRUE(facts.succeeded());
        EXPECT_EQ(facts.values.size(), 2U);
    }

    {
        ts::TradingSystemStorage reopened(database.config());
        EXPECT_EQ(reopened.settlement(1).kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(reopened.settlement(2).kind, ts::StorageResultKind::APPLIED);
        auto unit = reopened.begin();
        EXPECT_EQ(unit.next_id("settlement").value, 3U);
        unit.rollback();
    }

    TestDatabase overflow_database;
    ts::TradingSystemStorage overflow_storage(overflow_database.config());
    mdbxc::KeyValueTable<std::string, std::string> sequences(
        overflow_storage.connection(), "ts_sequences");
    sequences.insert_or_assign(
        "settlement", std::to_string((std::numeric_limits<std::uint64_t>::max)()));
    auto unit = overflow_storage.begin();
    EXPECT_EQ(unit.next_id("settlement").kind,
              ts::StorageResultKind::INVALID_ARGUMENT);
    unit.rollback();

    TestDatabase legacy_database;
    {
        ts::TradingSystemStorage legacy_storage(legacy_database.config());
        auto legacy_unit = legacy_storage.begin();
        auto legacy_settlement = terminal_settlement();
        legacy_settlement.settlement_id = 1;
        ASSERT_EQ(legacy_unit.put_settlement(
            {legacy_settlement, money(100), std::nullopt, true,
             ts::trading_storage_schema_version}).kind,
            ts::StorageResultKind::APPLIED);
        ASSERT_EQ(legacy_unit.commit().kind, ts::StorageResultKind::APPLIED);
        mdbxc::KeyValueTable<std::string, std::string> legacy_sequences(
            legacy_storage.connection(), "ts_sequences");
        legacy_sequences.erase("settlement");
        auto blocked = legacy_storage.begin();
        EXPECT_EQ(blocked.next_id("settlement").kind,
                  ts::StorageResultKind::INVALID_ARGUMENT);
        blocked.rollback();
        auto rebuild = legacy_storage.begin();
        ASSERT_EQ(rebuild.rebuild_settlement_sequence().kind,
                  ts::StorageResultKind::APPLIED);
        EXPECT_EQ(rebuild.next_id("settlement").value, 2U);
        rebuild.rollback();
    }
}

TEST(TradingSystemBatch5C, RejectsMultipleOwnersForOneRiskScope) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::TradingSystemStorage storage(database.config());

    auto admitted = intent();
    admitted.execution_group = {180, 2};
    admitted.cardinality = ts::ExecutionCardinality::MULTIPLE;
    ts::ExecutionRosterRecord group{
        180, admitted.intent_id, 2,
        {{801, 11, 1000, money(25)}, {802, 12, 1000, money(25)}},
        {{801, 801, 1000, money(25), money(25), "duplicate-owner-a"},
         {802, 802, 1000, money(25), money(25), "duplicate-owner-b"}},
        ts::trading_storage_schema_version};
    auto first = reservation();
    first.reservation_id = 801;
    first.intent_id = admitted.intent_id;
    first.execution_group = {180, 2};
    first.cardinality = ts::ExecutionCardinality::MULTIPLE;
    first.stake = money(25);
    first.potential_risk = money(25);
    first.idempotency_key = "duplicate-owner-a";
    auto second = first;
    second.reservation_id = 802;
    second.idempotency_key = "duplicate-owner-b";
    const std::vector<ts::ReservationRecord> reservations{first, second};
    const std::vector<ts::PhysicalTradeRecord> physical{
        {801, admitted.intent_id, 180, 11, 1000, "ADMITTED", {}, 0,
         ts::trading_storage_schema_version},
        {802, admitted.intent_id, 180, 12, 1000, "ADMITTED", {}, 0,
         ts::trading_storage_schema_version}};

    auto make_owner = [](const ts::ReservationRecord& value) {
        ts::RiskAdmissionConfig config;
        config.scope = {value.risk_scope_kind, value.risk_scope_id};
        config.max_snapshot_age_ms = 60'000;
        config.max_reserved_risk = value.potential_risk;
        auto owner = std::make_unique<ts::RiskAdmission>(config);
        ts::RiskAccountSnapshot snapshot;
        snapshot.availability = ts::RiskAvailability::SUPPORTED;
        snapshot.available_balance = value.potential_risk;
        snapshot.captured_at_ms = 1000;
        snapshot.revision = 7;
        EXPECT_EQ(owner->hydrate(snapshot, {value.to_risk_reservation()},
                                value.reservation_id + 1).kind,
                  ts::RiskAdmissionKind::APPLIED);
        return owner;
    };
    auto owner_a = make_owner(first);
    auto owner_b = make_owner(second);
    const std::vector<const ts::RiskAdmission*> owners{owner_a.get(), owner_b.get()};

    EXPECT_EQ(manager.prepare(storage, admitted, group, reservations, physical, owners).kind,
              ts::StorageResultKind::CONFLICT);
    EXPECT_EQ(storage.roster(180).kind, ts::StorageResultKind::NOT_FOUND);
}

TEST(TradingSystemBatch5C, ConcurrentAdmissionsSerializeWithoutPartialRoster) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    const auto run = [&storage](std::uint64_t group_id,
                                 std::uint64_t intent_id,
                                 std::uint64_t reservation_id,
                                 std::uint64_t trade_id,
                                 std::uint64_t account_id) {
        ts::FakeExecutionClock clock(1000);
        ts::ExecutionRecoveryManager manager(clock);
        auto admitted = intent();
        admitted.intent_id = intent_id;
        admitted.idempotency_key = "intent-" + std::to_string(intent_id);
        admitted.execution_group = {group_id, 1};
        admitted.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
        auto group = roster();
        group.group_id = group_id;
        group.intent_id = intent_id;
        group.members.front() = {trade_id, account_id, 1000, money(50)};
        group.reservation_bindings.front() = {
            trade_id, reservation_id, 1000, money(50), money(50),
            "concurrent-risk-" + std::to_string(reservation_id)};
        auto reserved = reservation();
        reserved.reservation_id = reservation_id;
        reserved.intent_id = intent_id;
        reserved.execution_group = {group_id, 1};
        reserved.idempotency_key = group.reservation_bindings.front().idempotency_key;
        auto physical = physical_trade();
        physical.trade_id = trade_id;
        physical.intent_id = intent_id;
        physical.group_id = group_id;
        physical.account_id = account_id;
        const auto owners = authoritative_owners({reserved});
        if (!owners.valid) {
            return ts::StorageResult{ts::StorageResultKind::INVALID_STATE,
                                     owners.message};
        }
        return manager.prepare(storage, admitted, group, {reserved}, {physical},
                               owners.references);
    };

    auto first = std::async(std::launch::async, run, 181, 181, 811, 811, 21);
    auto second = std::async(std::launch::async, run, 182, 182, 812, 812, 22);
    ASSERT_EQ(first.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    ASSERT_EQ(second.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    const auto first_result = first.get();
    const auto second_result = second.get();
    EXPECT_TRUE((first_result.kind == ts::StorageResultKind::APPLIED) !=
                (second_result.kind == ts::StorageResultKind::APPLIED))
        << ts::to_string(first_result.kind) << ", " << first_result.message
        << "; " << ts::to_string(second_result.kind) << ", " << second_result.message;
    const auto& rejected = first_result.kind == ts::StorageResultKind::APPLIED
        ? second_result : first_result;
    EXPECT_EQ(rejected.kind, ts::StorageResultKind::INVALID_STATE);

    const auto first_roster = storage.roster(181);
    const auto second_roster = storage.roster(182);
    EXPECT_TRUE(first_roster.found() != second_roster.found());
    const auto persisted = first_roster.found() ? first_roster : second_roster;
    ASSERT_TRUE(persisted.found());
    EXPECT_EQ(persisted.value->members.size(), 1U);
    EXPECT_EQ(persisted.value->reservation_bindings.size(), 1U);
}

TEST(TradingSystemBatch5C, LegacyAdmissionIndexRequiresExplicitRebuild) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    prepare(storage);

    mdbxc::KeyValueTable<std::string, std::string> index(
        storage.connection(), "ts_admission_scope_index");
    std::vector<std::pair<std::string, std::string>> entries;
    ASSERT_NO_THROW(index.load(entries));
    for (const auto& entry : entries) index.erase(entry.first);

    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    const auto owners = authoritative_owners({reservation()});
    ASSERT_TRUE(owners.valid) << owners.message;
    EXPECT_EQ(manager.prepare(storage, intent(), roster(), {reservation()},
                              {physical_trade()}, owners.references).kind,
              ts::StorageResultKind::INVALID_STATE);

    auto rebuild = storage.begin();
    ASSERT_EQ(rebuild.rebuild_admission_scope_index().kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(rebuild.commit().kind, ts::StorageResultKind::APPLIED);
    EXPECT_EQ(manager.prepare(storage, intent(), roster(), {reservation()},
                              {physical_trade()}, owners.references).kind,
              ts::StorageResultKind::APPLIED);
}

TEST(TradingSystemBatch5C, MultiAccountRosterKeepsPhysicalAndRiskIdentitiesSeparate) {
    TestDatabase database;
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ts::TradingSystemStorage storage(database.config());

    auto admitted = intent();
    admitted.execution_group = {81, 2};
    admitted.cardinality = ts::ExecutionCardinality::MULTIPLE;

    ts::ExecutionRosterRecord multi_roster{
        81, 41, 2,
        {{701, 11, 1000, money(25)}, {702, 12, 2000, money(25)}},
        {{701, 501, 1000, money(25), money(25), "risk-501"},
         {702, 502, 2000, money(25), money(25), "risk-502"}},
        ts::trading_storage_schema_version};
    auto first = reservation();
    first.reservation_id = 501;
    first.risk_scope_id = 1000;
    first.execution_group = {81, 2};
    first.cardinality = ts::ExecutionCardinality::MULTIPLE;
    first.stake = money(25);
    first.potential_risk = money(25);
    first.idempotency_key = "risk-501";
    auto second = first;
    second.reservation_id = 502;
    second.risk_scope_id = 2000;
    second.idempotency_key = "risk-502";
    auto physical_first = physical_trade();
    physical_first.trade_id = 701;
    physical_first.group_id = 81;
    physical_first.risk_scope_id = 1000;
    auto physical_second = physical_first;
    physical_second.trade_id = 702;
    physical_second.account_id = 12;
    physical_second.risk_scope_id = 2000;
    const std::vector<ts::ReservationRecord> multi_reservations{first, second};
    auto multi_owners = authoritative_owners(multi_reservations);
    ASSERT_TRUE(multi_owners.valid) << multi_owners.message;

    ASSERT_EQ(manager.prepare(storage, admitted, multi_roster,
                              multi_reservations,
                              {physical_first, physical_second},
                              multi_owners.references).kind,
              ts::StorageResultKind::APPLIED);
    const auto persisted = storage.roster(81);
    ASSERT_TRUE(persisted.value);
    EXPECT_EQ(persisted.value->members[0].account_id, 11U);
    EXPECT_EQ(persisted.value->members[1].account_id, 12U);
    EXPECT_EQ(persisted.value->members[0].risk_scope_id, 1000U);
    EXPECT_EQ(persisted.value->members[1].risk_scope_id, 2000U);
    EXPECT_EQ(ts::ExecutionRecoveryManager::recover(storage).operations.size(), 2U);
}

TEST(TradingSystemBatch5C, MultiAccountCrashRecoveryCompletesGroupExactlyOnce) {
    TestDatabase database;
    auto broker_state = std::make_shared<ts::SimulatedExecutionAdapter::State>();
    ts::TradeSettlement account_a = group_evidence(
        900, 700, ts::OutcomeKind::WIN, ts::ExecutionEvidence::EXECUTED, 100);
    ts::TradeSettlement account_c = group_evidence(
        902, 702, ts::OutcomeKind::LOSS, ts::ExecutionEvidence::EXECUTED, -25);

    auto admitted = intent();
    admitted.execution_group = {80, 3};
    admitted.cardinality = ts::ExecutionCardinality::MULTIPLE;
    ts::ExecutionRosterRecord group_roster{
        80, 41, 3,
        {{700, 11, 1000, money(50)}, {701, 12, 1000, money(20)},
         {702, 13, 2000, money(30)}},
        {{700, 500, 1000, money(50), money(50), "risk-500"},
         {701, 501, 1000, money(20), money(20), "risk-501"},
         {702, 502, 2000, money(30), money(30), "risk-502"}},
        ts::trading_storage_schema_version};
    auto reservation_a = reservation();
    reservation_a.execution_group = {80, 3};
    reservation_a.cardinality = ts::ExecutionCardinality::MULTIPLE;
    reservation_a.stake = money(50);
    reservation_a.potential_risk = money(50);
    reservation_a.idempotency_key = "risk-500";
    auto reservation_b = reservation_a;
    reservation_b.reservation_id = 501;
    reservation_b.risk_scope_id = 1000;
    reservation_b.stake = money(20);
    reservation_b.potential_risk = money(20);
    reservation_b.idempotency_key = "risk-501";
    auto reservation_c = reservation_a;
    reservation_c.reservation_id = 502;
    reservation_c.risk_scope_id = 2000;
    reservation_c.stake = money(30);
    reservation_c.potential_risk = money(30);
    reservation_c.idempotency_key = "risk-502";
    const std::vector<ts::ReservationRecord> reservations{
        reservation_a, reservation_b, reservation_c};
    const std::vector<ts::PhysicalTradeRecord> physical_trades{
        physical_trade(),
        {701, 41, 80, 12, 1000, "ADMITTED", {}, 0,
         ts::trading_storage_schema_version},
        {702, 41, 80, 13, 2000, "ADMITTED", {}, 0,
         ts::trading_storage_schema_version}};
    auto crash_owners = authoritative_owners(reservations);
    ASSERT_TRUE(crash_owners.valid) << crash_owners.message;

    {
        ts::FakeExecutionClock clock(1000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::TradingSystemStorage storage(database.config());
        ASSERT_EQ(manager.prepare(storage, admitted, group_roster,
                                  reservations, physical_trades,
                                  crash_owners.references).kind,
                  ts::StorageResultKind::APPLIED);
        auto unit = storage.begin();
        ts::SeriesCheckpoint waiting;
        waiting.series = {12, 7, 0, 0};
        waiting.status = ts::SeriesStatus::WAITING_OUTCOME;
        waiting.current_intent_id = 41;
        waiting.current_group_id = 80;
        waiting.current_step = 0;
        waiting.used_intents = {41};
        waiting.used_intent_keys = {"intent-41"};
        ASSERT_EQ(unit.put_series(waiting, 0).kind, ts::StorageResultKind::APPLIED);
        ts::MoneyManagementStateRecord mm{
            "mm.alpha", "test-state", ts::trading_storage_schema_version,
            0, 0, nlohmann::json{{"step", 0}}};
        ASSERT_EQ(unit.put_money_management(mm, 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);

        ts::SimulatedExecutionAdapter adapter(broker_state);
        const auto op_a = operation_for_trade(storage, 700);
        const auto op_b = operation_for_trade(storage, 701);
        const auto op_c = operation_for_trade(storage, 702);
        adapter.set_plan(op_a.operation_id, ts::SimulatedSubmissionPlan::SUCCESS);
        adapter.set_plan(op_b.operation_id, ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION);
        adapter.set_plan(op_c.operation_id, ts::SimulatedSubmissionPlan::TIMEOUT);
        ASSERT_EQ(manager.submit(storage, op_a, adapter).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(manager.submit(storage, op_b, adapter).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(manager.submit(storage, op_c, adapter).kind,
                  ts::StorageResultKind::APPLIED);
        adapter.add_history_entry({op_c.operation_id, op_c.client_operation_id,
                                   "broker-c", op_c.account_id, op_c.trade_id,
                                   account_c});
        ASSERT_EQ(manager.settle_reconciled(
                      storage, operation_for_trade(storage, 700), account_a).kind,
                  ts::StorageResultKind::APPLIED);
        EXPECT_FALSE(manager.admission_readiness(storage, true).scope_admission_allowed(1000));
    }

    {
        ts::FakeExecutionClock clock(2000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::TradingSystemStorage storage(database.config());
        ts::SimulatedExecutionAdapter adapter(broker_state);
        const auto op_c = operation_for_trade(storage, 702);
        ASSERT_EQ(manager.reconcile(storage, op_c, adapter).kind,
                  ts::ReconciliationKind::MATCH_CONFIRMED);
        const auto c_settlement = ts::SettlementRecord{
            account_c, money(-25), std::nullopt, true,
            ts::trading_storage_schema_version};
        const auto group_settlements = storage.settlements_for_group(80);
        ASSERT_TRUE(group_settlements.succeeded());
        std::vector<ts::TradeSettlement> receipts;
        for (const auto& value : group_settlements.values) {
            receipts.push_back(value.settlement);
        }
        receipts.push_back(account_c);
        ts::ExecutionGroupAggregator aggregate(
            41, {80, 3}, {700, 701, 702}, CurrencyType::USD);
        ASSERT_EQ(aggregate.apply(receipts[0]), ts::ProgressionResult::APPLIED);
        ASSERT_EQ(aggregate.apply(receipts[1]), ts::ProgressionResult::APPLIED);
        ASSERT_EQ(aggregate.apply(receipts[2]), ts::ProgressionResult::APPLIED);

        ts::SettlementTransition logical;
        logical.journal.event_type = "EXECUTION_SETTLED";
        logical.journal.aggregate_id = "trade:702";
        logical.journal.idempotency_key = "settlement:902";
        logical.settlement = c_settlement;
        logical.logical_currency = CurrencyType::USD;
        logical.series = ts::SeriesCheckpoint{};
        logical.series->series = {12, 7, 1, 1};
        logical.series->status = ts::SeriesStatus::ACTIVE;
        logical.series->current_outcome = aggregate.outcome();
        logical.series->receipts = receipts;
        logical.series->used_intents = {41};
        logical.series->used_intent_keys = {"intent-41"};
        logical.reservation = reservation_c;
        logical.reservation->state = ts::RiskReservationState::SETTLED;
        logical.reservation->state_revision = 1;
        logical.money_management = {
            "mm.alpha", "test-state", ts::trading_storage_schema_version,
            1, 902, nlohmann::json{{"step", 1}}};
        logical.expected_series_revision = 0;
        logical.expected_reservation_revision = 0;
        logical.expected_money_management_revision = 0;
        auto settled_a = reservation_a;
        settled_a.state = ts::RiskReservationState::SETTLED;
        settled_a.state_revision = 1;
        auto released_b = reservation_b;
        released_b.state = ts::RiskReservationState::RELEASED;
        released_b.state_revision = 1;
        logical.group_reservation_bindings = {
            {700, settled_a, 0}, {701, released_b, 0}};
        // Crash boundary: physical completion is durable before the logical transition.
        ASSERT_EQ(manager.settle_reconciled(
                      storage, operation_for_trade(storage, 702), account_c).kind,
                  ts::StorageResultKind::APPLIED);
        EXPECT_FALSE(manager.admission_readiness(storage, true).scope_admission_allowed(2000));
        const auto completion = manager.settle_reconciled(
            storage, operation_for_trade(storage, 702), account_c, logical);
        ASSERT_EQ(completion.kind, ts::StorageResultKind::APPLIED) << completion.message;
        EXPECT_EQ(storage.series(12).value->series.state_revision, 1U);
        const auto persisted_mm = storage.money_management("mm.alpha");
        ASSERT_TRUE(persisted_mm.value) << persisted_mm.message;
        EXPECT_EQ(persisted_mm.value->state_revision, 1U)
            << "last_settlement_id=" << persisted_mm.value->last_settlement_id
            << " payload=" << persisted_mm.value->payload.dump();
        EXPECT_EQ(storage.reservation(500).value->state, ts::RiskReservationState::SETTLED);
        EXPECT_EQ(storage.reservation(501).value->state, ts::RiskReservationState::RELEASED);
        EXPECT_EQ(storage.reservation(502).value->state, ts::RiskReservationState::SETTLED);
        EXPECT_EQ(manager.settle_reconciled(
                      storage, operation_for_trade(storage, 702), account_c).kind,
                  ts::StorageResultKind::DUPLICATE);
        auto conflicting = account_c;
        conflicting.kind = ts::OutcomeKind::WIN;
        conflicting.net_profit = money(25);
        EXPECT_EQ(manager.settle_reconciled(
                      storage, operation_for_trade(storage, 702), conflicting).kind,
                  ts::StorageResultKind::CONFLICT);
        EXPECT_FALSE(manager.admission_readiness(storage).scope_admission_allowed(1000));
        EXPECT_TRUE(manager.admission_readiness(storage, true).scope_admission_allowed(1000));
        EXPECT_TRUE(manager.admission_readiness(storage, true).scope_admission_allowed(2000));
    }
    {
        ts::FakeExecutionClock clock(3000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::TradingSystemStorage reopened(database.config());
        ts::SimulatedExecutionAdapter adapter(broker_state);
        EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
        EXPECT_EQ(reopened.money_management("mm.alpha").value->state_revision, 1U);
        EXPECT_EQ(reopened.journal_events("GROUP_COMPLETION").values.size(), 1U);
        EXPECT_EQ(manager.settle_reconciled(
                      reopened, operation_for_trade(reopened, 702), account_c).kind,
                  ts::StorageResultKind::DUPLICATE);
        for (const auto trade : {700U, 701U, 702U}) {
            const auto operation = operation_for_trade(reopened, trade);
            EXPECT_NE(manager.submit(reopened, operation, adapter).kind,
                      ts::StorageResultKind::APPLIED);
            EXPECT_EQ(adapter.submission_count(operation.operation_id), 1U);
        }
        EXPECT_TRUE(manager.admission_readiness(reopened, true).scope_admission_allowed(1000));
    }
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
