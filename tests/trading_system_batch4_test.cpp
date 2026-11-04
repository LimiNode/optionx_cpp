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
    EXPECT_EQ(owner.settle(token).kind, ts::RiskAdmissionKind::APPLIED);
    EXPECT_EQ(owner.reserved_risk_units(), 0);
    EXPECT_EQ(owner.settle(token).kind, ts::RiskAdmissionKind::DUPLICATE);
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
