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

ts::ReservationRecord three_reservation(
    std::uint64_t reservation_id, std::uint64_t risk_scope_id,
    std::int64_t amount, std::uint64_t revision = 0) {
    auto result = multi_reservation(reservation_id, risk_scope_id, amount, revision);
    result.execution_group = {80, 3};
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
    result.series->receipts = {settlement()};
    result.series->used_intents = {41};
    result.series->used_intent_keys = {"intent-41"};
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
    result.members = {{700, 11, 1000, money(50)}};
    result.reservation_bindings = {{700, 500, 1000, money(50), money(50), "risk-41"}};
    return result;
}

ts::ExecutionRosterRecord multi_roster() {
    ts::ExecutionRosterRecord result;
    result.group_id = 80;
    result.intent_id = 41;
    result.expected_members = 2;
    result.members = {{700, 11, 1000, money(50)}, {701, 12, 102, money(20)}};
    result.reservation_bindings = {
        {700, 500, 1000, money(50), money(50), "risk-500"},
        {701, 501, 102, money(20), money(20), "risk-501"}};
    return result;
}

ts::ExecutionRosterRecord three_roster() {
    ts::ExecutionRosterRecord result;
    result.group_id = 80;
    result.intent_id = 41;
    result.expected_members = 3;
    result.members = {{700, 11, 1000, money(50)},
                      {701, 12, 102, money(20)},
                      {702, 13, 103, money(30)}};
    result.reservation_bindings = {
        {700, 500, 1000, money(50), money(50), "risk-500"},
        {701, 501, 102, money(20), money(20), "risk-501"},
        {702, 502, 103, money(30), money(30), "risk-502"}};
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

void seed_three(TestDatabase& database) {
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
    ASSERT_EQ(unit.put_reservation(three_reservation(500, 1000, 50), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_reservation(three_reservation(501, 102, 20), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_reservation(three_reservation(502, 103, 30), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_physical_trade(physical_trade(), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_physical_trade(physical_trade_for(701, 12, 102), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_physical_trade(physical_trade_for(702, 13, 103), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_money_management(mm_state(), 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_roster(three_roster()).kind, ts::StorageResultKind::APPLIED);
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
    completion.series->receipts = {settlement(901, 700), settlement(902, 701)};
    completion.series->used_intents = {41};
    completion.series->used_intent_keys = {"intent-41"};
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
    resolution.series->receipts = {settlement(901, 700), settlement(902, 701)};
    resolution.series->used_intents = {41};
    resolution.series->used_intent_keys = {"intent-41"};
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

TEST(TradingSystemBatch5B, ThreeMemberCompletionAdvancesOneRevisionAndPreservesHistory) {
    TestDatabase database;
    seed_three(database);

    std::vector<ts::SettlementTransition> physical;
    for (std::size_t index = 0; index != 3; ++index) {
        const auto settlement_id = static_cast<std::uint64_t>(930 + index);
        const auto trade_id = static_cast<std::uint64_t>(700 + index);
        ts::SettlementTransition value;
        value.journal = journal();
        value.journal.event_id = 1930 + index;
        value.journal.idempotency_key = "settlement-event-" + std::to_string(settlement_id);
        value.settlement = {settlement(settlement_id, trade_id), money(100), std::nullopt,
                            true, ts::trading_storage_schema_version};
        value.physical_trade = index == 0
            ? physical_trade(1)
            : index == 1
                ? physical_trade_for(trade_id, 12, 102, 1)
                : physical_trade_for(trade_id, 13, 103, 1);
        value.logical_currency = CurrencyType::USD;
        physical.push_back(value);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        for (const auto& value : physical) {
            auto unit = storage.begin();
        const auto result = unit.apply_settlement_atomic(value);
        ASSERT_EQ(result.kind, ts::StorageResultKind::APPLIED) << result.message;
            ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
        }
    }

    ts::ExecutionGroupAggregator aggregate(41, {80, 3}, {700, 701, 702}, CurrencyType::USD);
    for (const auto& value : physical) {
        ASSERT_EQ(aggregate.apply(value.settlement.settlement), ts::ProgressionResult::APPLIED);
    }
    ts::SettlementTransition completion = physical.back();
    completion.series = checkpoint(1);
    completion.series->series.next_step = 1;
    completion.series->current_outcome = aggregate.outcome();
    completion.series->receipts = {
        physical[0].settlement.settlement,
        physical[1].settlement.settlement,
        physical[2].settlement.settlement};
    completion.series->used_intents = {41};
    completion.series->used_intent_keys = {"intent-41"};
    completion.physical_trade = physical_trade_for(702, 13, 103, 1);
    completion.reservation = three_reservation(502, 103, 30, 1);
    completion.money_management = mm_state(1);
    completion.money_management->last_settlement_id = 932;
    completion.expected_series_revision = 0;
    completion.expected_reservation_revision = 0;
    completion.expected_money_management_revision = 0;
    completion.group_reservation_bindings = {
        {700, three_reservation(500, 1000, 50, 1), 0},
        {701, three_reservation(501, 102, 20, 1), 0}};

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        const auto result = unit.apply_group_completion_atomic(completion);
        ASSERT_EQ(result.kind, ts::StorageResultKind::APPLIED) << result.message;
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        ASSERT_EQ(storage.series(12).value->series.state_revision, 1U);
        auto unit = storage.begin();
        const auto duplicate = unit.apply_group_completion_atomic(completion);
        EXPECT_EQ(duplicate.kind, ts::StorageResultKind::DUPLICATE) << duplicate.message;
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    {
        ts::TradingSystemStorage storage(database.config());
        auto next = *storage.series(12).value;
        next.series.state_revision = 2;
        next.status = ts::SeriesStatus::WAITING_OUTCOME;
        next.current_intent_id = 42;
        next.current_group_id = 81;
        next.current_step = 1;
        next.current_outcome = {};
        next.used_intents.push_back(42);
        next.used_intent_keys.push_back("intent-42");
        ts::ExecutionRosterRecord next_roster;
        next_roster.group_id = 81;
        next_roster.intent_id = 42;
        next_roster.expected_members = 1;
        next_roster.members = {{800, 14, 104, money(10)}};
        next_roster.reservation_bindings = {{800, 600, 104, money(10), money(10), "risk-600"}};
        auto unit = storage.begin();
        auto next_reservation = reservation();
        next_reservation.reservation_id = 600;
        next_reservation.risk_scope_id = 104;
        next_reservation.intent_id = 42;
        next_reservation.execution_group = {81, 1};
        next_reservation.stake = money(10);
        next_reservation.potential_risk = money(10);
        next_reservation.idempotency_key = "risk-600";
        ASSERT_EQ(unit.put_reservation(next_reservation, 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_roster(next_roster).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_series(next, 1).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(database.config());
    ASSERT_TRUE(reopened.series(12).value);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 2U);
    EXPECT_EQ(reopened.series(12).value->receipts.size(), 3U);
    EXPECT_EQ(reopened.series(12).value->used_intents.size(), 2U);
    EXPECT_EQ(reopened.settlements_for_group(80).values.size(), 3U);
}

TEST(TradingSystemBatch5B, IndependentValuationsSurviveRestartBeforeLogicalCompletion) {
    TestDatabase database;
    seed_multi(database);

    auto native_a = settlement_record(CurrencyType::EUR);
    native_a.settlement = settlement(941, 700);
    native_a.settlement.net_profit = money(10000, CurrencyType::EUR);
    native_a.native_profit = money(10000, CurrencyType::EUR);
    native_a.logical_outcome_ready = false;
    auto native_b = settlement_record(CurrencyType::RUB);
    native_b.settlement = settlement(942, 701);
    native_b.settlement.kind = ts::OutcomeKind::LOSS;
    native_b.settlement.net_profit = money(-500000, CurrencyType::RUB);
    native_b.native_profit = money(-500000, CurrencyType::RUB);
    native_b.logical_outcome_ready = false;

    auto persist_native = [](ts::SettlementRecord record, std::uint64_t event_id,
                              ts::PhysicalTradeRecord trade) {
        ts::SettlementTransition value;
        value.journal.event_id = event_id;
        value.journal.event_type = "SETTLEMENT_APPLIED";
        value.journal.aggregate_id = "group:80";
        value.journal.idempotency_key = "settlement-event-" +
                                         std::to_string(record.settlement.settlement_id);
        value.settlement = std::move(record);
        value.physical_trade = trade;
        value.logical_currency = CurrencyType::USD;
        return value;
    };
    const auto physical_a = persist_native(native_a, 1941, physical_trade(1));
    const auto physical_b = persist_native(native_b, 1942, physical_trade_for(701, 12, 102, 1));
    for (const auto& value : {physical_a, physical_b}) {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(value).kind,
                  ts::StorageResultKind::PENDING_VALUATION);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::ValuationTransition resolution_a;
    resolution_a.journal.event_id = 1943;
    resolution_a.journal.event_type = "VALUATION_RESOLVED";
    resolution_a.journal.aggregate_id = "group:80";
    resolution_a.journal.idempotency_key = "valuation-event-941";
    resolution_a.settlement_id = 941;
    resolution_a.valuation = {CurrencyType::EUR, CurrencyType::USD, 1, 1, 100,
                              "test", "fx-941", "EXACT_FIXED_POINT", true, 2};
    resolution_a.logical_currency = CurrencyType::USD;
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.record_valuation_resolution_atomic(resolution_a).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        EXPECT_EQ(storage.series(12).value->series.state_revision, 0U);
        EXPECT_TRUE(storage.valuation_resolution(941).value.has_value());
    }
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        auto conflicting = resolution_a;
        conflicting.valuation.rate_numerator = 2;
        conflicting.valuation.snapshot_id = "fx-941-conflict";
        EXPECT_EQ(unit.record_valuation_resolution_atomic(conflicting).kind,
                  ts::StorageResultKind::CONFLICT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }

    ts::TradeSettlement logical_a = settlement(941, 700);
    logical_a.net_profit = money(10000);
    ts::TradeSettlement logical_b = settlement(942, 701);
    logical_b.kind = ts::OutcomeKind::LOSS;
    logical_b.net_profit = money(-5000);
    ts::ExecutionGroupAggregator outcome_aggregate(
        41, {80, 2}, {700, 701}, CurrencyType::USD);
    ASSERT_EQ(outcome_aggregate.apply(logical_a), ts::ProgressionResult::APPLIED);
    ASSERT_EQ(outcome_aggregate.apply(logical_b), ts::ProgressionResult::APPLIED);

    ts::ValuationTransition resolution_b;
    resolution_b.journal.event_id = 1944;
    resolution_b.journal.event_type = "VALUATION_RESOLVED";
    resolution_b.journal.aggregate_id = "group:80";
    resolution_b.journal.idempotency_key = "valuation-event-942";
    resolution_b.settlement_id = 942;
    resolution_b.valuation = {CurrencyType::RUB, CurrencyType::USD, 1, 100, 101,
                              "test", "fx-942", "EXACT_FIXED_POINT", true, 2};
    resolution_b.logical_currency = CurrencyType::USD;
    resolution_b.series = checkpoint(1);
    resolution_b.series->series.next_step = 1;
    resolution_b.series->current_outcome = outcome_aggregate.outcome();
    resolution_b.series->receipts = {logical_a, logical_b};
    resolution_b.series->used_intents = {41};
    resolution_b.series->used_intent_keys = {"intent-41"};
    resolution_b.physical_trade = physical_trade_for(701, 12, 102, 1);
    resolution_b.reservation = multi_reservation(501, 102, 20, 1);
    resolution_b.money_management = mm_state(1);
    resolution_b.money_management->last_settlement_id = 942;
    resolution_b.expected_series_revision = 0;
    resolution_b.expected_physical_trade_revision = 1;
    resolution_b.expected_reservation_revision = 0;
    resolution_b.expected_money_management_revision = 0;
    resolution_b.group_reservation_bindings = {
        {700, multi_reservation(500, 1000, 50, 1), 0}};
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.resolve_pending_valuation_atomic(resolution_b).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        EXPECT_EQ(storage.series(12).value->series.state_revision, 1U);
        auto unit = storage.begin();
        EXPECT_EQ(unit.record_valuation_resolution_atomic(resolution_a).kind,
                  ts::StorageResultKind::DUPLICATE);
        EXPECT_EQ(unit.resolve_pending_valuation_atomic(resolution_b).kind,
                  ts::StorageResultKind::DUPLICATE);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
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
    second.series->receipts = {first.settlement.settlement, second.settlement.settlement};
    second.series->used_intents = {41};
    second.series->used_intent_keys = {"intent-41"};
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

TEST(TradingSystemBatch5B, SameScopeReservationSwapIsRejectedByImmutableRoster) {
    TestDatabase database;
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        auto waiting = checkpoint(0);
        waiting.status = ts::SeriesStatus::WAITING_OUTCOME;
        waiting.current_intent_id = 41;
        waiting.current_group_id = 80;
        waiting.used_intents = {41};
        waiting.used_intent_keys = {"intent-41"};
        ASSERT_EQ(unit.put_series(waiting, 0).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_reservation(multi_reservation(500, 1000, 50), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_reservation(multi_reservation(501, 1000, 20), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_physical_trade(physical_trade(), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_physical_trade(physical_trade_for(701, 12, 1000), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_money_management(mm_state(), 0).kind,
                  ts::StorageResultKind::APPLIED);
        auto immutable_roster = multi_roster();
        immutable_roster.members[1].risk_scope_id = 1000;
        immutable_roster.reservation_bindings[1].risk_scope_id = 1000;
        ASSERT_EQ(unit.put_roster(immutable_roster).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    auto first = settlement_record();
    first.settlement = settlement(961, 700);
    auto second = settlement_record();
    second.settlement = settlement(962, 701);
    {
        ts::TradingSystemStorage storage(database.config());
        for (const auto& value : {std::make_pair(first, physical_trade(1)),
                                  std::make_pair(second, physical_trade_for(701, 12, 1000, 1))}) {
            ts::SettlementTransition transition;
            transition.journal.event_id = value.first.settlement.settlement_id + 1000;
            transition.journal.event_type = "SETTLEMENT_APPLIED";
            transition.journal.aggregate_id = "group:80";
            transition.journal.idempotency_key = "settlement-event-" +
                                                 std::to_string(value.first.settlement.settlement_id);
            transition.settlement = value.first;
            transition.physical_trade = value.second;
            transition.logical_currency = CurrencyType::USD;
            auto unit = storage.begin();
            ASSERT_EQ(unit.apply_settlement_atomic(transition).kind,
                      ts::StorageResultKind::APPLIED);
            ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
        }
    }

    ts::ExecutionGroupAggregator aggregate(41, {80, 2}, {700, 701}, CurrencyType::USD);
    ASSERT_EQ(aggregate.apply(first.settlement), ts::ProgressionResult::APPLIED);
    ASSERT_EQ(aggregate.apply(second.settlement), ts::ProgressionResult::APPLIED);
    ts::SettlementTransition swapped;
    swapped.journal.event_id = 1963;
    swapped.journal.event_type = "GROUP_COMPLETION";
    swapped.journal.aggregate_id = "group:80";
    swapped.journal.idempotency_key = "settlement-event-962";
    swapped.settlement = second;
    swapped.series = checkpoint(1);
    swapped.series->series.next_step = 1;
    swapped.series->current_outcome = aggregate.outcome();
    swapped.series->receipts = {first.settlement, second.settlement};
    swapped.series->used_intents = {41};
    swapped.series->used_intent_keys = {"intent-41"};
    swapped.physical_trade = physical_trade_for(701, 12, 1000, 1);
    swapped.reservation = multi_reservation(500, 1000, 50, 1);
    swapped.money_management = mm_state(1);
    swapped.money_management->last_settlement_id = 962;
    swapped.expected_series_revision = 0;
    swapped.expected_reservation_revision = 0;
    swapped.expected_money_management_revision = 0;
    swapped.logical_currency = CurrencyType::USD;
    swapped.group_reservation_bindings = {
        {700, multi_reservation(501, 1000, 20, 1), 0}};
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        const auto result = unit.apply_group_completion_atomic(swapped);
        EXPECT_EQ(result.kind, ts::StorageResultKind::CONFLICT) << result.message;
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 0U);
    EXPECT_EQ(reopened.reservation(500).value->state, ts::RiskReservationState::RESERVED);
    EXPECT_EQ(reopened.reservation(501).value->state, ts::RiskReservationState::RESERVED);
}

TEST(TradingSystemBatch5B, GroupIndexReportsCorruptionAndRebuildRemovesOrphans) {
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
        {
            mdbxc::KeyValueTable<std::string, std::string> settlements(
                storage.connection(), "ts_settlements");
            auto transaction = storage.connection()->transaction(mdbxc::TransactionMode::WRITABLE);
            ASSERT_TRUE(settlements.erase("900", transaction));
            transaction.commit();
        }
        EXPECT_EQ(storage.settlements_for_group(80).kind, ts::StorageResultKind::STORAGE_ERROR);
    }

    TestDatabase rebuild_database;
    seed(rebuild_database);
    physical = transition();
    physical.series.reset();
    physical.reservation.reset();
    physical.money_management.reset();
    {
        ts::TradingSystemStorage storage(rebuild_database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.record_physical_settlement(physical).kind,
                  ts::StorageResultKind::APPLIED);
        auto other = settlement_record();
        other.settlement = settlement(901, 701);
        other.settlement.execution_group_id = 81;
        other.native_profit = money(100);
        ASSERT_EQ(unit.put_settlement(other).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(rebuild_database.config());
        {
            mdbxc::KeyValueTable<std::string, std::string> index(
                storage.connection(), "ts_group_settlement_index");
            auto transaction = storage.connection()->transaction(mdbxc::TransactionMode::WRITABLE);
            index.insert("80:901", "901", transaction);
            index.insert("80:999", "999", transaction);
            transaction.commit();
        }
        EXPECT_EQ(storage.settlements_for_group(80).kind, ts::StorageResultKind::STORAGE_ERROR);
    }
    {
        ts::TradingSystemStorage storage(rebuild_database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.rebuild_group_settlement_index().kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(rebuild_database.config());
    EXPECT_EQ(reopened.settlements_for_group(80).kind, ts::StorageResultKind::APPLIED);
    EXPECT_EQ(reopened.settlements_for_group(80).values.size(), 1U);
}

TEST(TradingSystemBatch5B, CanonicalReplayOrdersUnknownBeforeTerminalEvidence) {
    TestDatabase database;
    seed(database);

    auto unknown = settlement_record();
    unknown.settlement = settlement(9, 700);
    unknown.settlement.kind = ts::OutcomeKind::UNKNOWN;
    unknown.settlement.evidence = ts::ExecutionEvidence::MAY_HAVE_EXECUTED;
    unknown.settlement.net_profit.reset();
    unknown.native_profit = money(0);
    unknown.logical_outcome_ready = true;

    auto terminal = settlement_record();
    terminal.settlement = settlement(10, 700);

    auto persist = [](ts::SettlementRecord record, std::uint64_t event_id,
                      const ts::PhysicalTradeRecord& physical,
                      const std::string& key) {
        ts::SettlementTransition value;
        value.journal = journal();
        value.journal.event_id = event_id;
        value.journal.idempotency_key = key;
        value.settlement = std::move(record);
        value.physical_trade = physical;
        value.expected_physical_trade_revision = physical.state_revision == 0
            ? 0 : physical.state_revision - 1;
        value.logical_currency = CurrencyType::USD;
        return value;
    };

    const auto unknown_physical = physical_trade(1);
    const auto terminal_physical = physical_trade(2);
    for (const auto& value : {persist(unknown, 100, unknown_physical, "settlement-event-9"),
                              persist(terminal, 101, terminal_physical, "settlement-event-10")}) {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.record_physical_settlement(value).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    auto completion = persist(terminal, 101, terminal_physical, "settlement-event-10");
    completion.series = checkpoint(1);
    completion.series->series.next_step = 1;
    completion.series->current_outcome = terminal_outcome();
    completion.series->receipts = {unknown.settlement, terminal.settlement};
    completion.series->used_intents = {41};
    completion.series->used_intent_keys = {"intent-41"};
    completion.reservation = reservation(1);
    completion.money_management = mm_state(1);
    completion.money_management->last_settlement_id = 10;
    completion.expected_series_revision = 0;
    completion.expected_reservation_revision = 0;
    completion.expected_money_management_revision = 0;

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        const auto result = unit.apply_group_completion_atomic(completion);
        ASSERT_EQ(result.kind, ts::StorageResultKind::APPLIED) << result.message;
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.record_physical_settlement(
                      persist(unknown, 100, terminal_physical, "settlement-event-9")).kind,
                  ts::StorageResultKind::DUPLICATE);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(database.config());
    ASSERT_TRUE(reopened.series(12).value);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
    EXPECT_EQ(reopened.series(12).value->current_outcome.kind,
              std::optional<ts::OutcomeKind>(ts::OutcomeKind::WIN));
}

TEST(TradingSystemBatch5B, ConflictingTerminalReplayIsRejected) {
    TestDatabase database;
    seed(database);
    auto win = transition();
    win.settlement.settlement.settlement_id = 10;
    win.journal.event_id = 100;
    win.journal.idempotency_key = "settlement-event-10";
    win.physical_trade = physical_trade(1);
    auto loss = win;
    loss.settlement.settlement.settlement_id = 11;
    loss.settlement.settlement.kind = ts::OutcomeKind::LOSS;
    loss.settlement.settlement.net_profit = money(-100);
    loss.settlement.native_profit = money(-100);
    loss.journal.event_id = 101;
    loss.journal.idempotency_key = "settlement-event-11";
    loss.physical_trade = physical_trade(2);
    loss.expected_physical_trade_revision = 1;
    for (const auto& value : {win, loss}) {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.record_physical_settlement(value).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::TradingSystemStorage storage(database.config());
    auto unit = storage.begin();
    const auto result = unit.apply_group_completion_atomic(win);
    EXPECT_EQ(result.kind, ts::StorageResultKind::CONFLICT) << result.message;
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
}

TEST(TradingSystemBatch5B, CanonicalReplayHandlesMixedMemberCallbackOrder) {
    TestDatabase database;
    seed_multi(database);

    auto unknown = settlement_record();
    unknown.settlement = settlement(9, 700);
    unknown.settlement.kind = ts::OutcomeKind::UNKNOWN;
    unknown.settlement.evidence = ts::ExecutionEvidence::MAY_HAVE_EXECUTED;
    unknown.settlement.net_profit.reset();
    unknown.native_profit = money(0);

    auto first_terminal = settlement_record();
    first_terminal.settlement = settlement(10, 700);
    auto second_terminal = settlement_record();
    second_terminal.settlement = settlement(11, 701);

    auto persist = [](ts::SettlementRecord record, std::uint64_t event_id,
                      const ts::PhysicalTradeRecord& physical,
                      const std::string& key) {
        ts::SettlementTransition value;
        value.journal = journal();
        value.journal.event_id = event_id;
        value.journal.idempotency_key = key;
        value.settlement = std::move(record);
        value.physical_trade = physical;
        value.expected_physical_trade_revision = physical.state_revision == 0
            ? 0 : physical.state_revision - 1;
        value.logical_currency = CurrencyType::USD;
        return value;
    };

    const auto unknown_physical = physical_trade_for(700, 11, 1000, 1);
    const auto first_terminal_physical = physical_trade_for(700, 11, 1000, 2);
    const auto second_terminal_physical = physical_trade_for(701, 12, 102, 1);
    for (const auto& value : {
             persist(second_terminal, 111, second_terminal_physical, "settlement-event-11"),
             persist(unknown, 109, unknown_physical, "settlement-event-9"),
             persist(first_terminal, 110, first_terminal_physical, "settlement-event-10")}) {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.record_physical_settlement(value).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    auto completion = persist(first_terminal, 110, first_terminal_physical,
                              "settlement-event-10");
    completion.series = checkpoint(1);
    completion.series->series.next_step = 1;
    completion.series->current_outcome = multi_terminal_outcome();
    completion.series->receipts = {
        unknown.settlement, first_terminal.settlement, second_terminal.settlement};
    completion.series->used_intents = {41};
    completion.series->used_intent_keys = {"intent-41"};
    completion.reservation = multi_reservation(500, 1000, 50, 1);
    completion.money_management = mm_state(1);
    completion.money_management->last_settlement_id = 10;
    completion.expected_series_revision = 0;
    completion.expected_reservation_revision = 0;
    completion.expected_money_management_revision = 0;
    completion.group_reservation_bindings = {
        {701, multi_reservation(501, 102, 20, 1), 0}};

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        const auto result = unit.apply_group_completion_atomic(completion);
        ASSERT_EQ(result.kind, ts::StorageResultKind::APPLIED) << result.message;
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
    EXPECT_EQ(reopened.reservation(500).value->state, ts::RiskReservationState::SETTLED);
    EXPECT_EQ(reopened.reservation(501).value->state, ts::RiskReservationState::SETTLED);
}

TEST(TradingSystemBatch5B, TerminalRiskEvidenceIgnoresLargerUnknownAndDuplicateIds) {
    TestDatabase database;
    seed(database);

    auto win = settlement_record();
    win.settlement = settlement(9, 700);
    auto unknown = settlement_record();
    unknown.settlement = settlement(10, 700);
    unknown.settlement.kind = ts::OutcomeKind::UNKNOWN;
    unknown.settlement.evidence = ts::ExecutionEvidence::MAY_HAVE_EXECUTED;
    unknown.settlement.net_profit.reset();
    unknown.native_profit = money(0);
    auto duplicate_win = win;
    duplicate_win.settlement.settlement_id = 11;

    auto persist = [](ts::SettlementRecord record, std::uint64_t event_id,
                      const ts::PhysicalTradeRecord& physical,
                      const std::string& key) {
        ts::SettlementTransition value;
        value.journal = journal();
        value.journal.event_id = event_id;
        value.journal.idempotency_key = key;
        value.settlement = std::move(record);
        value.physical_trade = physical;
        value.expected_physical_trade_revision = physical.state_revision == 0
            ? 0 : physical.state_revision - 1;
        value.logical_currency = CurrencyType::USD;
        return value;
    };
    const auto physical_one = physical_trade(1);
    const auto physical_two = physical_trade(2);
    const auto physical_three = physical_trade(3);
    for (const auto& value : {
             persist(win, 109, physical_one, "settlement-event-9"),
             persist(unknown, 110, physical_two, "settlement-event-10"),
             persist(duplicate_win, 111, physical_three, "settlement-event-11")}) {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.record_physical_settlement(value).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    auto completion = persist(duplicate_win, 111, physical_three, "settlement-event-11");
    completion.series = checkpoint(1);
    completion.series->series.next_step = 1;
    completion.series->current_outcome = terminal_outcome();
    completion.series->receipts = {win.settlement, unknown.settlement,
                                    duplicate_win.settlement};
    completion.series->used_intents = {41};
    completion.series->used_intent_keys = {"intent-41"};
    completion.reservation = reservation(1);
    completion.money_management = mm_state(1);
    completion.money_management->last_settlement_id = 11;
    completion.expected_series_revision = 0;
    completion.expected_reservation_revision = 0;
    completion.expected_money_management_revision = 0;

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_group_completion_atomic(completion).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.reservation(500).value->state, ts::RiskReservationState::SETTLED);
    EXPECT_EQ(reopened.series(12).value->series.state_revision, 1U);
}

TEST(TradingSystemBatch5B, SeriesHydrationCanonicalizesDuplicateTerminalEvidenceAfterReopen) {
    TestDatabase database;
    seed_multi(database);

    auto win = settlement_record();
    win.settlement = settlement(9, 700);
    auto unknown = settlement_record();
    unknown.settlement = settlement(10, 701);
    unknown.settlement.kind = ts::OutcomeKind::UNKNOWN;
    unknown.settlement.evidence = ts::ExecutionEvidence::MAY_HAVE_EXECUTED;
    unknown.settlement.net_profit.reset();
    unknown.native_profit = money(0);
    auto duplicate_win = win;
    duplicate_win.settlement.settlement_id = 11;
    auto loss = settlement_record();
    loss.settlement = settlement(12, 701);
    loss.settlement.kind = ts::OutcomeKind::LOSS;
    loss.settlement.net_profit = money(-20);
    loss.native_profit = money(-20);

    auto persist = [](ts::SettlementRecord record, std::uint64_t event_id,
                      const ts::PhysicalTradeRecord& physical,
                      const std::string& key) {
        ts::SettlementTransition value;
        value.journal = journal();
        value.journal.event_id = event_id;
        value.journal.idempotency_key = key;
        value.journal.payload = ts::detail::settlement_to_json(record.settlement);
        value.settlement = std::move(record);
        value.physical_trade = physical;
        value.expected_physical_trade_revision = physical.state_revision == 0
            ? 0 : physical.state_revision - 1;
        value.logical_currency = CurrencyType::USD;
        return value;
    };

    const auto physical_700 = physical_trade_for(700, 11, 1000, 1);
    const auto physical_701 = physical_trade_for(701, 12, 102, 1);
    for (const auto& value : {
             persist(win, 109, physical_700, "settlement-event-9"),
             persist(unknown, 110, physical_701, "settlement-event-10"),
             persist(duplicate_win, 111, physical_700, "settlement-event-11"),
             persist(loss, 112, physical_701, "settlement-event-12")}) {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        const auto recorded = unit.record_physical_settlement(value);
        ASSERT_TRUE(recorded.kind == ts::StorageResultKind::APPLIED ||
                    recorded.kind == ts::StorageResultKind::DUPLICATE)
            << recorded.message;
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    const auto checkpoint_from = [](const ts::SeriesSnapshot& snapshot,
                                    std::vector<ts::TradeSettlement> receipts) {
        ts::SeriesCheckpoint result;
        result.series = snapshot.series;
        result.status = snapshot.status;
        result.current_intent_id = snapshot.current_intent_id;
        result.current_group_id = snapshot.current_group_id;
        result.current_step = snapshot.current_step;
        result.current_outcome = snapshot.outcome;
        result.receipts = std::move(receipts);
        result.used_intents = {41};
        result.used_intent_keys = {"intent-41"};
        result.schema_version = ts::trading_storage_schema_version;
        return result;
    };

    ts::SeriesSnapshot initial;
    initial.series = {12, 7, 0, 0};
    initial.status = ts::SeriesStatus::WAITING_OUTCOME;
    initial.current_intent_id = 41;
    initial.current_group_id = 80;
    initial.current_step = 0;
    ts::SeriesProgression owner(initial.series);
    ASSERT_EQ(owner.hydrate(
                  initial, ts::ExecutionGroupAggregator(
                              41, {80, 2}, {700, 701}, CurrencyType::USD),
                  {}, {41}, {"intent-41"}),
              ts::ProgressionResult::APPLIED);
    ASSERT_EQ(owner.apply_group_completion(
                  {unknown.settlement, win.settlement, duplicate_win.settlement}, 0),
              ts::ProgressionResult::APPLIED);
    ASSERT_EQ(owner.snapshot().status, ts::SeriesStatus::UNRESOLVED);
    ASSERT_EQ(owner.snapshot().series.state_revision, 1U);
    ASSERT_EQ(owner.snapshot().outcome.completed_members, 1U);

    const auto partial = checkpoint_from(
        owner.snapshot(), {duplicate_win.settlement, unknown.settlement, win.settlement});
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.put_series(partial, 0).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::SeriesCheckpoint reopened_checkpoint;
    {
        ts::TradingSystemStorage storage(database.config());
        ASSERT_TRUE(storage.series(12).value);
        reopened_checkpoint = *storage.series(12).value;
    }

    ts::SeriesSnapshot reopened_snapshot;
    reopened_snapshot.series = reopened_checkpoint.series;
    reopened_snapshot.status = reopened_checkpoint.status;
    reopened_snapshot.current_intent_id = reopened_checkpoint.current_intent_id;
    reopened_snapshot.current_group_id = reopened_checkpoint.current_group_id;
    reopened_snapshot.current_step = reopened_checkpoint.current_step;
    reopened_snapshot.outcome = reopened_checkpoint.current_outcome;
    ts::SeriesProgression reopened_owner({12, 7, 0, 0});
    ASSERT_EQ(reopened_owner.hydrate(
                  reopened_snapshot, ts::ExecutionGroupAggregator(
                                        41, {80, 2}, {700, 701}, CurrencyType::USD),
                  reopened_checkpoint.receipts, {41}, {"intent-41"}),
              ts::ProgressionResult::APPLIED);
    EXPECT_EQ(reopened_owner.snapshot().status, ts::SeriesStatus::UNRESOLVED);
    EXPECT_EQ(reopened_owner.snapshot().outcome.completed_members, 1U);
    EXPECT_EQ(reopened_owner.apply(win.settlement, 1), ts::ProgressionResult::DUPLICATE);
    EXPECT_EQ(reopened_owner.apply(duplicate_win.settlement, 1),
              ts::ProgressionResult::DUPLICATE);

    auto conflicting = win.settlement;
    conflicting.settlement_id = 13;
    conflicting.kind = ts::OutcomeKind::LOSS;
    conflicting.net_profit = money(-100);
    EXPECT_EQ(reopened_owner.apply(conflicting, 1),
              ts::ProgressionResult::MEMBER_ALREADY_TERMINAL);
    ASSERT_EQ(reopened_owner.apply(loss.settlement, 1),
              ts::ProgressionResult::APPLIED);
    ASSERT_EQ(reopened_owner.snapshot().status, ts::SeriesStatus::ACTIVE);
    ASSERT_EQ(reopened_owner.snapshot().series.state_revision, 2U);
    ASSERT_EQ(reopened_owner.snapshot().series.next_step, 1U);

    auto completion = persist(
        loss, 112, physical_701, "settlement-event-12");
    completion.series = checkpoint_from(
        reopened_owner.snapshot(),
        {duplicate_win.settlement, unknown.settlement, win.settlement, loss.settlement});
    completion.reservation = multi_reservation(501, 102, 20, 1);
    completion.group_reservation_bindings = {
        {700, multi_reservation(500, 1000, 50, 1), 0}};
    completion.money_management = mm_state(1);
    completion.money_management->last_settlement_id = 12;
    completion.expected_series_revision = 1;
    completion.expected_reservation_revision = 0;
    completion.expected_money_management_revision = 0;

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_group_completion_atomic(completion).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        EXPECT_EQ(unit.apply_group_completion_atomic(completion).kind,
                  ts::StorageResultKind::DUPLICATE);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::TradingSystemStorage reopened(database.config());
    ASSERT_EQ(reopened.series(12).value->series.state_revision, 2U);
    EXPECT_EQ(reopened.reservation(500).value->state, ts::RiskReservationState::SETTLED);
    EXPECT_EQ(reopened.reservation(501).value->state, ts::RiskReservationState::SETTLED);
    EXPECT_EQ(reopened.money_management("mm.alpha").value->state_revision, 1U);
    EXPECT_EQ(reopened.money_management("mm.alpha").value->last_settlement_id, 12U);
}

TEST(TradingSystemBatch5B, ReservationCannotBeReusedByAnotherImmutableRoster) {
    TestDatabase database;
    seed(database);
    ts::ExecutionRosterRecord group_b;
    group_b.group_id = 81;
    group_b.intent_id = 42;
    group_b.expected_members = 1;
    group_b.members = {{801, 14, 1000, money(50)}};
    group_b.reservation_bindings = {
        {801, 500, 1000, money(50), money(50), "risk-41"}};

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.put_roster(group_b).kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::CONFLICT);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.roster(81).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(reopened.roster(80).kind, ts::StorageResultKind::APPLIED);
    EXPECT_EQ(reopened.audit_roster_reservation_bindings().kind,
              ts::StorageResultKind::APPLIED);
}

TEST(TradingSystemBatch5B, JournalOnlyCommitDoesNotScanHistoricalRosters) {
    TestDatabase database;
    for (std::uint64_t index = 0; index < 8; ++index) {
        ts::ReservationRecord reservation_value;
        reservation_value.reservation_id = 600 + index;
        reservation_value.risk_scope_id = 200 + index;
        reservation_value.intent_id = 500 + index;
        reservation_value.execution_group = {100 + index, 1};
        reservation_value.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
        reservation_value.stake = money(10);
        reservation_value.potential_risk = money(10);
        reservation_value.idempotency_key = "risk-history-" + std::to_string(index);

        ts::ExecutionRosterRecord roster_value;
        roster_value.group_id = 100 + index;
        roster_value.intent_id = 500 + index;
        roster_value.expected_members = 1;
        roster_value.members = {{800 + index, 900 + index, 200 + index, money(10)}};
        roster_value.reservation_bindings = {{
            800 + index, 600 + index, 200 + index, money(10), money(10),
            "risk-history-" + std::to_string(index)}};

        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.put_reservation(reservation_value, 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_roster(roster_value).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::TradingSystemStorage storage(database.config());
    auto unit = storage.begin();
    ts::JournalEvent event;
    event.event_id = 1;
    event.event_type = "MAINTENANCE_MARKER";
    event.aggregate_id = "maintenance";
    event.idempotency_key = "maintenance-1";
    ASSERT_EQ(unit.append_journal(event).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    EXPECT_EQ(unit.roster_validation_checks(), 0U);
    EXPECT_EQ(storage.audit_roster_reservation_bindings().kind,
              ts::StorageResultKind::APPLIED);
}

TEST(TradingSystemBatch5B, JournalEventIdsShareOneDurableNamespace) {
    TestDatabase database;
    seed(database);
    auto value = transition();
    value.journal.event_id = 1;
    value.journal.idempotency_key = "settlement-event-one";
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(value).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        ts::TradingSystemStorage storage(database.config());
        const auto logical = storage.journal_by_key("settlement-event-one:logical");
        ASSERT_TRUE(logical.value);
        EXPECT_EQ(logical.value->event_id, 2U);
        auto unit = storage.begin();
        ts::JournalEvent next;
        next.event_id = 3;
        next.event_type = "RECOVERY_MARKER";
        next.aggregate_id = "group:80";
        next.idempotency_key = "recovery-marker-3";
        ASSERT_EQ(unit.append_journal(next).kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(unit.next_id("journal_event").value, 4U);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    ts::TradingSystemStorage reopened(database.config());
    EXPECT_EQ(reopened.journal_by_key("recovery-marker-3").value->event_id, 3U);
    auto unit = reopened.begin();
    auto collision = ts::JournalEvent{};
    collision.event_id = 2;
    collision.event_type = "OTHER";
    collision.aggregate_id = "group:80";
    collision.idempotency_key = "collision";
    EXPECT_EQ(unit.append_journal(collision).kind, ts::StorageResultKind::CONFLICT);
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
}

TEST(TradingSystemBatch5B, ReservationIdentityCannotChangeAcrossLifecycleRevision) {
    TestDatabase database;
    seed(database);

    const auto expect_conflict = [&database](const auto& mutate) {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        auto changed = reservation(1);
        mutate(changed);
        EXPECT_EQ(unit.put_reservation(changed, 0).kind,
                  ts::StorageResultKind::CONFLICT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    };
    expect_conflict([](auto& value) { value.stake = money(49); });
    expect_conflict([](auto& value) { value.risk_scope_id = 1001; });
    expect_conflict([](auto& value) { value.intent_id = 42; });
    expect_conflict([](auto& value) { value.execution_group.group_id = 81; });
    expect_conflict([](auto& value) { value.idempotency_key = "risk-other"; });

    ts::TradingSystemStorage reopened(database.config());
    ASSERT_TRUE(reopened.reservation(500).value);
    EXPECT_EQ(reopened.reservation(500).value->stake.units, 50);
    EXPECT_EQ(reopened.reservation(500).value->risk_scope_id, 1000U);
    EXPECT_EQ(reopened.reservation(500).value->intent_id, 41U);
    EXPECT_EQ(reopened.reservation(500).value->execution_group.group_id, 80U);
    EXPECT_EQ(reopened.reservation(500).value->idempotency_key, "risk-41");
}

TEST(TradingSystemBatch5B, LogicalCompletionPayloadConflictIsNotDuplicate) {
    TestDatabase database;
    seed(database);
    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(transition()).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }

    ts::TradingSystemStorage storage(database.config());
    const auto logical = storage.journal_by_key("settlement-event-900:logical");
    ASSERT_TRUE(logical.value);
    auto replay = transition();
    replay.completion_journal = *logical.value;
    replay.completion_journal->payload["completed_members"] = 99;
    auto unit = storage.begin();
    const auto result = unit.apply_group_completion_atomic(replay);
    EXPECT_EQ(result.kind, ts::StorageResultKind::CONFLICT) << result.message;
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
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
    second.series->receipts = {first.settlement.settlement, second.settlement.settlement};
    second.series->used_intents = {41};
    second.series->used_intent_keys = {"intent-41"};
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
    correct.series->receipts = {settlement(900, 700)};
    correct.series->receipts.front().net_profit = money(200);
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
        ASSERT_EQ(unit.put_reservation(multi_reservation(500, 1000, 50), 0).kind,
                  ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.put_reservation(multi_reservation(501, 102, 20), 0).kind,
                  ts::StorageResultKind::APPLIED);
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
    resolution.series->receipts = {settlement()};
    resolution.series->used_intents = {41};
    resolution.series->used_intent_keys = {"intent-41"};
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

TEST(TradingSystemBatch5B, SymmetricScopeGatesAreIdempotentInBothOrdersAndAfterRebuild) {
    for (const auto first_scope : {1000U, 2000U}) {
        SCOPED_TRACE(first_scope);
        TestDatabase database;
        const auto second_scope = first_scope == 1000 ? 2000U : 1000U;
        ts::AccountIdentityRecord first{11, "IntradeBar", "same-account", "DEMO",
            11, first_scope, 0, true, ts::trading_storage_schema_version};
        auto second = first;
        second.account_id = second.canonical_account_id = 12;
        second.risk_scope_id = second_scope;
        {
            ts::TradingSystemStorage storage(database.config());
            auto unit = storage.begin();
            ASSERT_EQ(unit.put_account(first).kind, ts::StorageResultKind::APPLIED);
            EXPECT_EQ(unit.put_account(second).kind, ts::StorageResultKind::CONFLICT);
            EXPECT_EQ(unit.put_account(second).kind, ts::StorageResultKind::CONFLICT);
            ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
        }
        ts::TradingSystemStorage reopened(database.config());
        ASSERT_TRUE(reopened.admission_scope_index_verified());
        ASSERT_TRUE(reopened.risk_scope_gate(first_scope).found());
        EXPECT_EQ(reopened.risk_scope_gate(first_scope).value->related_scope_id, second_scope);
        EXPECT_EQ(reopened.risk_scope_gate(second_scope).value->related_scope_id, first_scope);
        EXPECT_EQ(reopened.account(12).kind, ts::StorageResultKind::NOT_FOUND);
        {
            auto unit = reopened.begin();
            ASSERT_EQ(unit.rebuild_admission_scope_index().kind, ts::StorageResultKind::APPLIED);
            ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
        }
        EXPECT_EQ(reopened.audit_admission_scope_index().kind, ts::StorageResultKind::APPLIED);
        for (const auto scope : {first_scope, second_scope}) {
            auto unit = reopened.begin();
            EXPECT_EQ(unit.validate_admission_scopes({scope}).kind, ts::StorageResultKind::INVALID_STATE);
        }
        auto another = first;
        another.account_id = another.canonical_account_id = 13;
        another.risk_scope_id = 3000;
        {
            auto unit = reopened.begin();
            EXPECT_EQ(unit.put_account(another).kind, ts::StorageResultKind::CONFLICT);
            EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
        }
        EXPECT_EQ(reopened.risk_scope_gate(3000).kind, ts::StorageResultKind::NOT_FOUND);
    }
}

TEST(TradingSystemBatch5B, ProjectionFailureRollsBackCanonicalGates) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    ts::AccountIdentityRecord first{11, "IntradeBar", "same-account", "DEMO",
        11, 1000, 0, true, ts::trading_storage_schema_version};
    {
        auto unit = storage.begin();
        ASSERT_EQ(unit.put_account(first).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    mdbxc::KeyValueTable<std::string, std::string> index(storage.connection(), "ts_admission_scope_index");
    index.insert(ts::detail::admission_scope_index_key(
        2000, "gate", "account-scope:1000:2000:same-account"), "{}");
    auto second = first;
    second.account_id = second.canonical_account_id = 12;
    second.risk_scope_id = 2000;
    {
        auto unit = storage.begin();
        EXPECT_EQ(unit.put_account(second).kind, ts::StorageResultKind::CONFLICT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    EXPECT_EQ(storage.risk_scope_gate(1000).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(storage.risk_scope_gate(2000).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(storage.account(12).kind, ts::StorageResultKind::NOT_FOUND);
}

TEST(TradingSystemBatch5B, AdmissionAndCompletionVisitOnlyTheTargetScopeOnLargeHistory) {
    TestDatabase database;
    seed(database);
    ts::TradingSystemStorage storage(database.config());
    {
        auto unit = storage.begin();
        for (std::uint64_t id = 10000; id != 11000; ++id) {
            auto history = reservation();
            history.reservation_id = history.intent_id = id;
            history.risk_scope_id = id;
            history.execution_group = {id, 1};
            history.state = ts::RiskReservationState::SETTLED;
            history.idempotency_key = "historical-risk-" + std::to_string(id);
            ASSERT_EQ(unit.put_reservation(history, 0).kind, ts::StorageResultKind::APPLIED);
            ts::ExecutionOperationRecord operation{
                "history-" + std::to_string(id), "client-" + std::to_string(id),
                id, id, id, id, 2000, ts::ExecutionOperationState::PREPARED, 0, {}, 1000};
            ts::JournalEvent event{id, "EXECUTION_OPERATION", "operation:" + operation.operation_id,
                0, ts::trading_storage_schema_version, operation.operation_id, 1000, operation};
            ASSERT_EQ(unit.append_journal(event).kind, ts::StorageResultKind::APPLIED);
        }
        ts::ExecutionOperationRecord operation{
            "target-operation", "target-client", 41, 80, 700, 11, 1000,
            ts::ExecutionOperationState::PREPARED, 0, {}, 1000};
        ts::JournalEvent event{11000, "EXECUTION_OPERATION", "operation:target-operation", 0,
            ts::trading_storage_schema_version, "target-operation", 1000, operation};
        ASSERT_EQ(unit.append_journal(event).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        auto unit = storage.begin();
        EXPECT_EQ(unit.validate_admission_scopes({1000}, 80).kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(unit.admission_scope_index_visits(), 2U);
        RecordProperty("admission_index_visits", static_cast<int>(unit.admission_scope_index_visits()));
    }
    {
        auto unit = storage.begin();
        const auto result = unit.apply_settlement_atomic(transition());
        ASSERT_EQ(result.kind, ts::StorageResultKind::APPLIED) << result.message;
        EXPECT_EQ(unit.admission_scope_index_visits(), 1U);
        RecordProperty("completion_index_visits", static_cast<int>(unit.admission_scope_index_visits()));
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        auto unit = storage.begin();
        EXPECT_EQ(unit.validate_admission_scopes({1000}).kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(unit.admission_scope_index_visits(), 0U);
    }
    EXPECT_EQ(storage.audit_admission_scope_index().kind, ts::StorageResultKind::APPLIED);
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
