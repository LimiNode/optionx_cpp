#include <optionx_cpp/trading_system.hpp>
#include <gtest/gtest.h>

#include <cfenv>
#include <cmath>
#include <limits>

namespace ts = optionx::trading_system;
namespace pm = optionx::trading_system::detail::platform_mapping;
using namespace optionx;

namespace {

struct Fixture {
    ts::TradeIntent intent;
    ts::ExecutionRosterRecord roster;
    ts::ExecutionOperationRecord operation;
    ts::AccountIdentityRecord account{11, "IntradeBar", "provider-account", "DEMO", 11, 1000};
    pm::MoneyProfile profile{CurrencyType::USD, 2};

    Fixture() {
        intent.intent_id = 20;
        intent.origin_signal_id = 21;
        intent.execution_group = {30, 1};
        intent.cardinality = ts::ExecutionCardinality::EXACTLY_ONE;
        intent.idempotency_key = "intent-20";
        intent.status = ts::IntentStatus::ADMITTED;
        intent.request.symbol = "EURUSD";
        intent.request.order_type = OrderType::BUY;
        intent.request.option_type = OptionType::SPRINT;
        intent.request.duration = 60;
        roster = {30, 20, 1, {{40, 11, 1000, {1000, 2, CurrencyType::USD}}},
                  {{40, 50, 1000, {1000, 2, CurrencyType::USD},
                    {1000, 2, CurrencyType::USD}, "reserve-50"}}};
        operation = {"operation-40", "client-40", 20, 30, 40, 11, 1000};
    }

    pm::MappingResult<TradeRequest> mapped() const {
        return pm::map_request(intent, roster, operation, account, profile);
    }

    TradeRequest request() const { return *mapped().value; }

    TradeResult result(TradeState state = TradeState::WIN) const {
        auto result = *request().create_trade_result_unique();
        result.platform_type = PlatformType::INTRADE_BAR;
        result.trade_state = state;
        result.option_id = 123;
        result.option_hash = "broker-hash";
        result.profit = state == TradeState::LOSS ? -10.0 :
            state == TradeState::WIN ? 8.0 : 0.0;
        return result;
    }

