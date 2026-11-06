#include <gtest/gtest.h>
#include <optionx_cpp/trading_system_storage.hpp>

#include <chrono>
#include <filesystem>
#include <limits>
#include <string>

namespace {

namespace ts = optionx::trading_system;
using optionx::CurrencyType;

struct TestDatabase {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("optionx_batch5b_" + std::to_string(
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

ts::MoneyAmount money(std::int64_t units, CurrencyType currency = CurrencyType::USD) {
    return {units, 2, currency};
}

ts::TradeSettlement settlement(std::uint64_t settlement_id = 900,
                               std::uint64_t trade_id = 700,
                               CurrencyType currency = CurrencyType::USD) {
    ts::TradeSettlement result;
    result.settlement_id = settlement_id;
    result.intent_id = 41;
    result.execution_group_id = 80;
    result.trade_id = trade_id;
    result.kind = ts::OutcomeKind::WIN;
    result.evidence = ts::ExecutionEvidence::EXECUTED;
    result.net_profit = money(100, currency);
    return result;
}

ts::TradeSettlement group_settlement(
    std::uint64_t settlement_id, ts::IntentId intent_id,
    ts::ExecutionGroupId group_id, std::uint64_t trade_id,
    std::int64_t profit) {
    auto result = settlement(settlement_id, trade_id);
    result.intent_id = intent_id;
    result.execution_group_id = group_id;
    result.net_profit = money(profit);
    return result;
}

ts::SettlementRecord settlement_record(CurrencyType currency = CurrencyType::USD) {
    return {settlement(900, 700, currency), money(100, currency), std::nullopt, true,
            ts::trading_storage_schema_version};
}

ts::SeriesCheckpoint checkpoint(std::uint64_t revision = 0) {
    ts::SeriesCheckpoint result;
    result.series = {12, 7, 0, revision};
    result.status = ts::SeriesStatus::ACTIVE;
    result.schema_version = ts::trading_storage_schema_version;
    return result;
}

ts::GroupOutcome terminal_outcome() {
    ts::ExecutionGroupAggregator aggregate(41, {80, 1}, {700}, CurrencyType::USD);
    EXPECT_TRUE(aggregate.valid());
    EXPECT_EQ(aggregate.apply(settlement()), ts::ProgressionResult::APPLIED);
    return aggregate.outcome();
}

ts::GroupOutcome multi_terminal_outcome() {
    ts::ExecutionGroupAggregator aggregate(41, {80, 2}, {700, 701}, CurrencyType::USD);
    EXPECT_TRUE(aggregate.valid());
    auto first = settlement(901, 700);
    auto second = settlement(902, 701);
    EXPECT_EQ(aggregate.apply(first), ts::ProgressionResult::APPLIED);
    EXPECT_EQ(aggregate.apply(second), ts::ProgressionResult::APPLIED);
    return aggregate.outcome();
}

ts::ReservationRecord reservation(std::uint64_t revision = 0) {
    ts::ReservationRecord result;
    result.reservation_id = 500;
    result.risk_scope_id = 1000;
    result.execution_group = {80, 1};
    result.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
    result.stake = money(50);
    result.intent_id = 41;
    result.state = revision == 0 ? ts::RiskReservationState::RESERVED
                                 : ts::RiskReservationState::SETTLED;
    result.state_revision = revision;
    result.account_revision = 7;
    result.potential_risk = money(50);
    result.idempotency_key = "risk-41";
    return result;
}

ts::MoneyManagementStateRecord mm_state(std::uint64_t revision = 0) {
    return {"mm.alpha", "test-state", ts::trading_storage_schema_version,
            revision, revision == 0 ? ts::SettlementId{0} : ts::SettlementId{900},
            nlohmann::json{{"step", revision}}};
}

ts::PhysicalTradeRecord physical_trade(std::uint64_t revision = 0) {
    return {700, 41, 80, 11, 1000,
            revision == 0 ? "ADMITTED" : "SETTLED", "", revision,
            ts::trading_storage_schema_version};
}

ts::ReservationRecord multi_reservation(
    std::uint64_t reservation_id, std::uint64_t risk_scope_id,
    std::int64_t amount, std::uint64_t revision = 0) {
    auto result = reservation(revision);
    result.reservation_id = reservation_id;
    result.risk_scope_id = risk_scope_id;
    result.execution_group = {80, 2};
    result.cardinality = ts::ExecutionCardinality::MULTIPLE;
    result.stake = money(amount);
    result.potential_risk = money(amount);
    result.idempotency_key = "risk-" + std::to_string(reservation_id);
    return result;
}

ts::PhysicalTradeRecord physical_trade_for(
    std::uint64_t trade_id, std::uint64_t account_id,
    std::uint64_t risk_scope_id, std::uint64_t revision = 0) {
    return {trade_id, 41, 80, account_id, risk_scope_id,
            revision == 0 ? "ADMITTED" : "SETTLED", "", revision,
            ts::trading_storage_schema_version};
}

ts::JournalEvent journal() {
    ts::JournalEvent result;
    result.event_id = 1001;
    result.event_type = "SETTLEMENT_APPLIED";
    result.aggregate_id = "group:80";
    result.state_revision = 1;
    result.idempotency_key = "settlement-event-900";
    result.payload = ts::detail::settlement_to_json(settlement_record().settlement);
    return result;
}

ts::SettlementTransition transition() {
    ts::SettlementTransition result;
    result.journal = journal();
    result.settlement = settlement_record();
    result.series = checkpoint(1);
    result.series->series.next_step = 1;
    result.series->current_outcome = terminal_outcome();
    result.physical_trade = physical_trade(1);
    result.reservation = reservation(1);
    result.money_management = mm_state(1);
    result.expected_series_revision = 0;
    result.expected_physical_trade_revision = 0;
    result.expected_reservation_revision = 0;
    result.expected_money_management_revision = 0;
    result.logical_currency = CurrencyType::USD;
    return result;
}

ts::ExecutionRosterRecord roster() {
    ts::ExecutionRosterRecord result;
    result.group_id = 80;
    result.intent_id = 41;
    result.expected_members = 1;
    result.members = {{700, 11, 1000, money(10)}};
    return result;
}

ts::ExecutionRosterRecord multi_roster() {
    ts::ExecutionRosterRecord result;
    result.group_id = 80;
    result.intent_id = 41;
    result.expected_members = 2;
    result.members = {{700, 11, 1000, money(10)}, {701, 12, 102, money(20)}};
    return result;
}

void seed(TestDatabase& database) {
    ts::TradingSystemStorage storage(database.config());
    auto unit = storage.begin();
    auto waiting = checkpoint(0);
    waiting.status = ts::SeriesStatus::WAITING_OUTCOME;
    waiting.current_intent_id = 41;
    waiting.current_group_id = 80;
    waiting.current_step = 0;
    waiting.used_intents = {41};
    waiting.used_intent_keys = {"intent-41"};
    ASSERT_EQ(unit.put_series(waiting, 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_reservation(reservation(), 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_physical_trade(physical_trade(), 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_money_management(mm_state(), 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_roster(roster()).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
}

void seed_multi(TestDatabase& database) {
    ts::TradingSystemStorage storage(database.config());
    auto unit = storage.begin();
    auto waiting = checkpoint(0);
    waiting.status = ts::SeriesStatus::WAITING_OUTCOME;
    waiting.current_intent_id = 41;
    waiting.current_group_id = 80;
    waiting.current_step = 0;
    waiting.used_intents = {41};
    waiting.used_intent_keys = {"intent-41"};
    ASSERT_EQ(unit.put_series(waiting, 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_reservation(multi_reservation(500, 1000, 50), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_reservation(multi_reservation(501, 102, 20), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_physical_trade(physical_trade(), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_physical_trade(physical_trade_for(701, 12, 102), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_money_management(mm_state(), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_roster(multi_roster()).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
}

TEST(TradingSystemBatch5B, AtomicSettlementWritesJournalAndAllProjections) {
    TestDatabase database;
    seed(database);

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        const auto result = unit.apply_settlement_atomic(transition());
        ASSERT_EQ(result.kind, ts::StorageResultKind::APPLIED) << result.message;
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.journal_by_key("settlement-event-900").kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_TRUE(reopened.journal_events("SETTLEMENT_APPLIED").succeeded());
    EXPECT_EQ(reopened.journal_events("SETTLEMENT_APPLIED").values.size(), 1U);
    EXPECT_EQ(reopened.settlement(900).kind, ts::StorageResultKind::APPLIED);
    EXPECT_EQ(reopened.settlements_for_group(80).values.size(), 1U);
    ASSERT_TRUE(reopened.series(12).value);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
    ASSERT_TRUE(reopened.reservation(500).value);
    EXPECT_EQ(reopened.reservation(500).value->state, ts::RiskReservationState::SETTLED);
    EXPECT_EQ(reopened.reservations_for_scope(1000).values.size(), 1U);
    ASSERT_TRUE(reopened.physical_trade(700).value);
    EXPECT_EQ(reopened.physical_trade(700).value->lifecycle_state, "SETTLED");
    EXPECT_EQ(reopened.physical_trades_in_state("SETTLED").values.size(), 1U);
    ASSERT_TRUE(reopened.money_management("mm.alpha").value);
    EXPECT_EQ(reopened.money_management("mm.alpha").value->last_settlement_id, 900U);
}

TEST(TradingSystemBatch5B, PhysicalFactCanCompleteAfterRestartWithSeparateLogicalIdentity) {
    TestDatabase database;
    seed(database);
    auto physical = transition();
    physical.series.reset();
    physical.reservation.reset();
    physical.money_management.reset();
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.record_physical_settlement(physical).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        const auto result = unit.apply_group_completion_atomic(transition());
        ASSERT_EQ(result.kind, ts::StorageResultKind::APPLIED) << result.message;
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.rebuild_group_settlement_index().kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.settlement(900).kind, ts::StorageResultKind::APPLIED);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
    EXPECT_EQ(reopened.journal_by_key("settlement-event-900:logical").kind,
              ts::StorageResultKind::APPLIED);
}

TEST(TradingSystemBatch5B, MixedCurrencyFactsRemainDurableUntilEveryValuationIsResolved) {
    TestDatabase database;
    seed_multi(database);

    auto first = ts::SettlementTransition{};
    first.journal = journal();
    first.journal.event_id = 1301;
    first.journal.idempotency_key = "settlement-event-901";
    first.settlement = settlement_record(CurrencyType::EUR);
    first.settlement.settlement.settlement_id = 901;
    first.settlement.settlement.trade_id = 700;
    first.settlement.valuation.reset();
    first.settlement.logical_outcome_ready = false;
    first.physical_trade = physical_trade(1);
    first.logical_currency = CurrencyType::USD;

    auto second = first;
    second.journal.event_id = 1302;
    second.journal.idempotency_key = "settlement-event-902";
    second.settlement.settlement.settlement_id = 902;
    second.settlement.settlement.trade_id = 701;
    second.settlement.valuation = ts::ValuationFact{
        CurrencyType::EUR, CurrencyType::USD, 1, 1, 123, "test", "fx-902",
        "EXACT_FIXED_POINT", true, 2};
    second.settlement.logical_outcome_ready = true;
    second.physical_trade = physical_trade_for(701, 12, 102, 1);

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(first).kind,
                  ts::StorageResultKind::PENDING_VALUATION);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(second).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    auto completion = second;
    completion.series = checkpoint(1);
    completion.series->series.next_step = 1;
    completion.series->current_outcome = multi_terminal_outcome();
    completion.reservation = multi_reservation(501, 102, 20, 1);
    completion.money_management = mm_state(1);
    completion.expected_series_revision = 0;
    completion.expected_reservation_revision = 0;
    completion.expected_money_management_revision = 0;
    completion.group_reservation_bindings.push_back(
        {700, multi_reservation(500, 1000, 50, 1), 0});
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.apply_group_completion_atomic(completion).kind,
                  ts::StorageResultKind::PENDING_VALUATION);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::ValuationTransition resolution;
    resolution.journal.event_id = 1303;
    resolution.journal.event_type = "VALUATION_RESOLVED";
    resolution.journal.aggregate_id = "group:80";
    resolution.journal.idempotency_key = "valuation-event-901";
    resolution.journal.payload = {"settlement_id", 901};
    resolution.settlement_id = 901;
    resolution.valuation = {CurrencyType::EUR, CurrencyType::USD, 1, 1, 123,
                            "test", "fx-901", "EXACT_FIXED_POINT", true, 2};
    resolution.logical_currency = CurrencyType::USD;
    resolution.series = checkpoint(1);
    resolution.series->series.next_step = 1;
    resolution.series->current_outcome = multi_terminal_outcome();
    resolution.physical_trade = physical_trade(2);
    resolution.reservation = multi_reservation(500, 1000, 50, 1);
    resolution.money_management = mm_state(1);
    resolution.money_management->last_settlement_id = 901;
    resolution.expected_series_revision = 0;
    resolution.expected_physical_trade_revision = 1;
    resolution.expected_reservation_revision = 0;
    resolution.expected_money_management_revision = 0;
    resolution.group_reservation_bindings.push_back(
        {701, multi_reservation(501, 102, 20, 1), 0});
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        const auto result = unit.resolve_pending_valuation_atomic(resolution);
        ASSERT_EQ(result.kind, ts::StorageResultKind::APPLIED) << result.message;
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::TradingSystemStorage reopened(database.config());
    ASSERT_TRUE(reopened.settlement(901).value);
    ASSERT_TRUE(reopened.settlement(902).value);
    EXPECT_EQ(reopened.settlement(901).value->native_profit.currency, CurrencyType::EUR);
    EXPECT_EQ(reopened.settlement(902).value->native_profit.currency, CurrencyType::EUR);
    EXPECT_EQ(reopened.reservation(500).value->state, ts::RiskReservationState::SETTLED);
    EXPECT_EQ(reopened.reservation(501).value->state, ts::RiskReservationState::SETTLED);
}

TEST(TradingSystemBatch5B, TerminalReservationStateFollowsItsOwnPhysicalOutcome) {
    TestDatabase database;
    seed_multi(database);

    auto first = ts::SettlementTransition{};
    first.journal = journal();
    first.journal.event_id = 1401;
    first.journal.idempotency_key = "settlement-event-911";
    first.settlement = settlement_record();
    first.settlement.settlement.settlement_id = 911;
    first.settlement.settlement.trade_id = 700;
    first.physical_trade = physical_trade(1);
    first.logical_currency = CurrencyType::USD;

    auto second = first;
    second.journal.event_id = 1402;
    second.journal.idempotency_key = "settlement-event-912";
    second.settlement.settlement.settlement_id = 912;
    second.settlement.settlement.trade_id = 701;
    second.settlement.settlement.kind = ts::OutcomeKind::CANCELLED;
    second.settlement.settlement.evidence = ts::ExecutionEvidence::NOT_SUBMITTED;
    second.settlement.settlement.net_profit.reset();
    second.settlement.native_profit = money(0);
    second.physical_trade = physical_trade_for(701, 12, 102, 1);
    second.series = checkpoint(1);
    second.series->series.next_step = 1;
    auto outcome = ts::ExecutionGroupAggregator(
        41, {80, 2}, {700, 701}, CurrencyType::USD);
    ASSERT_EQ(outcome.apply(first.settlement.settlement), ts::ProgressionResult::APPLIED);
    ASSERT_EQ(outcome.apply(second.settlement.settlement), ts::ProgressionResult::APPLIED);
    second.series->current_outcome = outcome.outcome();
    second.reservation = multi_reservation(501, 102, 20, 1);
    second.reservation->state = ts::RiskReservationState::RELEASED;
    second.money_management = mm_state(1);
    second.money_management->last_settlement_id = 912;
    second.expected_series_revision = 0;
    second.expected_reservation_revision = 0;
    second.expected_money_management_revision = 0;
    second.group_reservation_bindings.push_back(
        {700, multi_reservation(500, 1000, 50, 1), 0});

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(first).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        const auto result = unit.apply_settlement_atomic(second);
        ASSERT_EQ(result.kind, ts::StorageResultKind::APPLIED) << result.message;
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.reservation(500).value->state, ts::RiskReservationState::SETTLED);
    EXPECT_EQ(reopened.reservation(501).value->state, ts::RiskReservationState::RELEASED);
}

TEST(TradingSystemBatch5B, InvalidCheckpointCannotForgeLogicalProgression) {
    TestDatabase database;
    seed(database);
    auto invalid = transition();
    invalid.series->series.next_step = 99;
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.apply_settlement_atomic(invalid).kind,
                  ts::StorageResultKind::INVALID_STATE);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.settlement(900).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 0U);
}

TEST(TradingSystemBatch5B, PhysicalSettlementDoesNotCompleteMultiMemberGroup) {
    TestDatabase database;
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        auto waiting = checkpoint(0);
        waiting.status = ts::SeriesStatus::WAITING_OUTCOME;
        waiting.current_intent_id = 41;
        waiting.current_group_id = 80;
        waiting.current_step = 0;
        waiting.used_intents = {41};
        waiting.used_intent_keys = {"intent-41"};
        ASSERT_EQ(unit.put_series(waiting, 0).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_reservation(multi_reservation(500, 1000, 50), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_reservation(multi_reservation(501, 102, 20), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_physical_trade(physical_trade(), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_physical_trade(physical_trade_for(701, 12, 102), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_money_management(mm_state(), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_roster(multi_roster()).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::SettlementTransition first;
    first.journal = journal();
    first.journal.event_id = 1101;
    first.journal.idempotency_key = "settlement-event-901";
    first.settlement = {settlement(901, 700), money(100), std::nullopt, true,
                        ts::trading_storage_schema_version};
    first.physical_trade = physical_trade(1);
    first.logical_currency = CurrencyType::USD;
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(first).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::ExecutionGroupAggregator partial_group(41, {80, 2}, {700, 701}, CurrencyType::USD);
    ASSERT_EQ(partial_group.apply(first.settlement.settlement), ts::ProgressionResult::APPLIED);
    ts::SettlementTransition premature = first;
    premature.journal.event_id = 1102;
    premature.journal.idempotency_key = "settlement-event-901-premature";
    premature.physical_trade = physical_trade(1);
    premature.expected_physical_trade_revision = 1;
    premature.series = checkpoint(1);
    premature.series->current_outcome = partial_group.outcome();
    premature.reservation = multi_reservation(500, 1000, 50, 1);
    premature.money_management = mm_state(1);
    premature.money_management->last_settlement_id = 901;
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.apply_settlement_atomic(premature).kind,
                  ts::StorageResultKind::INVALID_STATE);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }

    ts::SettlementTransition second;
    second.journal = journal();
    second.journal.event_id = 1103;
    second.journal.idempotency_key = "settlement-event-902";
    second.settlement = {settlement(902, 701), money(100), std::nullopt, true,
                         ts::trading_storage_schema_version};
    second.physical_trade = physical_trade_for(701, 12, 102, 1);
    second.reservation = multi_reservation(501, 102, 20, 1);
    second.series = checkpoint(1);
    second.series->series.next_step = 1;
    second.expected_series_revision = 0;
    second.money_management = mm_state(1);
    second.money_management->last_settlement_id = 902;
    second.group_reservations = {multi_reservation(500, 1000, 50, 1)};
    second.expected_group_reservation_revisions = {0};
    second.logical_currency = CurrencyType::USD;
    ts::ExecutionGroupAggregator complete_group(41, {80, 2}, {700, 701}, CurrencyType::USD);
    ASSERT_EQ(complete_group.apply(first.settlement.settlement), ts::ProgressionResult::APPLIED);
    ASSERT_EQ(complete_group.apply(second.settlement.settlement), ts::ProgressionResult::APPLIED);
    second.series->current_outcome = complete_group.outcome();
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        const auto result = unit.apply_settlement_atomic(second);
        ASSERT_EQ(result.kind, ts::StorageResultKind::APPLIED) << result.message;
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::TradingSystemStorage reopened(database.config());
    ASSERT_TRUE(reopened.reservation(500).value);
    ASSERT_TRUE(reopened.reservation(501).value);
    EXPECT_EQ(reopened.reservation(500).value->state, ts::RiskReservationState::SETTLED);
    EXPECT_EQ(reopened.reservation(501).value->state, ts::RiskReservationState::SETTLED);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
    EXPECT_EQ(reopened.money_management("mm.alpha").value->last_settlement_id, 902U);
    EXPECT_EQ(reopened.physical_trade(700).value->state_revision, 1U);
    EXPECT_EQ(reopened.physical_trade(701).value->state_revision, 1U);
    EXPECT_EQ(reopened.journal_by_key("settlement-event-901-premature").kind,
              ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(reopened.settlements_for_group(80).values.size(), 2U);
}

TEST(TradingSystemBatch5B, ImmediateValuationMustMatchNormalizedGroupOutcome) {
    TestDatabase database;
    seed(database);

    auto incorrect = transition();
    incorrect.journal.event_id = 1201;
    incorrect.journal.idempotency_key = "settlement-event-900-fx-incorrect";
    incorrect.settlement = settlement_record(CurrencyType::EUR);
    incorrect.settlement.valuation = ts::ValuationFact{
        CurrencyType::EUR, CurrencyType::USD, 2, 1, 123, "test", "fx-immediate",
        "EXACT_FIXED_POINT", true, 2};
    incorrect.journal.payload = ts::detail::settlement_to_json(incorrect.settlement.settlement);
    incorrect.series->current_outcome = terminal_outcome();
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.apply_settlement_atomic(incorrect).kind,
                  ts::StorageResultKind::CONFLICT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }

    auto correct = incorrect;
    correct.journal.event_id = 1202;
    correct.journal.idempotency_key = "settlement-event-900-fx-correct";
    correct.series->current_outcome.net_profit = money(200);
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(correct).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.settlement(900).value->logical_outcome_ready, true);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
}

TEST(TradingSystemBatch5B, FixedPointHalfUpHandlesHalfNegativeAndOverflowCases) {
    const auto half = ts::ValuationFact{
        CurrencyType::EUR, CurrencyType::USD, 1, 2, 123, "test", "fx-half",
        "ROUND_HALF_UP", true, 2};
    ASSERT_EQ(ts::detail::convert_money(money(1, CurrencyType::EUR), half,
                                        CurrencyType::USD)->units, 1);
    ASSERT_EQ(ts::detail::convert_money(money(-1, CurrencyType::EUR), half,
                                        CurrencyType::USD)->units, -1);

    const auto exact = ts::ValuationFact{
        CurrencyType::EUR, CurrencyType::USD, 1, 1, 123, "test", "fx-exact",
        "EXACT_FIXED_POINT", true, 2};
    ASSERT_EQ(ts::detail::convert_money(
                  {std::numeric_limits<std::int64_t>::min(), 2, CurrencyType::EUR},
                  exact, CurrencyType::USD)->units,
              (std::numeric_limits<std::int64_t>::min)());
    EXPECT_FALSE(ts::detail::convert_money(
        {(std::numeric_limits<std::int64_t>::max)(), 2, CurrencyType::EUR},
        {CurrencyType::EUR, CurrencyType::USD, 2, 1, 123, "test", "fx-overflow",
         "EXACT_FIXED_POINT", true, 2}, CurrencyType::USD));
}

TEST(TradingSystemBatch5B, DuplicateSettlementDoesNotAdvanceAnyProjection) {
    TestDatabase database;
    seed(database);
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(transition()).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.apply_settlement_atomic(transition()).kind,
                  ts::StorageResultKind::DUPLICATE);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
    EXPECT_EQ(reopened.reservation(500).value->state_revision, 1U);
    EXPECT_EQ(reopened.money_management("mm.alpha").value->state_revision, 1U);
}

TEST(TradingSystemBatch5B, FailedRevisionCannotCommitPartialSettlement) {
    TestDatabase database;
    seed(database);
    auto invalid_revision = transition();
    invalid_revision.expected_series_revision = 1;

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.apply_settlement_atomic(invalid_revision).kind,
                  ts::StorageResultKind::REVISION_CONFLICT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }

    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.settlement(900).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(reopened.journal_by_key("settlement-event-900").kind,
              ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 0U);
}

TEST(TradingSystemBatch5B, TerminalSettlementRequiresCompleteConsistentProjectionSet) {
    TestDatabase database;
    seed(database);
    auto incomplete = transition();
    incomplete.physical_trade.reset();
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.apply_settlement_atomic(incomplete).kind,
                  ts::StorageResultKind::INVALID_ARGUMENT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        auto mismatch = transition();
        mismatch.physical_trade->risk_scope_id = 9999;
        EXPECT_EQ(unit.apply_settlement_atomic(mismatch).kind,
                  ts::StorageResultKind::CONFLICT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.settlement(900).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 0U);
}

TEST(TradingSystemBatch5B, RosterIsImmutableAndSurvivesReopen) {
    TestDatabase database;
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.put_roster(multi_roster()).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    auto changed = multi_roster();
    changed.members[1].account_id = 99;
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.put_roster(changed).kind, ts::StorageResultKind::CONFLICT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    ts::TradingSystemStorage reopened(database.config());
    ASSERT_TRUE(reopened.roster(80).value);
    EXPECT_EQ(reopened.roster(80).value->members[1].account_id, 12U);
}

TEST(TradingSystemBatch5B, NativeSettlementRemainsPendingWithoutValuation) {
    TestDatabase database;
    seed(database);
    ts::TradingSystemStorage storage(database.config());
    auto value = settlement_record(CurrencyType::EUR);
    auto pending = transition();
    pending.settlement = value;
    pending.journal.payload = ts::detail::settlement_to_json(value.settlement);
    pending.series.reset();
    pending.reservation.reset();
    pending.money_management.reset();
    auto unit = storage.begin();
    EXPECT_EQ(unit.apply_settlement_atomic(pending).kind,
              ts::StorageResultKind::PENDING_VALUATION);
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    ASSERT_TRUE(storage.settlement(900).value);
    EXPECT_FALSE(storage.settlement(900).value->logical_outcome_ready);
    EXPECT_EQ(storage.journal_by_key("settlement-event-900").kind,
              ts::StorageResultKind::APPLIED);
    EXPECT_EQ(storage.pending_valuations().values.size(), 1U);
}

TEST(TradingSystemBatch5B, StaleValuationRemainsPending) {
    TestDatabase database;
    seed(database);
    ts::SettlementTransition pending = transition();
    pending.settlement = settlement_record(CurrencyType::EUR);
    pending.settlement.valuation = ts::ValuationFact{
        CurrencyType::EUR, CurrencyType::USD, 11, 10, 123, "test", "fx-1",
        "EXACT_FIXED_POINT", false, 2};
    pending.journal.payload = ts::detail::settlement_to_json(pending.settlement.settlement);
    pending.series.reset();
    pending.reservation.reset();
    pending.money_management.reset();

    ts::TradingSystemStorage storage(database.config());
    auto unit = storage.begin();
    EXPECT_EQ(unit.apply_settlement_atomic(pending).kind,
              ts::StorageResultKind::PENDING_VALUATION);
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    ASSERT_TRUE(storage.settlement(900).value);
    EXPECT_FALSE(storage.settlement(900).value->logical_outcome_ready);
}

TEST(TradingSystemBatch5B, PendingValuationResolvesExactlyOnceAfterReopen) {
    TestDatabase database;
    seed(database);
    {
        ts::TradingSystemStorage storage(database.config());
        auto pending = transition();
        pending.settlement = settlement_record(CurrencyType::EUR);
        pending.journal.payload = ts::detail::settlement_to_json(pending.settlement.settlement);
        pending.series.reset();
        pending.reservation.reset();
        pending.money_management.reset();
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(pending).kind,
                  ts::StorageResultKind::PENDING_VALUATION);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::ValuationTransition resolution;
    resolution.journal.event_id = 1002;
    resolution.journal.event_type = "VALUATION_RESOLVED";
    resolution.journal.aggregate_id = "group:80";
    resolution.journal.idempotency_key = "valuation-event-900";
    resolution.journal.payload = {"settlement_id", 900};
    resolution.settlement_id = 900;
    resolution.valuation = {CurrencyType::EUR, CurrencyType::USD, 1, 1, 123,
                            "test", "fx-900", "EXACT_FIXED_POINT", true, 2};
    resolution.logical_currency = CurrencyType::USD;
    resolution.series = checkpoint(1);
    resolution.series->series.next_step = 1;
    resolution.expected_series_revision = 0;
    resolution.series->current_outcome = terminal_outcome();
    resolution.physical_trade = physical_trade(2);
    resolution.reservation = reservation(1);
    resolution.money_management = mm_state(1);
    resolution.expected_physical_trade_revision = 1;

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.resolve_pending_valuation_atomic(resolution).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        ASSERT_TRUE(storage.valuation_resolution(900).value);
        EXPECT_EQ(storage.valuation_resolution(900).value->logical_profit.units, 100);
        EXPECT_FALSE(storage.settlement(900).value->logical_outcome_ready);
        EXPECT_TRUE(storage.pending_valuations().values.empty());
        auto unit = storage.begin();
        EXPECT_EQ(unit.resolve_pending_valuation_atomic(resolution).kind,
                  ts::StorageResultKind::DUPLICATE);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
    EXPECT_EQ(reopened.money_management("mm.alpha").value->state_revision, 1U);
}

TEST(TradingSystemBatch5B, UnsupportedSchemaIsRejectedBeforeWrite) {
    TestDatabase database;
    auto value = roster();
    value.schema_version = 99;
    ts::TradingSystemStorage storage(database.config());
    auto unit = storage.begin();
    EXPECT_EQ(unit.put_roster(value).kind, ts::StorageResultKind::UNSUPPORTED_SCHEMA);
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    EXPECT_EQ(storage.roster(80).kind, ts::StorageResultKind::NOT_FOUND);
}

TEST(TradingSystemBatch5B, RosterRejectsDuplicatePhysicalTradeIdentity) {
    TestDatabase database;
    auto value = multi_roster();
    value.members[1].trade_id = value.members[0].trade_id;
    ts::TradingSystemStorage storage(database.config());
    auto unit = storage.begin();
    EXPECT_EQ(unit.put_roster(value).kind, ts::StorageResultKind::INVALID_ARGUMENT);
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
}

TEST(TradingSystemBatch5B, AccountIdentityTombstoneAndRiskScopeConflictAreDurable) {
    TestDatabase database;
    ts::AccountIdentityRecord canonical{
        11, "IntradeBar", "provider-account-1", "DEMO", 11, 1000, 0, true,
        ts::trading_storage_schema_version};
    ts::AccountIdentityRecord alias{
        12, "IntradeBar", "provider-account-1", "DEMO", 11, 1000, 0, true,
        ts::trading_storage_schema_version};
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.put_account(canonical).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_account(alias).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    {
        ts::TradingSystemStorage storage(database.config());
        ASSERT_TRUE(storage.account(11).value);
        EXPECT_TRUE(storage.account(11).value->active);
        EXPECT_EQ(storage.accounts_for_scope(1000).values.size(), 2U);

        auto tombstone = *storage.account(12).value;
        tombstone.active = false;
        tombstone.identity_revision = 1;
        {
            auto unit = storage.begin();
            ASSERT_EQ(unit.put_account(tombstone).kind, ts::StorageResultKind::APPLIED);
            ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
        }
        EXPECT_FALSE(storage.account(12).value->active);

        auto conflicting = canonical;
        conflicting.account_id = 13;
        conflicting.canonical_account_id = 13;
        conflicting.risk_scope_id = 2000;
        {
            auto unit = storage.begin();
            EXPECT_EQ(unit.put_account(conflicting).kind, ts::StorageResultKind::CONFLICT);
            EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
        }
    }
    bool gate_blocked = false;
    {
        ts::TradingSystemStorage reopened(database.config());
        ASSERT_TRUE(reopened.risk_scope_gate(1000).value);
        ASSERT_TRUE(reopened.risk_scope_gate(2000).value);
        gate_blocked = reopened.risk_scope_gate(1000).value->state ==
                       ts::RiskScopeGateState::BLOCKED;
        EXPECT_TRUE(gate_blocked);
        EXPECT_EQ(reopened.risk_scope_gate(2000).value->state,
                  ts::RiskScopeGateState::BLOCKED);
    }

    ts::RiskAdmissionConfig blocked_config;
    blocked_config.scope = {ts::RiskScopeKind::ACCOUNT, 1000};
    ts::RiskAdmission blocked_owner(blocked_config);
    ASSERT_EQ(blocked_owner.restore_admission_gate(gate_blocked).kind,
              ts::RiskAdmissionKind::APPLIED);
    ts::RiskRequest blocked_request;
    blocked_request.intent_id = 42;
    blocked_request.execution_group = {81, 1};
    blocked_request.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
    blocked_request.intent_status = ts::IntentStatus::PROPOSED;
    blocked_request.idempotency_key = "blocked-risk-42";
    blocked_request.stake = money(10);
    blocked_request.potential_risk = money(10);
    EXPECT_EQ(blocked_owner.reserve(blocked_request, 1000).reason,
              ts::RiskAdmissionReason::RISK_SCOPE_BLOCKED);
}

TEST(TradingSystemBatch5B, SeriesOwnerHydratesReceiptAndIdempotencyHistory) {
    ts::TradeSettlement first = settlement(901, 700);
    ts::TradeSettlement second = settlement(902, 701);
    ts::ExecutionGroupAggregator group(41, {80, 2}, {700, 701}, CurrencyType::USD);
    ASSERT_TRUE(group.valid());
    ASSERT_EQ(group.apply(first), ts::ProgressionResult::APPLIED);
    ASSERT_EQ(group.apply(second), ts::ProgressionResult::APPLIED);

    ts::SeriesSnapshot snapshot;
    snapshot.series = {12, 7, 0, 1};
    snapshot.status = ts::SeriesStatus::ACTIVE;
    snapshot.current_intent_id = 0;
    snapshot.current_group_id = 0;
    snapshot.current_step = 0;
    snapshot.outcome = group.outcome();

    ts::SeriesProgression owner({12, 7, 0, 0});
    EXPECT_EQ(owner.hydrate(snapshot, ts::ExecutionGroupAggregator(
                  41, {80, 2}, {700, 701}, CurrencyType::USD),
                  {first, second}, {41}, {"intent-41"}),
              ts::ProgressionResult::APPLIED);
    EXPECT_EQ(owner.apply(first, 1), ts::ProgressionResult::DUPLICATE);
    EXPECT_EQ(owner.snapshot().series.state_revision, 1U);
    EXPECT_EQ(owner.snapshot().status, ts::SeriesStatus::ACTIVE);
}

TEST(TradingSystemBatch5B, SeriesHydrationRejectsTerminalGroupMarkedWaiting) {
    const auto receipt = settlement(905, 700);
    ts::ExecutionGroupAggregator group(41, {80, 1}, {700}, CurrencyType::USD);
    ASSERT_TRUE(group.valid());
    ASSERT_EQ(group.apply(receipt), ts::ProgressionResult::APPLIED);

    ts::SeriesSnapshot snapshot;
    snapshot.series = {12, 7, 0, 1};
    snapshot.status = ts::SeriesStatus::WAITING_OUTCOME;
    snapshot.current_intent_id = 41;
    snapshot.current_group_id = 80;
    snapshot.current_step = 0;
    snapshot.outcome = group.outcome();

    ts::SeriesProgression owner({12, 7, 0, 0});
    EXPECT_EQ(owner.hydrate(
                  snapshot, ts::ExecutionGroupAggregator(
                               41, {80, 1}, {700}, CurrencyType::USD),
                  {receipt}, {41}, {"intent-41"}),
              ts::ProgressionResult::INVALID_STATE);
}

TEST(TradingSystemBatch5B, RiskOwnerHydratesActiveReservationAndSequence) {
    ts::RiskAdmissionConfig admission_config;
    admission_config.scope = {ts::RiskScopeKind::ACCOUNT, 1000};
    ts::RiskAdmission owner(admission_config);
    ts::RiskAccountSnapshot account;
    account.availability = ts::RiskAvailability::SUPPORTED;
    account.available_balance = money(100);
    account.captured_at_ms = 1000;
    account.revision = 7;

    ts::RiskReservation active;
    active.reservation_id = 500;
    active.scope = admission_config.scope;
    active.intent_id = 41;
    active.execution_group = {0, 1};
    active.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
    active.idempotency_key = "risk-41";
    active.stake = money(10);
    active.potential_risk = money(50);
    active.state = ts::RiskReservationState::RESERVED;
    active.account_revision = 7;

    ASSERT_EQ(owner.hydrate(account, {active}, 501).kind,
              ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserved_risk_units(), 50);
    ts::RiskRequest request;
    request.intent_id = 41;
    request.execution_group = {0, 1};
    request.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
    request.intent_status = ts::IntentStatus::PROPOSED;
    request.idempotency_key = "risk-41";
    request.stake = money(10);
    request.potential_risk = money(50);
    EXPECT_EQ(owner.reserve(request, 1000).kind, ts::RiskAdmissionKind::DUPLICATE);
}

TEST(TradingSystemBatch5B, RiskOwnerRehydratesReservationFromMdbxAfterRestart) {
    TestDatabase database;
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.put_reservation(reservation(), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::ReservationRecord persisted;
    {
        ts::TradingSystemStorage storage(database.config());
        ASSERT_TRUE(storage.reservation(500).value);
        persisted = *storage.reservation(500).value;
    }

    ts::RiskAdmissionConfig admission_config;
    admission_config.scope = {ts::RiskScopeKind::ACCOUNT, 1000};
    ts::RiskAdmission owner(admission_config);
    ts::RiskAccountSnapshot account;
    account.availability = ts::RiskAvailability::SUPPORTED;
    account.available_balance = money(100);
    account.captured_at_ms = 1000;
    account.revision = 7;
    ASSERT_EQ(owner.hydrate(account, {persisted.to_risk_reservation()}, 501).kind,
              ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserved_risk_units(), 50);

    ts::TradeIntent intent;
    intent.intent_id = 41;
    intent.execution_group = {80, 1};
    intent.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
    intent.status = ts::IntentStatus::PROPOSED;
    intent.idempotency_key = "risk-41";
    auto request = ts::RiskRequest::from_intent(intent, money(50), money(50));
    EXPECT_EQ(owner.reserve(request, 1000).kind, ts::RiskAdmissionKind::DUPLICATE);

    auto invalid = persisted;
    invalid.reservation_id = 501;
    invalid.potential_risk.units = -1;
    ts::TradingSystemStorage storage(database.config());
    auto unit = storage.begin();
    EXPECT_EQ(unit.put_reservation(invalid, 0).kind,
              ts::StorageResultKind::INVALID_ARGUMENT);
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
}

TEST(TradingSystemBatch5B, SeriesHydrationReopensMultipleGroupsAndAdvancesCurrentOnce) {
    TestDatabase database;
    const auto first = group_settlement(901, 101, 80, 801, 10);
    const auto second = group_settlement(902, 102, 81, 811, 20);
    const auto current_first = group_settlement(903, 103, 82, 821, 30);

    ts::ExecutionGroupAggregator current_group(103, {82, 2}, {821, 822}, CurrencyType::USD);
    ASSERT_TRUE(current_group.valid());
    ASSERT_EQ(current_group.apply(current_first), ts::ProgressionResult::APPLIED);

    ts::SeriesCheckpoint persisted = checkpoint(3);
    persisted.series.next_step = 2;
    persisted.status = ts::SeriesStatus::WAITING_OUTCOME;
    persisted.current_intent_id = 103;
    persisted.current_group_id = 82;
    persisted.current_step = 2;
    persisted.current_outcome = current_group.outcome();
    persisted.receipts = {first, second, current_first};
    persisted.used_intents = {101, 102, 103};
    persisted.used_intent_keys = {"intent-101", "intent-102", "intent-103"};

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        auto initial = persisted;
        initial.series.next_step = 0;
        initial.series.state_revision = 0;
        initial.status = ts::SeriesStatus::ACTIVE;
        initial.current_intent_id = 0;
        initial.current_group_id = 0;
        initial.current_step = 0;
        initial.current_outcome = {};
        initial.receipts.clear();
        initial.used_intents.clear();
        initial.used_intent_keys.clear();
        ASSERT_EQ(unit.put_series(initial, 0).kind, ts::StorageResultKind::APPLIED);
        auto revision_one = persisted;
        revision_one.series.state_revision = 1;
        ASSERT_EQ(unit.put_series(revision_one, 0).kind, ts::StorageResultKind::APPLIED);
        auto revision_two = persisted;
        revision_two.series.state_revision = 2;
        ASSERT_EQ(unit.put_series(revision_two, 1).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_series(persisted, 2).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::SeriesCheckpoint reopened_checkpoint;
    {
        ts::TradingSystemStorage storage(database.config());
        ASSERT_TRUE(storage.series(12).value);
        reopened_checkpoint = *storage.series(12).value;
    }

    ts::SeriesSnapshot snapshot;
    snapshot.series = reopened_checkpoint.series;
    snapshot.status = reopened_checkpoint.status;
    snapshot.current_intent_id = reopened_checkpoint.current_intent_id;
    snapshot.current_group_id = reopened_checkpoint.current_group_id;
    snapshot.current_step = reopened_checkpoint.current_step;
    snapshot.outcome = reopened_checkpoint.current_outcome;
    ts::SeriesProgression owner({12, 7, 0, 0});
    ASSERT_EQ(owner.hydrate(
                  snapshot, ts::ExecutionGroupAggregator(
                               103, {82, 2}, {821, 822}, CurrencyType::USD),
                  reopened_checkpoint.receipts,
                  {101, 102, 103},
                  {"intent-101", "intent-102", "intent-103"}),
              ts::ProgressionResult::APPLIED);

    EXPECT_EQ(owner.apply(first, 3), ts::ProgressionResult::DUPLICATE);
    EXPECT_EQ(owner.apply(second, 3), ts::ProgressionResult::DUPLICATE);
    EXPECT_EQ(owner.apply(current_first, 3), ts::ProgressionResult::DUPLICATE);

    const auto current_second = group_settlement(904, 103, 82, 822, 40);
    ASSERT_EQ(owner.apply(current_second, 3), ts::ProgressionResult::APPLIED);
    EXPECT_EQ(owner.snapshot().series.state_revision, 4U);
    EXPECT_EQ(owner.snapshot().series.next_step, 3U);
    EXPECT_EQ(owner.snapshot().status, ts::SeriesStatus::ACTIVE);
    EXPECT_EQ(owner.apply(current_second, 4), ts::ProgressionResult::DUPLICATE);
    EXPECT_EQ(owner.snapshot().series.state_revision, 4U);
}

TEST(TradingSystemBatch5B, LateProjectionFailureRollsBackAllSettlementFacts) {
    TestDatabase database;
    seed(database);
    auto late_failure = transition();
    late_failure.expected_money_management_revision = 1;

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.apply_settlement_atomic(late_failure).kind,
                  ts::StorageResultKind::REVISION_CONFLICT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }

    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.journal_by_key("settlement-event-900").kind,
              ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(reopened.settlement(900).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 0U);
    EXPECT_EQ(reopened.physical_trade(700).value->state_revision, 0U);
    EXPECT_EQ(reopened.reservation(500).value->state_revision, 0U);
    EXPECT_EQ(reopened.money_management("mm.alpha").value->state_revision, 0U);
}

TEST(TradingSystemBatch5B, ReadPathRejectsDurableRecordWithoutSchema) {
    TestDatabase database;
    {
        ts::TradingSystemStorage storage(database.config());
        mdbxc::KeyValueTable<std::string, std::string> rosters(
            storage.connection(), "ts_rosters");
        rosters.insert_or_assign("999", R"({"group_id":999})");
        rosters.insert_or_assign(
            "998", R"({"schema_version":1,"group_id":0,"intent_id":41,"expected_members":1,"members":[]})");
    }

    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.roster(999).kind, ts::StorageResultKind::STORAGE_ERROR);
    EXPECT_EQ(reopened.roster(998).kind, ts::StorageResultKind::STORAGE_ERROR);
}

TEST(TradingSystemBatch5B, DurableSequencePersistsAndRollsBackWithUnitOfWork) {
    TestDatabase database;
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.next_id("settlement").value, 1U);
        EXPECT_EQ(unit.next_id("settlement").value, 2U);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.next_id("settlement").value, 3U);
        unit.rollback();
    }
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.next_id("settlement").value, 3U);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
