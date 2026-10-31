#include <gtest/gtest.h>

#include <optionx_cpp/trading_system.hpp>

#include <cmath>
#include <limits>

namespace {

using optionx::CurrencyType;
using optionx::trading_system::BalanceSnapshot;
using optionx::trading_system::DataAvailability;
using optionx::trading_system::MoneyAmount;
using optionx::trading_system::MoneyRounding;
using optionx::trading_system::PolicyDecisionKind;
using optionx::trading_system::PolicyReason;
using optionx::trading_system::SizingDecisionKind;
using optionx::trading_system::SizingReason;
using optionx::trading_system::TradeIntent;
using optionx::trading_system::TradingDecisionContext;
using optionx::trading_system::TradingTimeWindow;

MoneyAmount money(const char* value, CurrencyType currency = CurrencyType::USD) {
    const auto parsed = MoneyAmount::from_decimal(value, currency, 2);
    EXPECT_TRUE(parsed.has_value());
    return parsed.value_or(MoneyAmount{});
}

TradingDecisionContext make_context() {
    TradingDecisionContext context;
    context.symbol = "EUR/USD";
    context.bridge_id = 7;
    context.signal_name = "alpha";
    context.evaluation_time_ms =
        time_shield::to_timestamp(2026, 10, 28, 9, 30, 0) * 1000;
    context.time_zone = optionx::TradeTimeZone::utc();
    context.account.availability = DataAvailability::SUPPORTED;
    context.account.currency = CurrencyType::USD;
    context.account.balance = money("100.00");
    return context;
}

TEST(TradingSystemBatch2, SymbolFilterNormalizesAliasesAndRejectsUnknowns) {
    const optionx::trading_system::SymbolFilter filter(
        {"eurusd"}, {}, {"EURUSD", "EURGBP"});
    const auto context = make_context();

    const auto allowed = filter.evaluate(context);
    EXPECT_EQ(allowed.kind, PolicyDecisionKind::ALLOW);
    EXPECT_EQ(allowed.reason, PolicyReason::MATCHED);

    auto denied_context = context;
    denied_context.symbol = "EUR/GBP";
    EXPECT_EQ(filter.evaluate(denied_context).kind, PolicyDecisionKind::REJECT);
    EXPECT_EQ(filter.evaluate(denied_context).reason, PolicyReason::SYMBOL_NOT_ALLOWED);

    denied_context.symbol = "UNKNOWN/PAIR";
    EXPECT_EQ(filter.evaluate(denied_context).kind, PolicyDecisionKind::UNAVAILABLE);
    EXPECT_EQ(filter.evaluate(denied_context).reason, PolicyReason::UNKNOWN_SYMBOL);
}

TEST(TradingSystemBatch2, SourceFilterCombinesBridgeAndSignalNameRules) {
    const optionx::trading_system::SourceFilter filter(
        {7}, {}, {"alpha"}, {"blocked"});
    const auto context = make_context();
    EXPECT_EQ(filter.evaluate(context).kind, PolicyDecisionKind::ALLOW);

    auto source = context;
    source.bridge_id = 8;
    EXPECT_EQ(filter.evaluate(source).reason, PolicyReason::SOURCE_NOT_ALLOWED);

    auto signal = context;
    signal.signal_name = "blocked";
    EXPECT_EQ(filter.evaluate(signal).reason, PolicyReason::SIGNAL_NAME_DENIED);

    source.bridge_id = 0;
    EXPECT_EQ(filter.evaluate(source).kind, PolicyDecisionKind::UNAVAILABLE);
    EXPECT_EQ(filter.evaluate(source).reason, PolicyReason::MISSING_SOURCE_ID);
}

TEST(TradingSystemBatch2, TradingWindowFilterHandlesWeekdaysAndOvernightWindows) {
    const optionx::trading_system::TradingWindowFilter daytime(
        {{9 * 3600, 10 * 3600}}, {}, {time_shield::WED});
    const auto context = make_context();
    EXPECT_EQ(daytime.evaluate(context).kind, PolicyDecisionKind::ALLOW);

    auto after_window = context;
    after_window.evaluation_time_ms =
        time_shield::to_timestamp(2026, 10, 28, 10, 0, 0) * 1000;
    EXPECT_EQ(daytime.evaluate(after_window).reason, PolicyReason::TIME_NOT_ALLOWED);

    const optionx::trading_system::TradingWindowFilter overnight(
        {{22 * 3600, 2 * 3600}});
    auto late = context;
    late.evaluation_time_ms =
        time_shield::to_timestamp(2026, 10, 28, 23, 30, 0) * 1000;
    EXPECT_EQ(overnight.evaluate(late).kind, PolicyDecisionKind::ALLOW);

    auto early = context;
    early.evaluation_time_ms =
        time_shield::to_timestamp(2026, 10, 29, 1, 30, 0) * 1000;
    EXPECT_EQ(overnight.evaluate(early).kind, PolicyDecisionKind::ALLOW);
}

TEST(TradingSystemBatch2, TradingWindowFilterUsesContextTimezoneAndRejectsMissingTime) {
    const optionx::trading_system::TradingWindowFilter filter(
        {{0, 1 * 3600}}, {}, {time_shield::THU});
    auto context = make_context();
    context.time_zone = optionx::TradeTimeZone::fixed_offset(3 * 3600);
    context.evaluation_time_ms =
        time_shield::to_timestamp(2026, 10, 28, 21, 30, 0) * 1000;
    EXPECT_EQ(filter.evaluate(context).kind, PolicyDecisionKind::ALLOW);

    context.evaluation_time_ms.reset();
    EXPECT_EQ(filter.evaluate(context).kind, PolicyDecisionKind::UNAVAILABLE);
    EXPECT_EQ(filter.evaluate(context).reason, PolicyReason::MISSING_EVALUATION_TIME);
}

TEST(TradingSystemBatch2, TradingWindowFilterUsesNamedZoneDstRules) {
    const optionx::trading_system::TradingWindowFilter filter(
        {{1 * 3600, 2 * 3600}}, {}, {time_shield::SUN});
    auto context = make_context();
    context.time_zone = optionx::TradeTimeZone::named(time_shield::CET);

    context.evaluation_time_ms =
        time_shield::to_timestamp(2024, 3, 31, 0, 30, 0) * 1000;
    EXPECT_EQ(filter.evaluate(context).kind, PolicyDecisionKind::ALLOW);

    context.evaluation_time_ms =
        time_shield::to_timestamp(2024, 3, 31, 1, 30, 0) * 1000;
    EXPECT_EQ(filter.evaluate(context).kind, PolicyDecisionKind::REJECT);
    EXPECT_EQ(filter.evaluate(context).reason, PolicyReason::TIME_NOT_ALLOWED);
}

TEST(TradingSystemBatch2, InvalidWindowConfigurationIsUnavailable) {
    const optionx::trading_system::TradingWindowFilter filter({{3600, 3600}});
    EXPECT_EQ(filter.evaluate(make_context()).kind, PolicyDecisionKind::UNAVAILABLE);
    EXPECT_EQ(filter.evaluate(make_context()).reason, PolicyReason::INVALID_CONFIGURATION);
}

TEST(TradingSystemBatch2, MoneyAmountRequiresExplicitPrecision) {
    const auto parsed = MoneyAmount::from_decimal("12.50", CurrencyType::USD, 2);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->units, 1250);
    EXPECT_EQ(parsed->to_decimal(), "12.50");