    pm::PlatformObservation observe(const TradeResult& value) const {
        return pm::map_observation(value, request(), operation, account, profile);
    }
};

TEST(TradingSystemBatch7B, CopiesRequestDataAndUsesAuthoritativeRosterStake) {
    Fixture f;
    auto& source = f.intent.request;
    source.signal_name = "strategy";
    source.signal_id = 21;
    source.bridge_id = 99;
    source.unique_id = 777;
    source.unique_hash = "local-hash-not-broker-idempotency";
    source.user_data = "metadata";
    source.comment = "comment";
    source.source_time_ms = 1000123;
    source.is_assumed = true;
    source.assumed_reason = "continuation";
    source.assumed_source_identity = "source";
    source.assumed_source_step = 2;
    source.expiry_time = 1700000060;
    source.refund = 0.1;
    source.min_payout = 0.8;
    source.amount = 100; // Logical template amount is not the physical stake.
    const nlohmann::json before = f.intent;
    const auto mapped = f.mapped();
    ASSERT_TRUE(mapped.succeeded());
    auto expected = source;
    expected.trade_id = 40;
    expected.account_id = 11;
    expected.account_type = AccountType::DEMO;
    expected.currency = CurrencyType::USD;
    expected.amount = 10;
    EXPECT_EQ(nlohmann::json(*mapped.value), nlohmann::json(expected));
    EXPECT_EQ(nlohmann::json(f.intent), before);
}

TEST(TradingSystemBatch7B, DoesNotCopyOrRegisterRequestCallbacks) {
    Fixture f;
    int calls = 0;
    f.intent.request.add_callback([&](auto, auto) { ++calls; });
    auto mapped = f.mapped();
    ASSERT_TRUE(mapped.succeeded());
    auto request = mapped.value->clone_unique();
    auto result = request->create_trade_result_unique();
    mapped.value->dispatch_callbacks(request, result);
    EXPECT_EQ(calls, 0);
    f.intent.request.dispatch_callbacks(request, result);
    EXPECT_EQ(calls, 1);
}

TEST(TradingSystemBatch7B, SplitGroupAndAliasRetainPhysicalAccountIdentity) {
    Fixture f;
    f.intent.cardinality = ts::ExecutionCardinality::SPLIT_GROUP;
    f.intent.execution_group.expected_members = f.roster.expected_members = 2;
    f.roster.members.push_back({41, 12, 1000, {500, 2, CurrencyType::USD}});
    f.roster.reservation_bindings.push_back(
        {41, 51, 1000, {500, 2, CurrencyType::USD}, {500, 2, CurrencyType::USD}, "reserve-51"});
    ASSERT_TRUE(f.mapped().succeeded());
    f.operation.trade_id = 41;
    f.operation.account_id = 12;
    f.account.account_id = 12; // Canonical account remains 11; no identity rewrite.
    const auto second = f.mapped();
    ASSERT_TRUE(second.succeeded());
    EXPECT_EQ(second.value->trade_id, 41U);
    EXPECT_EQ(second.value->account_id, 12);
    EXPECT_EQ(second.value->amount, 5);
}

TEST(TradingSystemBatch7B, RequestRejectsMismatchedBindingsAndRoster) {
    Fixture f;
    f.operation.intent_id++;
    EXPECT_EQ(f.mapped().error, pm::MappingError::INVALID_BINDING);
    f = Fixture{};
    f.operation.group_id++;
    EXPECT_EQ(f.mapped().error, pm::MappingError::INVALID_BINDING);
    f = Fixture{};
    f.operation.trade_id++;
    EXPECT_EQ(f.mapped().error, pm::MappingError::TRADE_MISMATCH);
    f = Fixture{};
    f.roster.members[0].account_id++;
    EXPECT_EQ(f.mapped().error, pm::MappingError::ACCOUNT_MISMATCH);
    f = Fixture{};
    f.roster.reservation_bindings[0].stake.units++;
    EXPECT_EQ(f.mapped().error, pm::MappingError::INVALID_BINDING);
    f = Fixture{};
    f.roster.schema_version++;
    EXPECT_EQ(f.mapped().error, pm::MappingError::INVALID_BINDING);
}

TEST(TradingSystemBatch7B, RequestRejectsConflictingTemplateIdentities) {
    Fixture f;
    f.intent.request.trade_id = 41;
    EXPECT_EQ(f.mapped().error, pm::MappingError::TRADE_MISMATCH);
    f = Fixture{};
    f.intent.request.signal_id = 22;
    EXPECT_EQ(f.mapped().error, pm::MappingError::TRADE_MISMATCH);
    f = Fixture{};
    f.intent.request.account_id = 12;
    EXPECT_EQ(f.mapped().error, pm::MappingError::ACCOUNT_MISMATCH);
    f = Fixture{};
    f.intent.request.account_type = AccountType::REAL;
    EXPECT_EQ(f.mapped().error, pm::MappingError::ACCOUNT_MISMATCH);
    f = Fixture{};
    f.intent.request.currency = CurrencyType::RUB;
    EXPECT_EQ(f.mapped().error, pm::MappingError::CURRENCY_MISMATCH);
}

TEST(TradingSystemBatch7B, ChecksPhysicalIdNarrowingBoundary) {
    Fixture f;
    const auto max = (std::numeric_limits<std::uint32_t>::max)();
    f.operation.trade_id = f.roster.members[0].trade_id =
        f.roster.reservation_bindings[0].trade_id = max;
    ASSERT_TRUE(f.mapped().succeeded());
    EXPECT_EQ(f.mapped().value->trade_id, max);
    f.operation.trade_id = static_cast<std::uint64_t>(max) + 1;
    EXPECT_EQ(f.mapped().error, pm::MappingError::ID_OUT_OF_RANGE);
}

TEST(TradingSystemBatch7B, ChecksAccountIdNarrowingBoundary) {
    Fixture f;
    const auto max = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    f.operation.account_id = f.account.account_id = f.account.canonical_account_id =
        f.roster.members[0].account_id = max;
    ASSERT_TRUE(f.mapped().succeeded());
    EXPECT_EQ(f.mapped().value->account_id, static_cast<std::int64_t>(max));
    ++f.operation.account_id;
    EXPECT_EQ(f.mapped().error, pm::MappingError::ID_OUT_OF_RANGE);
}

TEST(TradingSystemBatch7B, RequestValidatesNativeStakeAndParameters) {
    Fixture f;
    f.profile.precision = 3;
    EXPECT_EQ(f.mapped().error, pm::MappingError::INVALID_REQUEST);
    f = Fixture{};
    f.profile.currency = CurrencyType::RUB;
    EXPECT_EQ(f.mapped().error, pm::MappingError::CURRENCY_MISMATCH);
    f = Fixture{};
    f.intent.request.option_type = OptionType::UNKNOWN;
    EXPECT_EQ(f.mapped().error, pm::MappingError::INVALID_REQUEST);
    f = Fixture{};
    f.intent.request.expiry_time = -1;
    EXPECT_EQ(f.mapped().error, pm::MappingError::INVALID_REQUEST);
    f = Fixture{};
    f.intent.request.refund = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(f.mapped().error, pm::MappingError::INVALID_REQUEST);
}

TEST(TradingSystemBatch7B, NativeProfilesAreExplicitAndRoundTripChecked) {
    for (const auto profile : {pm::MoneyProfile{CurrencyType::RUB, 0},
                               pm::MoneyProfile{CurrencyType::BTC, 8},
                               pm::MoneyProfile{CurrencyType::ETH, 18}}) {
        const ts::MoneyAmount stake{500, static_cast<std::uint8_t>(profile.precision), profile.currency};
        const auto legacy = pm::stake_to_double(stake, profile);
        ASSERT_TRUE(legacy.succeeded());
        const auto restored = pm::normalize_money(*legacy.value, profile.currency, profile);
        ASSERT_TRUE(restored.succeeded());
        EXPECT_EQ(restored.value->units, 500);
    }
    EXPECT_EQ(pm::stake_to_double({9007199254740993LL, 0, CurrencyType::USD},
                                  {CurrencyType::USD, 0}).error, pm::MappingError::LOSSY_STAKE);
}

struct StateCase {
    TradeState state;
    pm::ObservationKind kind;
    bool opening;
    bool financial;
};

class StateMapping : public testing::TestWithParam<StateCase> {};

TEST_P(StateMapping, UsesOnlyTradeStateAndPreservesRawObservation) {
    Fixture f;
    const auto test = GetParam();
    auto raw = f.result(test.state);
    raw.live_state = test.financial ? TradeState::OPEN_ERROR : TradeState::WIN;
    const auto value = f.observe(raw);
    EXPECT_EQ(value.error, pm::MappingError::NONE);
    EXPECT_EQ(value.kind, test.kind);
    EXPECT_EQ(value.proves_opening, test.opening);
    EXPECT_EQ(value.net_profit.has_value(), test.financial);
    EXPECT_EQ(value.outcome.has_value(), test.financial);
    EXPECT_EQ(value.raw.trade_state, raw.trade_state);
    EXPECT_EQ(value.raw.live_state, raw.live_state);
    EXPECT_EQ(value.raw.option_hash, raw.option_hash);
    EXPECT_EQ(value.operation.operation_id, f.operation.operation_id);
    EXPECT_EQ(value.account.account_id, 11U);
}

INSTANTIATE_TEST_SUITE_P(AllStates, StateMapping, testing::Values(
    StateCase{TradeState::UNKNOWN, pm::ObservationKind::PROGRESS, false, false},
    StateCase{TradeState::WAITING_OPEN, pm::ObservationKind::PROGRESS, false, false},
    StateCase{TradeState::OPEN_SUCCESS, pm::ObservationKind::OPEN_CONFIRMED, true, false},
    StateCase{TradeState::OPEN_ERROR, pm::ObservationKind::FAILED_OPEN, false, false},
    StateCase{TradeState::IN_PROGRESS, pm::ObservationKind::OPEN_CONFIRMED, true, false},
    StateCase{TradeState::WAITING_CLOSE, pm::ObservationKind::OPEN_CONFIRMED, true, false},
    StateCase{TradeState::CHECK_ERROR, pm::ObservationKind::RESULT_CHECK_FAILED, true, false},
    StateCase{TradeState::WIN, pm::ObservationKind::FINANCIAL_RESULT, true, true},
    StateCase{TradeState::LOSS, pm::ObservationKind::FINANCIAL_RESULT, true, true},
    StateCase{TradeState::STANDOFF, pm::ObservationKind::FINANCIAL_RESULT, true, true},
    StateCase{TradeState::REFUND, pm::ObservationKind::FINANCIAL_RESULT, true, true},
    StateCase{TradeState::CANCELED_TRADE, pm::ObservationKind::CANCELLATION_REPORTED, false, false}));

TEST(TradingSystemBatch7B, UnknownNumericStateFailsWithoutFabricatingFacts) {
    Fixture f;
    auto raw = f.result(static_cast<TradeState>(999));
    const auto mapped = f.observe(raw);
    EXPECT_EQ(mapped.error, pm::MappingError::UNSUPPORTED_STATE);
    EXPECT_EQ(static_cast<int>(mapped.raw.trade_state), 999);
    EXPECT_FALSE(mapped.net_profit);
    EXPECT_FALSE(mapped.outcome);
}

TEST(TradingSystemBatch7B, OpenErrorPreservesOriginalErrorWithoutSettlementEvidence) {
    Fixture f;
    auto raw = f.result(TradeState::OPEN_ERROR);
    raw.option_id = 0;
    raw.error_code = TradeErrorCode::PARSING_ERROR;
    raw.error_desc = "Trade open failed. Server returned an empty response; instrument may be closed or unavailable.";
    const auto mapped = f.observe(raw);
    EXPECT_EQ(mapped.error, pm::MappingError::NONE);
    EXPECT_EQ(mapped.kind, pm::ObservationKind::FAILED_OPEN);
    EXPECT_EQ(mapped.raw.error_code, raw.error_code);
    EXPECT_EQ(mapped.raw.error_desc, raw.error_desc);
    EXPECT_FALSE(mapped.proves_opening);
    EXPECT_FALSE(mapped.outcome);
    EXPECT_FALSE(mapped.net_profit);
}

TEST(TradingSystemBatch7B, CheckErrorIgnoresEstimatedProfitAndPreservesOpening) {
    Fixture f;
    auto raw = f.result(TradeState::CHECK_ERROR);
    raw.profit = 8.5;
    raw.live_state = TradeState::WIN;
    const auto mapped = f.observe(raw);
    EXPECT_EQ(mapped.error, pm::MappingError::NONE);
    EXPECT_EQ(mapped.kind, pm::ObservationKind::RESULT_CHECK_FAILED);
    EXPECT_TRUE(mapped.proves_opening);
    EXPECT_EQ(mapped.broker_trade_id, "123");
    EXPECT_EQ(mapped.raw.profit, 8.5);
    EXPECT_FALSE(mapped.net_profit);
    EXPECT_FALSE(mapped.outcome);
}

TEST(TradingSystemBatch7B, OpeningWithMissingBrokerIdRemainsConfirmed) {
    Fixture f;
    auto raw = f.result(TradeState::OPEN_SUCCESS);
    raw.option_id = 0;
    const auto mapped = f.observe(raw);
    EXPECT_EQ(mapped.error, pm::MappingError::NONE);
    EXPECT_TRUE(mapped.proves_opening);
    EXPECT_TRUE(mapped.broker_trade_id.empty()); // Hash alone is not an Intrade numeric ID.
    EXPECT_FALSE(mapped.net_profit);
}

TEST(TradingSystemBatch7B, NetProfitIsNotReducedByStakeAgain) {
    Fixture f;
    const auto win = f.observe(f.result());
    ASSERT_TRUE(win.net_profit);
    EXPECT_EQ(win.net_profit->units, 800);
    EXPECT_EQ(win.outcome, ts::OutcomeKind::WIN);
    const auto loss = f.observe(f.result(TradeState::LOSS));
    ASSERT_TRUE(loss.net_profit);
    EXPECT_EQ(loss.net_profit->units, -1000);
    EXPECT_EQ(loss.outcome, ts::OutcomeKind::LOSS);
    const auto draw = f.observe(f.result(TradeState::STANDOFF));
    ASSERT_TRUE(draw.net_profit);
    EXPECT_EQ(draw.net_profit->units, 0);
    EXPECT_EQ(draw.outcome, ts::OutcomeKind::DRAW);
}

TEST(TradingSystemBatch7B, RefundMustBeExactFullRefundAndRetainsOriginalState) {
    Fixture f;
    auto raw = f.result(TradeState::REFUND);
    auto mapped = f.observe(raw);
    ASSERT_TRUE(mapped.net_profit);
    EXPECT_EQ(mapped.raw.trade_state, TradeState::REFUND);
    EXPECT_EQ(mapped.outcome, ts::OutcomeKind::DRAW);
    for (const auto profit : {-5.0, 0.00001, 5.0}) {
        raw.profit = profit;
        mapped = f.observe(raw);
        EXPECT_EQ(mapped.error, pm::MappingError::UNSUPPORTED_REFUND);
        EXPECT_EQ(mapped.raw.profit, profit);
        EXPECT_FALSE(mapped.outcome);
        EXPECT_FALSE(mapped.net_profit);
    }
}

TEST(TradingSystemBatch7B, ValidatesRawAndQuantizedOutcomeSigns) {
    Fixture f;
    for (const auto state : {TradeState::WIN, TradeState::LOSS, TradeState::STANDOFF}) {
        for (const auto profit : {-1.0, -0.00001, 0.0, 0.00001, 1.0}) {
            auto raw = f.result(state);
            raw.profit = profit;
            const bool valid = (state == TradeState::WIN && profit == 1) ||
                (state == TradeState::LOSS && profit == -1) ||
                (state == TradeState::STANDOFF && profit == 0);
            const auto mapped = f.observe(raw);
            EXPECT_EQ(mapped.error, valid ? pm::MappingError::NONE : pm::MappingError::INVALID_OUTCOME);
            EXPECT_EQ(mapped.net_profit.has_value(), valid);
        }
    }
}

TEST(TradingSystemBatch7B, UsesExactBinaryValueWithNearestEvenRounding) {
    const pm::MoneyProfile profile{CurrencyType::USD, 2};
    for (const auto pair : {std::pair<double, std::int64_t>{1.125, 112}, {1.375, 138},
                            {-1.125, -112}, {-1.375, -138}, {2.675, 267},
                            {1.005, 100}, {-0.0, 0}}) {
        const auto result = pm::normalize_money(pair.first, profile.currency, profile);
        ASSERT_TRUE(result.succeeded());
        EXPECT_EQ(result.value->units, pair.second);
    }
    EXPECT_EQ(pm::normalize_money(std::nextafter(1.125, 0.0), profile.currency, profile).value->units, 112);
    EXPECT_EQ(pm::normalize_money(std::nextafter(1.125, 2.0), profile.currency, profile).value->units, 113);
}

TEST(TradingSystemBatch7B, NormalizationDoesNotDependOnFloatingRoundingMode) {
    const auto previous = std::fegetround();
    for (const auto mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO, FE_TONEAREST}) {
        EXPECT_EQ(std::fesetround(mode), 0);
        const auto result = pm::normalize_money(1.125, CurrencyType::USD, {CurrencyType::USD, 2});
        EXPECT_TRUE(result.succeeded());
        if (result.value) EXPECT_EQ(result.value->units, 112);
    }
    EXPECT_EQ(std::fesetround(previous), 0);
}

