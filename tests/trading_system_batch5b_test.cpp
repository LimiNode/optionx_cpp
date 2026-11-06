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

ts::ReservationRecord reservation(std::uint64_t revision = 0) {
    ts::ReservationRecord result;
    result.reservation_id = 500;
    result.risk_scope_id = 1000;
    result.execution_group = {80, 2};
    result.cardinality = ts::ExecutionCardinality::MULTIPLE;
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
    result.expected_members = 2;
    result.members = {{700, 11, 1000, money(10)}, {701, 12, 102, money(20)}};
    return result;
}

void seed(TestDatabase& database) {
    ts::TradingSystemStorage storage(database.config());
    auto unit = storage.begin();
    ASSERT_EQ(unit.put_series(checkpoint(), 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_reservation(reservation(), 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_physical_trade(physical_trade(), 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_money_management(mm_state(), 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_roster(roster()).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
}

TEST(TradingSystemBatch5B, AtomicSettlementWritesJournalAndAllProjections) {
    TestDatabase database;
    seed(database);

    {
        ts::TradingSystemStorage storage(database.config());
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(transition()).kind,
                  ts::StorageResultKind::APPLIED);
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
        ASSERT_EQ(unit.put_roster(roster()).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    auto changed = roster();
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
    ts::TradingSystemStorage storage(database.config());
    auto value = settlement_record(CurrencyType::EUR);
    auto pending = transition();
    pending.settlement = value;
    pending.journal.payload = ts::detail::settlement_to_json(value.settlement);
    pending.series.reset();
    pending.physical_trade.reset();
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
    ts::SettlementTransition pending = transition();
    pending.settlement = settlement_record(CurrencyType::EUR);
    pending.settlement.valuation = ts::ValuationFact{
        CurrencyType::EUR, CurrencyType::USD, 11, 10, 123, "test", "fx-1",
        "EXACT_FIXED_POINT", false, 2};
    pending.journal.payload = ts::detail::settlement_to_json(pending.settlement.settlement);
    pending.series.reset();
    pending.physical_trade.reset();
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
        pending.physical_trade.reset();
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
    resolution.physical_trade = physical_trade(1);
    resolution.reservation = reservation(1);
    resolution.money_management = mm_state(1);

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
    auto value = roster();
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
    {
        ts::TradingSystemStorage reopened(database.config());
        ASSERT_TRUE(reopened.risk_scope_gate(1000).value);
        ASSERT_TRUE(reopened.risk_scope_gate(2000).value);
        EXPECT_EQ(reopened.risk_scope_gate(1000).value->state,
                  ts::RiskScopeGateState::BLOCKED);
        EXPECT_EQ(reopened.risk_scope_gate(2000).value->state,
                  ts::RiskScopeGateState::BLOCKED);
    }

    ts::RiskAdmissionConfig blocked_config;
    blocked_config.scope = {ts::RiskScopeKind::ACCOUNT, 1000};
    blocked_config.admission_blocked = true;
    ts::RiskAdmission blocked_owner(blocked_config);
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
    intent.execution_group = {80, 2};
    intent.cardinality = ts::ExecutionCardinality::MULTIPLE;
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