    EXPECT_FALSE(MoneyAmount::from_decimal("12.501", CurrencyType::USD, 2).has_value());
    EXPECT_FALSE(MoneyAmount::from_decimal("nan", CurrencyType::USD, 2).has_value());
}

TEST(TradingSystemBatch2, MoneyAmountPreservesSignedInt64Boundaries) {
    const auto maximum = MoneyAmount::from_decimal(
        "92233720368547758.07", CurrencyType::USD, 2);
    const auto minimum = MoneyAmount::from_decimal(
        "-92233720368547758.08", CurrencyType::USD, 2);

    ASSERT_TRUE(maximum.has_value());
    ASSERT_TRUE(minimum.has_value());
    EXPECT_EQ(maximum->units, (std::numeric_limits<std::int64_t>::max)());
    EXPECT_EQ(minimum->units, (std::numeric_limits<std::int64_t>::min)());
    EXPECT_EQ(maximum->to_decimal(), "92233720368547758.07");
    EXPECT_EQ(minimum->to_decimal(), "-92233720368547758.08");

    EXPECT_FALSE(MoneyAmount::from_decimal(
        "92233720368547758.08", CurrencyType::USD, 2).has_value());
    EXPECT_FALSE(MoneyAmount::from_decimal(
        "-92233720368547758.09", CurrencyType::USD, 2).has_value());
    EXPECT_FALSE(MoneyAmount::from_decimal(
        "1e2", CurrencyType::USD, 2).has_value());
}

TEST(TradingSystemBatch2, FixedStakeIsPureAndRejectsInvalidAmounts) {
    const auto context = make_context();
    const auto stake = optionx::trading_system::FixedStake(money("7.25"));
    const auto result = stake.evaluate(context);
    ASSERT_EQ(result.kind, SizingDecisionKind::SIZED);
    ASSERT_TRUE(result.amount.has_value());
    EXPECT_EQ(result.amount->to_decimal(), "7.25");

    const auto zero = optionx::trading_system::FixedStake(money("0.00"));
    EXPECT_EQ(zero.evaluate(context).reason, SizingReason::INVALID_AMOUNT);

    const auto negative = optionx::trading_system::FixedStake(money("-1.00"));
    EXPECT_EQ(negative.evaluate(context).reason, SizingReason::INVALID_AMOUNT);
}