TEST(TradingSystemBatch7B, MoneyRejectsInvalidProfilesAndNonfiniteOrOverflowValues) {
    EXPECT_EQ(pm::normalize_money(1, CurrencyType::USD, {}).error, pm::MappingError::UNSUPPORTED_CURRENCY);
    for (const auto precision : {-1, 19, 255}) {
        EXPECT_EQ(pm::normalize_money(1, CurrencyType::USD, {CurrencyType::USD, precision}).error,
                  pm::MappingError::INVALID_PRECISION);
    }
    for (const auto currency : {CurrencyType::UNKNOWN, static_cast<CurrencyType>(999)}) {
        EXPECT_EQ(pm::normalize_money(1, currency, {CurrencyType::USD, 2}).error,
                  pm::MappingError::UNSUPPORTED_CURRENCY);
    }
    EXPECT_EQ(pm::normalize_money(1, CurrencyType::RUB, {CurrencyType::USD, 2}).error,
              pm::MappingError::CURRENCY_MISMATCH);
    for (const auto value : {std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity()}) {
        EXPECT_EQ(pm::normalize_money(value, CurrencyType::USD, {CurrencyType::USD, 2}).error,
                  pm::MappingError::NON_FINITE_MONEY);
    }
    for (const auto value : {std::numeric_limits<double>::max(),
                             -std::numeric_limits<double>::max(), 9223372036854775808.0}) {
        EXPECT_EQ(pm::normalize_money(value, CurrencyType::USD, {CurrencyType::USD, 0}).error,
                  pm::MappingError::NUMERIC_OVERFLOW);
    }
    const auto minimum = pm::normalize_money(-9223372036854775808.0, CurrencyType::USD, {CurrencyType::USD, 0});
    ASSERT_TRUE(minimum.succeeded());
    EXPECT_EQ(minimum.value->units, (std::numeric_limits<std::int64_t>::min)());
}

