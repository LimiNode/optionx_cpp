#include <gtest/gtest.h>
#include <optionx_cpp/trading_system_storage.hpp>

#include <chrono>
#include <filesystem>
#include <string>

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
    const auto operations = ts::ExecutionRecoveryManager::latest(storage);
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

void prepare(ts::TradingSystemStorage& storage) {
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ASSERT_EQ(manager.prepare(storage, intent(), roster(), {reservation()}, {physical_trade()}).kind,
              ts::StorageResultKind::APPLIED);
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
        EXPECT_EQ(manager.blocked_risk_scopes(storage).count(1000), 1U);
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

TEST(TradingSystemBatch5C, ProvenPreSubmitRejectionIsTerminalWithoutSettlement) {
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
    EXPECT_EQ(manager.blocked_risk_scopes(storage).count(1000), 0U);
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
    physical_first.risk_scope_id = 1000;
    auto physical_second = physical_first;
    physical_second.trade_id = 702;
    physical_second.account_id = 12;
    physical_second.risk_scope_id = 2000;

    ASSERT_EQ(manager.prepare(storage, admitted, multi_roster,
                              {first, second}, {physical_first, physical_second}).kind,
              ts::StorageResultKind::APPLIED);
    const auto persisted = storage.roster(81);
    ASSERT_TRUE(persisted.value);
    EXPECT_EQ(persisted.value->members[0].account_id, 11U);
    EXPECT_EQ(persisted.value->members[1].account_id, 12U);
    EXPECT_EQ(persisted.value->members[0].risk_scope_id, 1000U);
    EXPECT_EQ(persisted.value->members[1].risk_scope_id, 2000U);
    EXPECT_EQ(ts::ExecutionRecoveryManager::latest(storage).size(), 2U);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