TEST(TradingSystemBatch2, PercentOfBalanceUsesFixedPointAndExplicitRounding) {
    const auto context = make_context();
    const auto percent = optionx::trading_system::PercentOfBalance(12.5);
    const auto result = percent.evaluate(context);
    ASSERT_EQ(result.kind, SizingDecisionKind::SIZED);
    ASSERT_TRUE(result.amount.has_value());
    EXPECT_EQ(result.amount->to_decimal(), "12.50");

    const auto down = optionx::trading_system::PercentOfBalance::from_basis_points(
        5000, 2, MoneyRounding::DOWN);
    auto boundary = context;
    boundary.account.balance = money("1.01");
    EXPECT_EQ(down.evaluate(boundary).amount->to_decimal(), "0.50");

    const auto half_up = optionx::trading_system::PercentOfBalance::from_basis_points(
        5000, 2, MoneyRounding::HALF_UP);
    EXPECT_EQ(half_up.evaluate(boundary).amount->to_decimal(), "0.51");

    auto higher_precision = context;
    higher_precision.account.balance = MoneyAmount::from_decimal(
        "1.00", CurrencyType::USD, 2);
    const auto precise = optionx::trading_system::PercentOfBalance::from_basis_points(
        10, 3, MoneyRounding::DOWN);
    EXPECT_EQ(precise.evaluate(higher_precision).amount->to_decimal(), "0.001");

    const auto lower_precision = optionx::trading_system::PercentOfBalance::from_basis_points(
        5000, 1, MoneyRounding::DOWN);
    EXPECT_EQ(
        lower_precision.evaluate(context).reason,
        SizingReason::PRECISION_MISMATCH);
}

TEST(TradingSystemBatch2, PercentageRoundingUsesOutputPrecisionDenominator) {
    auto context = make_context();
    context.account.balance = money("1.01");

    const auto down = optionx::trading_system::PercentOfBalance::from_basis_points(
        1250, 4, MoneyRounding::DOWN);
    const auto half_up = optionx::trading_system::PercentOfBalance::from_basis_points(
        1250, 4, MoneyRounding::HALF_UP);
    const auto up = optionx::trading_system::PercentOfBalance::from_basis_points(
        1250, 4, MoneyRounding::UP);

    EXPECT_EQ(down.evaluate(context).amount->to_decimal(), "0.1262");
    EXPECT_EQ(half_up.evaluate(context).amount->to_decimal(), "0.1263");
    EXPECT_EQ(up.evaluate(context).amount->to_decimal(), "0.1263");
}

TEST(TradingSystemBatch2, PercentageSizingRejectsIntermediateOverflow) {
    auto context = make_context();
    context.account.balance = MoneyAmount{
        (std::numeric_limits<std::int64_t>::max)(), 0, CurrencyType::USD};

    const auto result = optionx::trading_system::PercentOfBalance::from_basis_points(
        10000, 1).evaluate(context);

    EXPECT_EQ(result.kind, SizingDecisionKind::REJECTED);
    EXPECT_EQ(result.reason, SizingReason::NUMERIC_OVERFLOW);
}

TEST(TradingSystemBatch2, PercentOfBalanceDistinguishesUnavailableAndInvalidBalance) {
    auto context = make_context();
    const auto percent = optionx::trading_system::PercentOfBalance(10.0);

    context.account.availability = DataAvailability::STALE;
    EXPECT_EQ(percent.evaluate(context).kind, SizingDecisionKind::UNAVAILABLE);
    EXPECT_EQ(percent.evaluate(context).reason, SizingReason::BALANCE_STALE);

    context.account.availability = DataAvailability::UNAVAILABLE;
    EXPECT_EQ(percent.evaluate(context).reason, SizingReason::BALANCE_UNAVAILABLE);

    context.account.availability = DataAvailability::SUPPORTED;
    context.account.balance.reset();
    EXPECT_EQ(percent.evaluate(context).reason, SizingReason::MISSING_BALANCE);

    context.account.balance = money("0.00");
    EXPECT_EQ(percent.evaluate(context).reason, SizingReason::ZERO_BALANCE);
    context.account.balance = money("-1.00");
    EXPECT_EQ(percent.evaluate(context).reason, SizingReason::NEGATIVE_BALANCE);
}