TEST(TradingSystemBatch7B, ObservationRejectsForeignIdentityAndKeepsRawFacts) {
    Fixture f;
    auto raw = f.result();
    auto request = f.request();
    raw.trade_id++;
    EXPECT_EQ(f.observe(raw).error, pm::MappingError::TRADE_MISMATCH);
    raw = f.result();
    request.account_id++;
    EXPECT_EQ(pm::map_observation(raw, request, f.operation, f.account, f.profile).error,
              pm::MappingError::ACCOUNT_MISMATCH);
    raw.account_type = AccountType::REAL;
    EXPECT_EQ(f.observe(raw).error, pm::MappingError::ACCOUNT_MISMATCH);
    raw = f.result();
    raw.platform_type = PlatformType::SIMULATOR;
    EXPECT_EQ(f.observe(raw).error, pm::MappingError::UNSUPPORTED_PLATFORM);
    raw = f.result();
    raw.currency = CurrencyType::RUB;
    EXPECT_EQ(f.observe(raw).error, pm::MappingError::CURRENCY_MISMATCH);
    raw = f.result();
    request = f.request();
    f.account.account_id++;
    EXPECT_EQ(pm::map_observation(raw, request, f.operation, f.account, f.profile).error,
              pm::MappingError::ACCOUNT_MISMATCH);
}

