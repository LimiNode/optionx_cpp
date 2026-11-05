#include <optionx_cpp/trading_system.hpp>

#include <atomic>
#include <limits>
#include <thread>

#include <gtest/gtest.h>

namespace {

namespace ts = optionx::trading_system;
using optionx::CurrencyType;

ts::MoneyAmount money(std::int64_t units, CurrencyType currency = CurrencyType::USD) {
    return {units, 2, currency};
}

ts::RiskAdmissionConfig config(std::int64_t max_risk = 0) {
    ts::RiskAdmissionConfig result;
    result.scope = {ts::RiskScopeKind::ACCOUNT, 7};
    if (max_risk != 0) result.max_reserved_risk = money(max_risk);
    return result;
}

ts::RiskAccountSnapshot snapshot(
    std::int64_t balance = 1000,
    std::uint64_t revision = 1,
    std::int64_t captured_at_ms = 1000) {
    return {ts::RiskAvailability::SUPPORTED, money(balance), captured_at_ms, revision};
}

ts::TradeIntent intent(
    ts::IntentId id,
    const std::string& key,
    ts::ExecutionCardinality cardinality = ts::ExecutionCardinality::ZERO_OR_ONE,
    ts::ExecutionGroupIdentity group = {}) {
    ts::TradeIntent result;
    result.intent_id = id;
    result.idempotency_key = key;
    result.cardinality = cardinality;
    result.execution_group = group;
    result.status = ts::IntentStatus::ADMITTED;
    return result;
}

ts::RiskRequest request(
    ts::IntentId id,
    const std::string& key,
    std::int64_t stake = 10,
    std::int64_t risk = 10) {
    return ts::RiskRequest::from_intent(intent(id, key), money(stake), money(risk));
}

ts::RiskSettlementEvidence terminal_evidence(
    const ts::RiskReservation& reservation,
    std::uint32_t members = 1) {
    ts::RiskSettlementEvidence evidence;
    evidence.reservation_id = reservation.reservation_id;
    evidence.intent_id = reservation.intent_id;
    evidence.execution_group = reservation.execution_group;
    evidence.outcome.final = true;
    evidence.outcome.all_members_terminal = true;
    evidence.outcome.completed_members = members;
    evidence.outcome.aggregation_status = ts::AggregationStatus::READY;
    evidence.outcome.kind = ts::OutcomeKind::DRAW;
    evidence.outcome.net_profit = money(0);
    for (std::uint32_t i = 0; i < members; ++i) {
        ts::TradeSettlement settlement;
        settlement.settlement_id = 100 + i;
        settlement.intent_id = reservation.intent_id;
        settlement.execution_group_id = reservation.execution_group.group_id;
        settlement.trade_id = 1 + i;
        settlement.kind = ts::OutcomeKind::DRAW;
        settlement.evidence = ts::ExecutionEvidence::EXECUTED;
        settlement.net_profit = money(0);
        evidence.settlements.push_back(std::move(settlement));
    }
    return evidence;
}

TEST(TradingSystemBatch4, RequiresSupportedFreshSnapshot) {
    ts::RiskAdmission owner(config());
    EXPECT_TRUE(owner.valid());
    EXPECT_EQ(owner.reserve(request(1, "one"), 1000).kind,
              ts::RiskAdmissionKind::UNAVAILABLE);

    auto unavailable = snapshot();
    unavailable.availability = ts::RiskAvailability::UNAVAILABLE;
    EXPECT_EQ(owner.update_snapshot(unavailable).kind, ts::RiskAdmissionKind::UNAVAILABLE);
    ASSERT_EQ(owner.update_snapshot(snapshot()).kind, ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserve(request(1, "one"), 31'001).reason,
              ts::RiskAdmissionReason::SNAPSHOT_STALE);
    EXPECT_EQ(owner.reserved_risk_units(), 0);
}

TEST(TradingSystemBatch4, ReservesFullGroupRiskAndHonorsLimits) {
    ts::RiskAdmission owner(config(50));
    ASSERT_EQ(owner.update_snapshot(snapshot(100)).kind, ts::RiskAdmissionKind::APPLIED);
    auto split = request(1, "split", 10, 30);
    split.cardinality = ts::ExecutionCardinality::SPLIT_GROUP;
    split.execution_group = {80, 3};
    ASSERT_EQ(owner.reserve(split, 1000).kind, ts::RiskAdmissionKind::ADMITTED);
    EXPECT_EQ(owner.reserved_risk_units(), 30);
    EXPECT_EQ(owner.reserve(request(2, "too-much", 10, 25), 1000).reason,
              ts::RiskAdmissionReason::RISK_LIMIT_EXCEEDED);
    EXPECT_EQ(owner.reserved_risk_units(), 30);
}

TEST(TradingSystemBatch4, ReserveIsIdempotentAndPayloadBound) {
    ts::RiskAdmission owner(config());
    ASSERT_EQ(owner.update_snapshot(snapshot()).kind, ts::RiskAdmissionKind::APPLIED);
    const auto first = owner.reserve(request(1, "one"), 1000);
    ASSERT_EQ(first.kind, ts::RiskAdmissionKind::ADMITTED);
    ASSERT_TRUE(first.reservation);
    const auto duplicate = owner.reserve(request(1, "one"), 1000);
    EXPECT_EQ(duplicate.kind, ts::RiskAdmissionKind::DUPLICATE);
    EXPECT_EQ(duplicate.reservation->reservation_id, first.reservation->reservation_id);
    EXPECT_TRUE(duplicate.is_submission_admission());

    EXPECT_EQ(owner.reserve(request(1, "one", 11), 1000).kind,
              ts::RiskAdmissionKind::CONFLICT);
    EXPECT_EQ(owner.reserve(request(1, "another"), 1000).kind,
              ts::RiskAdmissionKind::CONFLICT);
    auto precision = request(2, "precision");
    precision.potential_risk.precision = 3;
    EXPECT_EQ(owner.reserve(precision, 1000).reason,
              ts::RiskAdmissionReason::PRECISION_MISMATCH);
}

TEST(TradingSystemBatch4, LifecycleRetainsUnknownRiskUntilSettlement) {
    ts::RiskAdmission owner(config());
    ASSERT_EQ(owner.update_snapshot(snapshot()).kind, ts::RiskAdmissionKind::APPLIED);
    const auto reserved = owner.reserve(request(1, "one", 10, 40), 1000);
    ASSERT_TRUE(reserved.reservation);
    const auto token = *reserved.reservation;
    EXPECT_EQ(owner.commit(token).kind, ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.release(token).reason, ts::RiskAdmissionReason::INVALID_TRANSITION);
    EXPECT_EQ(owner.mark_unknown(token).kind, ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserved_risk_units(), 40);
    EXPECT_EQ(owner.release(token).reason, ts::RiskAdmissionReason::INVALID_TRANSITION);
    const auto evidence = terminal_evidence(token);
    EXPECT_EQ(owner.settle(token, evidence).kind, ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserved_risk_units(), 0);
    const auto historical = owner.reserve(request(1, "one", 10, 40), 1000);
    EXPECT_EQ(historical.kind, ts::RiskAdmissionKind::DUPLICATE);
    EXPECT_FALSE(historical.succeeded());
    EXPECT_FALSE(historical.is_submission_admission());
    EXPECT_FALSE(historical.retains_risk());
    EXPECT_EQ(owner.settle(token, evidence).kind, ts::RiskAdmissionKind::DUPLICATE);
}

TEST(TradingSystemBatch4, RejectedBeforeSubmitReleasesExactlyOnce) {
    ts::RiskAdmission owner(config());
    ASSERT_EQ(owner.update_snapshot(snapshot()).kind, ts::RiskAdmissionKind::APPLIED);
    const auto reserved = owner.reserve(request(1, "one", 10, 40), 1000);
    ASSERT_TRUE(reserved.reservation);
    EXPECT_EQ(owner.release(*reserved.reservation).kind, ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserved_risk_units(), 0);
    EXPECT_EQ(owner.release(*reserved.reservation).kind, ts::RiskAdmissionKind::DUPLICATE);
    EXPECT_EQ(owner.mark_unknown(*reserved.reservation).reason,
              ts::RiskAdmissionReason::INVALID_TRANSITION);
    const auto historical = owner.reserve(request(1, "one", 10, 40), 1000);
    EXPECT_EQ(historical.kind, ts::RiskAdmissionKind::DUPLICATE);
    EXPECT_FALSE(historical.succeeded());
}

TEST(TradingSystemBatch4, PartialOrBlockedEvidenceCannotReleaseUnknownGroup) {
    ts::RiskAdmission owner(config());
    ASSERT_EQ(owner.update_snapshot(snapshot()).kind, ts::RiskAdmissionKind::APPLIED);
    auto split = request(1, "split", 10, 40);
    split.cardinality = ts::ExecutionCardinality::SPLIT_GROUP;
    split.execution_group = {80, 2};
    const auto reserved = owner.reserve(split, 1000);
    ASSERT_EQ(reserved.kind, ts::RiskAdmissionKind::ADMITTED);
    const auto token = *reserved.reservation;
    ASSERT_EQ(owner.commit(token).kind, ts::RiskAdmissionKind::APPLIED);
    ASSERT_EQ(owner.mark_unknown(token).kind, ts::RiskAdmissionKind::APPLIED);

    auto partial = terminal_evidence(token, 1);
    EXPECT_EQ(owner.settle(token, partial).reason,
              ts::RiskAdmissionReason::INVALID_SETTLEMENT_EVIDENCE);
    EXPECT_EQ(owner.reserved_risk_units(), 40);

    auto blocked = terminal_evidence(token, 2);
    blocked.outcome.aggregation_status = ts::AggregationStatus::NUMERIC_OVERFLOW;
    EXPECT_EQ(owner.settle(token, blocked).reason,
              ts::RiskAdmissionReason::INVALID_SETTLEMENT_EVIDENCE);
    EXPECT_EQ(owner.reserved_risk_units(), 40);

    auto stale = terminal_evidence(token, 2);
    ++stale.reservation_id;
    EXPECT_EQ(owner.settle(token, stale).reason,
              ts::RiskAdmissionReason::INVALID_SETTLEMENT_EVIDENCE);

    const auto complete = terminal_evidence(token, 2);
    EXPECT_EQ(owner.settle(token, complete).kind, ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserved_risk_units(), 0);
}

TEST(TradingSystemBatch4, SettlementEvidenceMustMatchAggregatorOutcome) {
    ts::RiskAdmission owner(config());
    ASSERT_EQ(owner.update_snapshot(snapshot()).kind, ts::RiskAdmissionKind::APPLIED);
    auto split = request(1, "consistent", 10, 40);
    split.cardinality = ts::ExecutionCardinality::SPLIT_GROUP;
    split.execution_group = {81, 2};
    const auto reserved = owner.reserve(split, 1000);
    ASSERT_EQ(reserved.kind, ts::RiskAdmissionKind::ADMITTED);
    const auto token = *reserved.reservation;
    ASSERT_EQ(owner.commit(token).kind, ts::RiskAdmissionKind::APPLIED);
    ASSERT_EQ(owner.mark_unknown(token).kind, ts::RiskAdmissionKind::APPLIED);

    auto evidence = terminal_evidence(token, 2);
    evidence.settlements[0].kind = ts::OutcomeKind::WIN;
    evidence.settlements[0].net_profit = money(100);
    evidence.settlements[1].kind = ts::OutcomeKind::WIN;
    evidence.settlements[1].net_profit = money(200);
    evidence.outcome.kind = ts::OutcomeKind::DRAW;
    evidence.outcome.net_profit = money(0);
    EXPECT_EQ(owner.settle(token, evidence).reason,
              ts::RiskAdmissionReason::INVALID_SETTLEMENT_EVIDENCE);
    EXPECT_EQ(owner.reserved_risk_units(), 40);

    evidence.outcome.kind = ts::OutcomeKind::LOSS;
    evidence.outcome.net_profit = money(300);
    EXPECT_EQ(owner.settle(token, evidence).reason,
              ts::RiskAdmissionReason::INVALID_SETTLEMENT_EVIDENCE);

    evidence.outcome.kind = ts::OutcomeKind::WIN;
    EXPECT_EQ(owner.settle(token, evidence).kind, ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserved_risk_units(), 0);

    ts::RiskAdmission no_execution_owner(config());
    ASSERT_EQ(no_execution_owner.update_snapshot(snapshot()).kind, ts::RiskAdmissionKind::APPLIED);
    const auto no_execution = no_execution_owner.reserve(request(2, "cancelled"), 1000);
    ASSERT_EQ(no_execution.kind, ts::RiskAdmissionKind::ADMITTED);
    const auto no_execution_token = *no_execution.reservation;
    ASSERT_EQ(no_execution_owner.commit(no_execution_token).kind, ts::RiskAdmissionKind::APPLIED);
    auto cancelled = terminal_evidence(no_execution_token);
    cancelled.settlements[0].kind = ts::OutcomeKind::CANCELLED;
    cancelled.settlements[0].evidence = ts::ExecutionEvidence::NOT_SUBMITTED;
    cancelled.settlements[0].net_profit.reset();
    cancelled.outcome.kind = ts::OutcomeKind::CANCELLED;
    cancelled.outcome.net_profit = money(0);
    EXPECT_EQ(no_execution_owner.settle(no_execution_token, cancelled).reason,
              ts::RiskAdmissionReason::INVALID_SETTLEMENT_EVIDENCE);
    cancelled.outcome.net_profit.reset();
    EXPECT_EQ(no_execution_owner.settle(no_execution_token, cancelled).kind,
              ts::RiskAdmissionKind::APPLIED);
}

TEST(TradingSystemBatch4, EvidenceRejectsMoneyMismatchAndAggregateOverflow) {
    ts::RiskAdmission owner(config());
    ASSERT_EQ(owner.update_snapshot(snapshot()).kind, ts::RiskAdmissionKind::APPLIED);
    const auto reserved = owner.reserve(request(1, "money"), 1000);
    ASSERT_EQ(reserved.kind, ts::RiskAdmissionKind::ADMITTED);
    const auto token = *reserved.reservation;
    ASSERT_EQ(owner.commit(token).kind, ts::RiskAdmissionKind::APPLIED);
    auto evidence = terminal_evidence(token);
    evidence.settlements[0].net_profit->currency = CurrencyType::EUR;
    EXPECT_EQ(owner.settle(token, evidence).reason,
              ts::RiskAdmissionReason::INVALID_SETTLEMENT_EVIDENCE);
    EXPECT_EQ(owner.reserved_risk_units(), 10);

    ts::RiskAdmission overflow_owner(config());
    ASSERT_EQ(overflow_owner.update_snapshot(snapshot((std::numeric_limits<std::int64_t>::max)())).kind,
              ts::RiskAdmissionKind::APPLIED);
    auto overflow_request = request(2, "overflow", 10, (std::numeric_limits<std::int64_t>::max)());
    overflow_request.cardinality = ts::ExecutionCardinality::SPLIT_GROUP;
    overflow_request.execution_group = {82, 2};
    const auto overflow = overflow_owner.reserve(overflow_request, 1000);
    ASSERT_EQ(overflow.kind, ts::RiskAdmissionKind::ADMITTED);
    const auto overflow_token = *overflow.reservation;
    ASSERT_EQ(overflow_owner.commit(overflow_token).kind, ts::RiskAdmissionKind::APPLIED);
    auto overflow_evidence = terminal_evidence(overflow_token, 2);
    overflow_evidence.settlements[0].kind = ts::OutcomeKind::WIN;
    overflow_evidence.settlements[0].net_profit = money((std::numeric_limits<std::int64_t>::max)());
    overflow_evidence.settlements[1].kind = ts::OutcomeKind::WIN;
    overflow_evidence.settlements[1].net_profit = money(1);
    overflow_evidence.outcome.kind = ts::OutcomeKind::WIN;
    overflow_evidence.outcome.net_profit = money((std::numeric_limits<std::int64_t>::max)());
    EXPECT_EQ(overflow_owner.settle(overflow_token, overflow_evidence).reason,
              ts::RiskAdmissionReason::INVALID_SETTLEMENT_EVIDENCE);
    EXPECT_EQ(overflow_owner.reserved_risk_units(), (std::numeric_limits<std::int64_t>::max)());
}

TEST(TradingSystemBatch4, ReservationIdsDoNotWrapAtUint64Maximum) {
    ts::RiskAdmission owner(config(), (std::numeric_limits<std::uint64_t>::max)());
    ASSERT_EQ(owner.update_snapshot(snapshot()).kind, ts::RiskAdmissionKind::APPLIED);
    const auto last = owner.reserve(request(1, "last"), 1000);
    ASSERT_EQ(last.kind, ts::RiskAdmissionKind::ADMITTED);
    EXPECT_EQ(last.reservation->reservation_id,
              (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_EQ(owner.reserve(request(2, "exhausted"), 1000).reason,
              ts::RiskAdmissionReason::NUMERIC_OVERFLOW);
    EXPECT_EQ(owner.reserved_risk_units(), 10);
}

TEST(TradingSystemBatch4, ReleaseAndSettlementRaceHasOneTerminalWinner) {
    ts::RiskAdmission owner(config());
    ASSERT_EQ(owner.update_snapshot(snapshot()).kind, ts::RiskAdmissionKind::APPLIED);
    const auto reserved = owner.reserve(request(1, "race", 10, 40), 1000);
    ASSERT_EQ(reserved.kind, ts::RiskAdmissionKind::ADMITTED);
    const auto token = *reserved.reservation;
    ASSERT_EQ(owner.commit(token).kind, ts::RiskAdmissionKind::APPLIED);
    const auto evidence = terminal_evidence(token);
    ts::RiskAdmissionResult release_result;
    ts::RiskAdmissionResult settle_result;
    std::thread release_thread([&] { release_result = owner.release(token); });
    std::thread settle_thread([&] { settle_result = owner.settle(token, evidence); });
    release_thread.join();
    settle_thread.join();
    EXPECT_EQ(release_result.kind, ts::RiskAdmissionKind::INVALID_STATE);
    EXPECT_EQ(settle_result.kind, ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserved_risk_units(), 0);
}

TEST(TradingSystemBatch4, SnapshotRevisionAndCurrencyAreGuarded) {
    ts::RiskAdmission owner(config());
    ASSERT_EQ(owner.update_snapshot(snapshot(100, 4)).kind, ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.update_snapshot(snapshot(100, 4)).kind, ts::RiskAdmissionKind::DUPLICATE);
    EXPECT_EQ(owner.update_snapshot(snapshot(100, 3)).kind, ts::RiskAdmissionKind::CONFLICT);
    auto changed = snapshot(100, 5);
    changed.available_balance.currency = CurrencyType::EUR;
    EXPECT_EQ(owner.update_snapshot(changed).reason, ts::RiskAdmissionReason::CURRENCY_MISMATCH);
}

TEST(TradingSystemBatch4, InvalidAndOverflowRiskNeverMutatesOwner) {
    ts::RiskAdmission owner(config());
    ASSERT_EQ(owner.update_snapshot(snapshot((std::numeric_limits<std::int64_t>::max)())).kind,
              ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserve(request(1, "zero", 0, 0), 1000).reason,
              ts::RiskAdmissionReason::INVALID_RISK);
    const auto huge = request(2, "huge", 1, (std::numeric_limits<std::int64_t>::max)());
    ASSERT_EQ(owner.reserve(huge, 1000).kind, ts::RiskAdmissionKind::ADMITTED);
    EXPECT_EQ(owner.reserved_risk_units(), (std::numeric_limits<std::int64_t>::max)());
    EXPECT_EQ(owner.reserve(request(3, "overflow", 1, 1), 1000).reason,
              ts::RiskAdmissionReason::NUMERIC_OVERFLOW);
}

TEST(TradingSystemBatch4, ConcurrentClientsCannotDoubleReserveBalance) {
    ts::RiskAdmission owner(config());
    ASSERT_EQ(owner.update_snapshot(snapshot(100)).kind, ts::RiskAdmissionKind::APPLIED);
    const auto first_request = request(1, "one", 10, 60);
    const auto second_request = request(2, "two", 10, 60);
    ts::RiskAdmissionResult first;
    ts::RiskAdmissionResult second;
    std::thread first_thread([&] { first = owner.reserve(first_request, 1000); });
    std::thread second_thread([&] { second = owner.reserve(second_request, 1000); });
    first_thread.join();
    second_thread.join();
    const auto applied = (first.kind == ts::RiskAdmissionKind::ADMITTED ? 1 : 0) +
                         (second.kind == ts::RiskAdmissionKind::ADMITTED ? 1 : 0);
    ASSERT_EQ(applied, 1);
    const auto rejected = first.kind == ts::RiskAdmissionKind::REJECTED ? first : second;
    EXPECT_EQ(rejected.reason, ts::RiskAdmissionReason::BALANCE_LIMIT_EXCEEDED);
    EXPECT_EQ(owner.reserved_risk_units(), 60);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