TEST(TradingSystemBatch2, PercentOfBalanceRejectsInvalidPercentages) {
    const auto context = make_context();
    EXPECT_EQ(
        optionx::trading_system::PercentOfBalance(0.0).evaluate(context).reason,
        SizingReason::INVALID_PERCENT);
    EXPECT_EQ(
        optionx::trading_system::PercentOfBalance(-1.0).evaluate(context).reason,
        SizingReason::INVALID_PERCENT);
    EXPECT_EQ(
        optionx::trading_system::PercentOfBalance(
            std::numeric_limits<double>::quiet_NaN()).evaluate(context).reason,
        SizingReason::INVALID_PERCENT);
    EXPECT_EQ(
        optionx::trading_system::PercentOfBalance(
            std::numeric_limits<double>::infinity()).evaluate(context).reason,
        SizingReason::INVALID_PERCENT);
    EXPECT_EQ(
        optionx::trading_system::PercentOfBalance(
            -std::numeric_limits<double>::infinity()).evaluate(context).reason,
        SizingReason::INVALID_PERCENT);
    EXPECT_EQ(
        optionx::trading_system::PercentOfBalance(
            std::numeric_limits<double>::denorm_min()).evaluate(context).reason,
        SizingReason::INVALID_PERCENT);
}

TEST(TradingSystemBatch2, PoliciesDoNotMutateContextAndAreDeterministic) {
    auto context = make_context();
    const auto original = context;
    const optionx::trading_system::SymbolFilter symbols({"EURUSD"});
    const auto first = symbols.evaluate(context);
    const auto second = symbols.evaluate(context);

    EXPECT_EQ(first.kind, second.kind);
    EXPECT_EQ(first.reason, second.reason);
    EXPECT_EQ(context.symbol, original.symbol);
    EXPECT_EQ(context.bridge_id, original.bridge_id);
    EXPECT_EQ(context.signal_name, original.signal_name);
    EXPECT_EQ(context.evaluation_time_ms, original.evaluation_time_ms);
    ASSERT_TRUE(context.account.balance.has_value());
    EXPECT_EQ(context.account.balance->units, original.account.balance->units);
}

TEST(TradingSystemBatch2, SignalFactoryOwnsItsDecisionSnapshot) {
    optionx::TradeSignal signal;
    signal.signal_id = 21;
    signal.bridge_id = 9;
    signal.symbol = "GBP/USD";
    signal.signal_name = "mean-reversion";

    auto account = make_context().account;
    account.availability = DataAvailability::SUPPORTED;
    account.currency = CurrencyType::USD;
    account.balance = money("25.00");

    const auto context = TradingDecisionContext::from_signal(
        signal,
        time_shield::to_timestamp(2026, 10, 28, 9, 30, 0) * 1000,
        optionx::TradeTimeZone::utc(),
        account);

    signal.symbol = "EUR/USD";
    signal.signal_name = "mutated-after-snapshot";
    account.balance->units = 1;

    EXPECT_EQ(context.signal_id, 21U);
    EXPECT_EQ(context.bridge_id, 9U);
    EXPECT_EQ(context.symbol, "GBP/USD");
    EXPECT_EQ(context.signal_name, "mean-reversion");
    ASSERT_TRUE(context.account.balance.has_value());
    EXPECT_EQ(context.account.balance->to_decimal(), "25.00");
}

TEST(TradingSystemBatch2, UsageChainCanEvaluateWithoutBrokerCalls) {
    TradeIntent intent;
    intent.intent_id = 10;
    intent.origin_signal_id = 7;
    intent.execution_group.expected_members = 1;
    intent.cardinality = optionx::trading_system::ExecutionCardinality::EXACTLY_ONE;
    intent.idempotency_key = "batch2-10";
    intent.request.symbol = "EUR/USD";
    intent.request.bridge_id = 7;
    intent.request.signal_name = "alpha";

    auto context = TradingDecisionContext::from_intent(
        intent,
        time_shield::to_timestamp(2026, 10, 28, 9, 30, 0) * 1000,
        optionx::TradeTimeZone::utc());
    context.account.availability = DataAvailability::SUPPORTED;
    context.account.currency = CurrencyType::USD;
    context.account.balance = money("100.00");

    const optionx::trading_system::SymbolFilter symbol_filter({"EURUSD"});
    const optionx::trading_system::SourceFilter source_filter({7}, {}, {"alpha"});
    const optionx::trading_system::TradingWindowFilter time_filter({{9 * 3600, 10 * 3600}});
    const auto stake = optionx::trading_system::FixedStake(money("5.00"));

    EXPECT_TRUE(symbol_filter.evaluate(context).is_allowed());
    EXPECT_TRUE(source_filter.evaluate(context).is_allowed());
    EXPECT_TRUE(time_filter.evaluate(context).is_allowed());
    EXPECT_TRUE(stake.evaluate(context).is_sized());
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