TEST(TradingSystemBatch7B, BrokerIdentityIsNumericScopedAndNeverAReplacementTradeId) {
    Fixture f;
    auto raw = f.result();
    f.operation.broker_trade_id = "123";
    EXPECT_EQ(f.observe(raw).error, pm::MappingError::NONE);
    raw.option_id = 0;
    EXPECT_EQ(f.observe(raw).broker_trade_id, "123");
    raw.option_id = 124;
    EXPECT_EQ(f.observe(raw).error, pm::MappingError::BROKER_ID_MISMATCH);
    raw.option_id = -1;
    EXPECT_EQ(f.observe(raw).error, pm::MappingError::INVALID_BROKER_ID);
    for (const auto text : {"00123", "0", "hash-only", "9223372036854775808"}) {
        f.operation.broker_trade_id = text;
        EXPECT_EQ(f.observe(f.result()).error, pm::MappingError::INVALID_BROKER_ID);
    }
    f.operation.broker_trade_id.clear();
    raw = f.result();
    raw.option_id = 0;
    const auto missing = f.observe(raw);
    EXPECT_EQ(missing.error, pm::MappingError::MISSING_BROKER_ID);
    EXPECT_TRUE(missing.proves_opening);
    EXPECT_FALSE(missing.net_profit);
}

