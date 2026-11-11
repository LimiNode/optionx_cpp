#include <gtest/gtest.h>
#include <optionx_cpp/trading_system.hpp>

#include <chrono>
#include <filesystem>
#include <limits>
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

void prepare(ts::TradingSystemStorage& storage) {
    ts::FakeExecutionClock clock(1000);
    ts::ExecutionRecoveryManager manager(clock);
    ASSERT_EQ(manager.prepare(storage, intent(), roster(), {reservation()}, {physical_trade()}).kind,
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
    ASSERT_EQ(manager.prepare(storage, admitted, group_a,
                              {reservation_a, reservation_b}, physical_trades).kind,
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
    EXPECT_EQ(manager.prepare(storage, new_intent, group_b,
                              {reservation_c}, {physical_c}).kind,
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
        const auto reservation_before = storage.reservation(500);
        ASSERT_TRUE(reservation_before.value);
        const auto completions_before = storage.journal_events("GROUP_COMPLETION");
        ASSERT_TRUE(completions_before.succeeded());

        auto same_outcome_new_id = terminal;
        same_outcome_new_id.settlement_id = 902;
        EXPECT_EQ(manager.settle_reconciled(
                      storage, operation_from(storage), same_outcome_new_id).kind,
                  ts::StorageResultKind::DUPLICATE);
        const auto facts = storage.settlements_for_group(80);
        ASSERT_TRUE(facts.succeeded());
        EXPECT_EQ(facts.values.size(), 2U);
        EXPECT_EQ(storage.reservation(500).value->state,
                  reservation_before.value->state);
        EXPECT_EQ(storage.reservation(500).value->state_revision,
                  reservation_before.value->state_revision);
        EXPECT_EQ(storage.journal_events("GROUP_COMPLETION").values.size(),
                  completions_before.values.size());
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
                  ts::RiskReservationState::RESERVED);
        EXPECT_TRUE(reopened.journal_events("GROUP_COMPLETION").values.empty());
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
    {
        ts::TradingSystemStorage storage(database.config());
        ASSERT_EQ(manager.prepare(storage, admitted, group,
                                  {second, first}, {physical_second, physical_first}).kind,
                  ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage reopened(database.config());
        const auto reopened_result = manager.prepare(
            reopened, admitted, group, {first, second},
            {physical_first, physical_second});
        EXPECT_EQ(reopened_result.kind, ts::StorageResultKind::APPLIED)
            << reopened_result.message;
    }

    TestDatabase mismatch_database;
    ts::TradingSystemStorage mismatch_storage(mismatch_database.config());
    auto forged = physical_first;
    forged.account_id = 99;
    EXPECT_EQ(manager.prepare(mismatch_storage, admitted, group,
                              {first, second}, {forged, physical_second}).kind,
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

    ASSERT_EQ(manager.prepare(storage, admitted, multi_roster,
                              {first, second}, {physical_first, physical_second}).kind,
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

    {
        ts::FakeExecutionClock clock(1000);
        ts::ExecutionRecoveryManager manager(clock);
        ts::TradingSystemStorage storage(database.config());
        ASSERT_EQ(manager.prepare(storage, admitted, group_roster,
                                  reservations, physical_trades).kind,
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