TEST(TradingSystemBatch7B, TombstoneAllowsObservationButNotNewRequest) {
    Fixture f;
    auto request = f.request();
    auto raw = f.result();
    f.account.active = false;
    EXPECT_EQ(f.mapped().error, pm::MappingError::ACCOUNT_INACTIVE);
    const auto mapped = pm::map_observation(raw, request, f.operation, f.account, f.profile);
    EXPECT_EQ(mapped.error, pm::MappingError::NONE);
    EXPECT_TRUE(mapped.net_profit);
}

TEST(TradingSystemBatch7B, FinancialNormalizationFailuresKeepUnmodifiedEvidence) {
    Fixture f;
    auto raw = f.result();
    raw.amount = 11;
    EXPECT_EQ(f.observe(raw).error, pm::MappingError::STAKE_MISMATCH);
    raw = f.result();
    for (const auto value : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::max()}) {
        raw.profit = value;
        const auto mapped = f.observe(raw);
        EXPECT_NE(mapped.error, pm::MappingError::NONE);
        EXPECT_TRUE(mapped.proves_opening);
        EXPECT_FALSE(mapped.outcome);
        EXPECT_FALSE(mapped.net_profit);
    }
}

TEST(TradingSystemBatch7B, RepeatedConversionsDoNotAllocateOrMutateIdentity) {
    Fixture f;
    const auto raw = f.result();
    const auto first = f.observe(raw);
    for (int i = 0; i < 50; ++i) {
        const auto next = f.observe(raw);
        EXPECT_EQ(nlohmann::json(f.request()), nlohmann::json(f.request()));
        EXPECT_EQ(nlohmann::json(next.raw), nlohmann::json(first.raw));
        EXPECT_EQ(nlohmann::json(next.operation), nlohmann::json(first.operation));
        EXPECT_EQ(next.kind, first.kind);
        EXPECT_EQ(next.error, first.error);
        EXPECT_EQ(next.outcome, first.outcome);
        ASSERT_TRUE(next.net_profit);
        EXPECT_EQ(next.net_profit->units, first.net_profit->units);
        EXPECT_EQ(next.broker_trade_id, first.broker_trade_id);
        EXPECT_EQ(f.operation.state_revision, 0U);
    }
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
