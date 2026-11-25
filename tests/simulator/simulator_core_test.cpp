#include <optionx_cpp/simulator.hpp>
#include <gtest/gtest.h>
#include <memory>

using namespace optionx;
using namespace optionx::platforms;

namespace {
SimulatorPolicy policy() {
    SimulatorPolicy value;
    value.defaults.available = true;
    value.defaults.payout_ppm = 750000;
    value.defaults.min_stake_minor = 1;
    value.defaults.max_stake_minor = 10000;
    value.defaults.max_active_global = 10;
    value.defaults.max_active_symbol = 5;
    value.allowed_expiry = {60, 300};
    value.symbols["EURUSD"] = {};
    value.symbols["GBPUSD"] = {};
    return value;
}
SimulatorPolicySnapshot snapshot(SimulatorPolicy value = policy()) {
    return *SimulatorPolicySnapshot::create(std::move(value));
}
SimulatorAccountConfig account(std::int64_t balance = 100000) {
    return {"paper-test", CurrencyType::USD, 2, balance};
}
SimulatorOrder order(OrderType direction = OrderType::BUY, std::int64_t stake = 1000,
                     std::uint32_t duration = 60, std::string symbol = "EURUSD") {
    return {std::move(symbol), direction, stake, duration};
}
SimulatorQuoteFixture quotes(std::vector<SimulatorTick> ticks = {{"EURUSD", 0, 100}, {"EURUSD", 60000, 101}}) {
    return SimulatorQuoteFixture(std::move(ticks));
}
void invariant(const SimulatorCore& core) {
    const auto a = core.account();
    EXPECT_EQ(a.available_minor + a.reserved_minor + a.exposure_minor, a.balance_minor);
    EXPECT_GE(a.available_minor, 0);
    EXPECT_GE(a.reserved_minor, 0);
    EXPECT_GE(a.exposure_minor, 0);
}
}

TEST(SimulatorCoreTest, OutcomesDirectionsAndAllRequestedDelayDurationCombinations) {
    for (const auto open_delay : {0, 150, 500}) {
        for (const auto response_delay : {0, 100, 500}) {
            for (const auto duration : {60u, 120u, 300u}) {
                for (const auto direction : {OrderType::BUY, OrderType::SELL}) {
                    for (const auto close_price : {99, 100, 101}) {
                        const std::int64_t expiry = open_delay + duration * 1000;
                        SimulatorCore core(account(), {open_delay, response_delay, 0, 10}, snapshot(),
                            quotes({{"EURUSD", open_delay, 100}, {"EURUSD", expiry, close_price},
                                    {"EURUSD", expiry + 1, 1000}}));
                        int callbacks = 0;
                        const auto id = *core.enqueue(order(direction, 1000, duration), [&](const auto& result) {
                            ++callbacks;
                            EXPECT_EQ(core.now_ms(), expiry + response_delay);
                            EXPECT_EQ(result.callback_ms, expiry + response_delay);
                        });
                        EXPECT_EQ(core.account().reserved_minor, 0);
                        core.process();
                        if (open_delay) {
                            EXPECT_EQ(core.query(id)->phase, SimulatorTradePhase::PENDING);
                            EXPECT_EQ(core.account().reserved_minor, 1000);
                        }
                        core.advance_to(expiry);
                        const bool win = direction == OrderType::BUY ? close_price > 100 : close_price < 100;
                        const auto expected = close_price == 100 ? 0 : win ? 750 : -1000;
                        const auto result = *core.query(id);
                        EXPECT_EQ(result.outcome, close_price == 100 ? TradeState::STANDOFF : win ? TradeState::WIN : TradeState::LOSS);
                        EXPECT_EQ(result.net_profit_minor, expected);
                        EXPECT_EQ(result.open_ms, open_delay);
                        EXPECT_EQ(result.expiry_ms, expiry);
                        EXPECT_EQ(result.settlement_ms, expiry);
                        EXPECT_EQ(result.close_quote->price_units, close_price);
                        EXPECT_EQ(core.account().balance_minor, 100000 + expected);
                        EXPECT_EQ(core.account().open_trades, 0u);
                        EXPECT_EQ(callbacks, response_delay == 0 ? 1 : 0);
                        core.advance_to(expiry + response_delay + 100000);
                        core.process();
                        core.shutdown();
                        core.shutdown();
                        EXPECT_EQ(callbacks, 1);
                        EXPECT_EQ(core.account().balance_minor, 100000 + expected);
                        invariant(core);
                    }
                }
            }
        }
    }
}

TEST(SimulatorCoreTest, LargeAdvanceUsesEventTimeForPolicyFreshnessPriceAndCallback) {
    auto value = policy();
    value.rules.push_back({"later", 1, {{}, {}, SimulatorTimeWindow{1, 2}, {}}, {}});
    value.rules.back().overrides.payout_ppm = 500000;
    SimulatorCore core(account(), {150, 500, 0, 10}, snapshot(value),
        quotes({{"EURUSD", 150, 100}, {"EURUSD", 60150, 101}, {"EURUSD", 100000, 1}}));
    const auto id = *core.enqueue(order());
    ASSERT_TRUE(core.advance_to(100000));
    const auto result = *core.query(id);
    EXPECT_EQ(result.open_ms, 150);
    EXPECT_EQ(result.payout_ppm, 750000);
    EXPECT_EQ(result.close_quote->timestamp_ms, 60150);
    EXPECT_EQ(result.net_profit_minor, 750);
    EXPECT_EQ(result.callback_ms, 60650);
    EXPECT_EQ(core.now_ms(), 100000);
    EXPECT_FALSE(core.advance_to(99999));
}

TEST(SimulatorCoreTest, ActualOpenFreezesNewPolicyAndNeverRepricesExistingTrade) {
    SimulatorCore core(account(), {150, 500, 0, 10}, snapshot(),
        quotes({{"EURUSD", 150, 100}, {"EURUSD", 60150, 101}}));
    const auto id = *core.enqueue(order());
    core.process();
    auto value = policy();
    value.version = 2;
    value.defaults.payout_ppm = 500000;
    ASSERT_TRUE(core.set_policy(snapshot(value)));
    core.advance_to(150);
    EXPECT_EQ(core.query(id)->policy_version, 2u);
    EXPECT_EQ(core.query(id)->payout_ppm, 500000);
    value.version = 3;
    value.defaults.payout_ppm = 900000;
    core.set_policy(snapshot(value));
    core.advance_to(60150);
    EXPECT_EQ(core.query(id)->net_profit_minor, 500);
    EXPECT_EQ(core.query(id)->policy_version, 2u);
    EXPECT_FALSE(core.query(id)->callback_ms);
    EXPECT_EQ(core.account().balance_minor, 100500);
    core.advance_to(60650);
    EXPECT_EQ(core.account().balance_minor, 100500);
}

TEST(SimulatorCoreTest, CrossingWindowRevalidatesAvailabilityStakeAndCapsAndReleasesOnce) {
    for (int change = 0; change < 4; ++change) {
        auto value = policy();
        SimulatorRule rule;
        rule.id = "after-second-one";
        rule.selector.utc_window = SimulatorTimeWindow{1, 2};
        if (change == 0) rule.overrides.available = false;
        if (change == 1) rule.overrides.max_stake_minor = 500;
        if (change == 2) rule.overrides.max_active_global = 0;
        if (change == 3) rule.overrides.max_active_symbol = 0;
        value.rules.push_back(rule);
        SimulatorCore core(account(), {150, 0, 0, 10}, snapshot(value), quotes({{"EURUSD", 1050, 100}}), 900);
        const auto id = *core.enqueue(order());
        core.process();
        EXPECT_EQ(core.account().reserved_minor, 1000);
        core.advance_to(1050);
        EXPECT_EQ(core.query(id)->phase, SimulatorTradePhase::FAILED_OPEN);
        EXPECT_FALSE(core.query(id)->open_ms);
        EXPECT_FALSE(core.query(id)->net_profit_minor);
        EXPECT_EQ(core.account().reserved_minor, 0);
        EXPECT_EQ(core.account().pending_trades, 0u);
        core.process();
        core.shutdown();
        invariant(core);
        EXPECT_EQ(core.account().balance_minor, 100000);
    }
}

TEST(SimulatorCoreTest, PolicyUpdateRejectsSameOrOlderVersionWithoutChangingFutureOpen) {
    SimulatorCore core(account(), {150, 0, 0, 10}, snapshot(),
        quotes({{"EURUSD", 150, 100}, {"EURUSD", 60150, 101}}));
    const auto id = *core.enqueue(order());
    core.process();
    auto changed = policy();
    changed.defaults.payout_ppm = 100000;
    EXPECT_FALSE(core.set_policy(snapshot(changed)));
    changed.version = 2;
    changed.defaults.payout_ppm = 500000;
    EXPECT_TRUE(core.set_policy(snapshot(changed)));
    EXPECT_FALSE(core.set_policy(snapshot()));
    changed.defaults.payout_ppm = 900000;
    EXPECT_FALSE(core.set_policy(snapshot(changed)));
    core.advance_to(60150);
    EXPECT_EQ(core.query(id)->policy_version, 2u);
    EXPECT_EQ(core.query(id)->payout_ppm, 500000);
    EXPECT_EQ(core.query(id)->net_profit_minor, 500);
    core.shutdown();
    changed.version = 3;
    EXPECT_FALSE(core.set_policy(snapshot(changed)));
}

TEST(SimulatorCoreTest, WindowAtOpenOverridesPayoutAndRecordsMatchingIdentity) {
    auto value = policy();
    SimulatorRule rule;
    rule.id = "window";
    rule.selector.utc_window = SimulatorTimeWindow{1, 2};
    rule.overrides.payout_ppm = 250000;
    value.rules.push_back(rule);
    SimulatorCore core(account(), {500, 0, 0, 10}, snapshot(value),
        quotes({{"EURUSD", 1400, 100}, {"EURUSD", 61400, 101}}), 900);
    const auto id = *core.enqueue(order());
    core.advance_to(100000);
    EXPECT_EQ(core.query(id)->payout_ppm, 250000);
    EXPECT_EQ(core.query(id)->applied_rule_ids, std::vector<std::string>{"window"});
    EXPECT_EQ(core.query(id)->net_profit_minor, 250);
}

TEST(SimulatorCoreTest, PendingReservationsEnforceGlobalAndSymbolCapsAcrossDirectionAndDuration) {
    auto value = policy();
    value.defaults.max_active_global = 2;
    value.defaults.max_active_symbol = 1;
    SimulatorCore core(account(), {500, 0, 0, 10}, snapshot(value),
        quotes({{"EURUSD", 500, 100}, {"GBPUSD", 500, 100}}));
    const auto a = *core.enqueue(order());
    const auto b = *core.enqueue(order(OrderType::SELL, 1000, 300));
    const auto c = *core.enqueue(order(OrderType::SELL, 1000, 120, "GBPUSD"));
    const auto d = *core.enqueue(order(OrderType::BUY, 1000, 60, "GBPUSD"));
    core.process();
    EXPECT_EQ(core.query(a)->phase, SimulatorTradePhase::PENDING);
    EXPECT_EQ(core.query(b)->error, TradeErrorCode::LIMIT_OPEN_TRADES);
    EXPECT_EQ(core.query(c)->phase, SimulatorTradePhase::PENDING);
    EXPECT_EQ(core.query(d)->error, TradeErrorCode::LIMIT_OPEN_TRADES);
    EXPECT_EQ(core.account().pending_trades, 2u);
    EXPECT_EQ(core.account().reserved_minor, 2000);
    core.advance_to(500);
    EXPECT_EQ(core.account().open_trades, 2u);
    EXPECT_EQ(core.account().exposure_minor, 2000);
    invariant(core);
}

TEST(SimulatorCoreTest, ReducedDelayedCapCountsOwnSlotWithoutDoubleCounting) {
    auto value = policy();
    value.defaults.max_active_global = 2;
    SimulatorRule rule;
    rule.id = "smaller";
    rule.selector.utc_window = SimulatorTimeWindow{1, 2};
    rule.overrides.max_active_global = 1;
    value.rules.push_back(rule);
    SimulatorCore core(account(), {150, 0, 0, 10}, snapshot(value), quotes({{"EURUSD", 1050, 100}}), 900);
    const auto a = *core.enqueue(order());
    const auto b = *core.enqueue(order());
    core.process();
    core.advance_to(1050);
    EXPECT_EQ(core.query(a)->error, TradeErrorCode::LIMIT_OPEN_TRADES);
    EXPECT_EQ(core.query(b)->phase, SimulatorTradePhase::OPEN);
    EXPECT_EQ(core.account().open_trades, 1u);
    EXPECT_EQ(core.account().reserved_minor, 0);
    invariant(core);
}

TEST(SimulatorCoreTest, InsufficientBalanceIsAtomicAcrossQueuedCommands) {
    SimulatorCore core(account(1500), {500, 0, 0, 10}, snapshot(), quotes());
    const auto a = *core.enqueue(order());
    const auto b = *core.enqueue(order());
    core.process();
    EXPECT_EQ(core.query(a)->phase, SimulatorTradePhase::PENDING);
    EXPECT_EQ(core.query(b)->error, TradeErrorCode::INSUFFICIENT_BALANCE);
    EXPECT_TRUE(core.query(b)->virtual_trade_id.empty());
    EXPECT_EQ(core.account().available_minor, 500);
    core.shutdown();
    EXPECT_EQ(core.account().available_minor, 1500);
    invariant(core);
}

TEST(SimulatorCoreTest, InvalidRequestsAndExcludedExpiryFailBeforeReserve) {
    auto value = policy();
    value.defaults.min_stake_minor = 100;
    value.excluded_expiries = {120};
    SimulatorCore core(account(), {}, snapshot(value), quotes());
    const std::vector<std::pair<SimulatorOrder, TradeErrorCode>> cases = {
        {order(OrderType::BUY, 99), TradeErrorCode::AMOUNT_TOO_LOW},
        {order(OrderType::BUY, 10001), TradeErrorCode::AMOUNT_TOO_HIGH},
        {order(OrderType::BUY, -1), TradeErrorCode::AMOUNT_TOO_LOW},
        {order(OrderType::BUY, 1000, 120), TradeErrorCode::INVALID_DURATION},
        {order(OrderType::BUY, 1000, 59), TradeErrorCode::INVALID_DURATION},
        {order(OrderType::BUY, 1000, 301), TradeErrorCode::INVALID_DURATION},
        {order(OrderType::BUY, 1000, 60, "UNKNOWN"), TradeErrorCode::INVALID_SYMBOL},
        {order(static_cast<OrderType>(999)), TradeErrorCode::INVALID_ORDER}
    };
    for (const auto& test : cases) {
        const auto id = *core.enqueue(test.first);
        core.process();
        EXPECT_EQ(core.query(id)->error, test.second);
        EXPECT_TRUE(core.query(id)->virtual_trade_id.empty());
        EXPECT_EQ(core.account().reserved_minor, 0);
        invariant(core);
    }
    for (const auto stake : {100, 10000}) {
        const auto id = *core.enqueue(order(OrderType::BUY, stake));
        core.process();
        EXPECT_EQ(core.query(id)->phase, SimulatorTradePhase::OPEN);
    }
}

TEST(SimulatorCoreTest, MissingStaleAndFutureOpeningQuotesReleasePendingExactlyOnce) {
    for (const auto& ticks : std::vector<std::vector<SimulatorTick>>{
            {}, {{"EURUSD", 0, 100}}, {{"EURUSD", 501, 100}}}) {
        SimulatorCore core(account(), {500, 0, 100, 10}, snapshot(), quotes(ticks));
        int callbacks = 0;
        const auto id = *core.enqueue(order(), [&](const auto&) { ++callbacks; });
        core.process();
        core.advance_to(1000);
        EXPECT_EQ(core.query(id)->phase, SimulatorTradePhase::FAILED_OPEN);
        EXPECT_EQ(core.query(id)->error, TradeErrorCode::NO_CONNECTION);
        EXPECT_FALSE(core.query(id)->open_quote);
        EXPECT_FALSE(core.query(id)->net_profit_minor);
        EXPECT_EQ(core.account().available_minor, 100000);
        core.shutdown();
        EXPECT_EQ(callbacks, 1);
        invariant(core);
    }
}

TEST(SimulatorCoreTest, MissingOrStaleExpiryNeverFabricatesLossOrReleasesOpenedRisk) {
    for (const auto& ticks : std::vector<std::vector<SimulatorTick>>{
            {{"EURUSD", 0, 100}, {"EURUSD", 60001, 99}},
            {{"EURUSD", 0, 100}, {"EURUSD", 59899, 99}}}) {
        auto capped = policy();
        capped.defaults.max_active_global = 1;
        SimulatorCore core(account(), {0, 500, 100, 10}, snapshot(capped), quotes(ticks));
        int callbacks = 0;
        const auto id = *core.enqueue(order(), [&](const auto& r) {
            ++callbacks;
            EXPECT_EQ(r.phase, SimulatorTradePhase::UNRESOLVED);
            EXPECT_FALSE(r.net_profit_minor);
        });
        core.advance_to(60000);
        EXPECT_EQ(core.query(id)->phase, SimulatorTradePhase::UNRESOLVED);
        EXPECT_FALSE(core.query(id)->settlement_ms);
        EXPECT_FALSE(core.query(id)->close_quote);
        EXPECT_EQ(core.account().balance_minor, 100000);
        EXPECT_EQ(core.account().reserved_minor, 0);
        EXPECT_EQ(core.account().exposure_minor, 1000);
        EXPECT_EQ(core.account().open_trades, 1u);
        core.advance_to(60500);
        const auto blocked = *core.enqueue(order(OrderType::SELL));
        core.process();
        EXPECT_EQ(core.query(blocked)->error, TradeErrorCode::LIMIT_OPEN_TRADES);
        core.shutdown();
        core.shutdown();
        EXPECT_EQ(callbacks, 1);
        EXPECT_EQ(core.account().exposure_minor, 1000);
        EXPECT_EQ(core.account().available_minor, 99000);
        invariant(core);
    }
}

TEST(SimulatorCoreTest, FixtureOutOfOrderEqualTimestampsAndFreshnessBoundaryAreDeterministic) {
    const auto fixture = quotes({{"EURUSD", 60001, 1}, {"EURUSD", 60000, 99},
        {"EURUSD", 0, 100}, {"EURUSD", 60000, 102}, {"GBPUSD", 0, 1}});
    EXPECT_FALSE(fixture.latest("ABSENT", 999999));
    EXPECT_EQ(fixture.latest("EURUSD", 60000)->price_units, 102);
    EXPECT_EQ(fixture.latest("EURUSD", 60000)->observation_id, 4u);
    SimulatorCore equal_time(account(), {}, snapshot(), fixture);
    const auto equal_id = *equal_time.enqueue(order());
    equal_time.advance_to(100000);
    EXPECT_EQ(equal_time.query(equal_id)->outcome, TradeState::WIN);
    EXPECT_EQ(equal_time.query(equal_id)->close_quote->observation_id, 4u);
    SimulatorCore core(account(), {0, 0, 100, 10}, snapshot(),
        quotes({{"EURUSD", 59900, 101}, {"EURUSD", 0, 100}, {"EURUSD", 60001, 1}}));
    const auto id = *core.enqueue(order());
    core.advance_to(100000);
    EXPECT_EQ(core.query(id)->outcome, TradeState::WIN);
    EXPECT_EQ(core.query(id)->close_quote->timestamp_ms, 59900);
}

TEST(SimulatorCoreTest, HalfUpRoundingAndOverflowCheckedWithoutWideIntegerExtension) {
    EXPECT_EQ(SimulatorCore::win_profit(1, 499999), 0);
    EXPECT_EQ(SimulatorCore::win_profit(1, 500000), 1);
    EXPECT_EQ(SimulatorCore::win_profit(3, 500000), 2);
    EXPECT_EQ(SimulatorCore::win_profit(101, 750000), 76);
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    EXPECT_EQ(SimulatorCore::win_profit(maximum, 1000000), maximum);
    EXPECT_EQ(SimulatorCore::win_profit(1, maximum), maximum / 1000000 + 1);
    EXPECT_FALSE(SimulatorCore::win_profit(maximum, maximum));
    EXPECT_FALSE(SimulatorCore::win_profit(maximum, 1000001));
    EXPECT_FALSE(SimulatorCore::win_profit(0, 1));
    EXPECT_FALSE(SimulatorCore::win_profit(1, -1));
    for (std::int64_t stake = 1; stake <= 1000000; stake += 7919) {
        for (std::int64_t payout = 0; payout <= 2000000; payout += 65537) {
            EXPECT_EQ(SimulatorCore::win_profit(stake, payout),
                      (stake * payout + 500000) / 1000000);
        }
    }
    SimulatorCore core(account(), {}, snapshot(), quotes());
    const auto id = *core.enqueue(order(OrderType::BUY, 101));
    core.advance_to(60000);
    EXPECT_EQ(core.query(id)->net_profit_minor, 76);
    EXPECT_EQ(core.account().balance_minor, 100076);
}

TEST(SimulatorCoreTest, MonetaryOverflowRejectsOpenAndReleasesReserve) {
    auto value = policy();
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    value.defaults.max_stake_minor = maximum;
    value.defaults.payout_ppm = maximum;
    SimulatorCore core(account(maximum), {}, snapshot(value), quotes());
    const auto id = *core.enqueue(order(OrderType::BUY, maximum));
    core.process();
    EXPECT_EQ(core.query(id)->phase, SimulatorTradePhase::FAILED_OPEN);
    EXPECT_EQ(core.account().available_minor, maximum);
    EXPECT_EQ(core.account().reserved_minor, 0);
    invariant(core);
}

TEST(SimulatorCoreTest, ConcurrentPossibleWinsReserveBalanceHeadroomBeforeOpening) {
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    SimulatorCore core(account(maximum - 1000), {}, snapshot(), quotes());
    const auto a = *core.enqueue(order());
    const auto b = *core.enqueue(order());
    core.process();
    EXPECT_EQ(core.query(a)->phase, SimulatorTradePhase::OPEN);
    EXPECT_EQ(core.query(b)->phase, SimulatorTradePhase::FAILED_OPEN);
    core.advance_to(60000);
    EXPECT_EQ(core.account().balance_minor, maximum - 250);
    invariant(core);
}

TEST(SimulatorCoreTest, TimestampOverflowChecksDelayDurationAndResponseBeforeReservation) {
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    for (const auto execution : {SimulatorExecutionConfig{500, 0, 0, 10},
                                SimulatorExecutionConfig{0, 0, 0, 10},
                                SimulatorExecutionConfig{0, maximum, 0, 10}}) {
        SimulatorCore core(account(), execution, snapshot(), quotes(), maximum - 100);
        const auto id = *core.enqueue(order());
        core.process();
        EXPECT_EQ(core.query(id)->error, TradeErrorCode::INVALID_EXPIRY_TIME);
        EXPECT_TRUE(core.query(id)->virtual_trade_id.empty());
        EXPECT_EQ(core.account().reserved_minor, 0);
        invariant(core);
    }
}

TEST(SimulatorCoreTest, ShutdownQueuedPendingOpenedAndNotificationKeepsDistinctEvidence) {
    for (const auto time : {-1, 0, 150, 60150}) {
        SimulatorCore core(account(), {150, 500, 0, 10}, snapshot(),
            quotes({{"EURUSD", 150, 100}, {"EURUSD", 60150, 101}}));
        int callbacks = 0;
        const auto id = *core.enqueue(order(), [&](const auto&) { ++callbacks; });
        if (time >= 0) core.advance_to(time);
        core.shutdown();
        core.shutdown();
        const auto r = *core.query(id);
        if (time < 150) {
            EXPECT_EQ(r.phase, SimulatorTradePhase::CANCELED);
            EXPECT_FALSE(r.open_ms);
            EXPECT_FALSE(r.net_profit_minor);
            EXPECT_EQ(core.account().exposure_minor, 0);
            EXPECT_EQ(core.account().balance_minor, 100000);
        } else if (time == 150) {
            EXPECT_EQ(r.phase, SimulatorTradePhase::UNRESOLVED);
            EXPECT_EQ(r.unresolved_reason, SimulatorUnresolvedReason::SHUTDOWN);
            EXPECT_EQ(core.account().exposure_minor, 1000);
            EXPECT_FALSE(r.net_profit_minor);
        } else {
            EXPECT_EQ(r.phase, SimulatorTradePhase::SETTLED);
            EXPECT_EQ(r.net_profit_minor, 750);
            EXPECT_EQ(core.account().balance_minor, 100750);
        }
        EXPECT_EQ(core.account().reserved_minor, 0);
        EXPECT_EQ(core.account().pending_trades, 0u);
        EXPECT_EQ(callbacks, 1);
        EXPECT_FALSE(core.enqueue(order()));
        EXPECT_FALSE(core.process());
        invariant(core);
    }
}

TEST(SimulatorCoreTest, BoundedCommandsHistoryStableIdentityAndRepeatability) {
    std::vector<std::vector<SimulatorTrade>> runs;
    for (int run = 0; run < 2; ++run) {
        SimulatorCore core(account(), {0, 0, 0, 2}, snapshot(), quotes());
        std::vector<std::uint64_t> delivery;
        for (int i = 0; i < 2; ++i) ASSERT_TRUE(core.enqueue(order(), [&](const auto& r) { delivery.push_back(r.command_id); }));
        EXPECT_FALSE(core.enqueue(order()));
        core.advance_to(100000);
        EXPECT_EQ(delivery, (std::vector<std::uint64_t>{1, 2}));
        const auto a = *core.query(1);
        const auto b = *core.query(2);
        EXPECT_EQ(a.virtual_trade_id, "sim/paper-test/1");
        EXPECT_NE(a.virtual_trade_id, b.virtual_trade_id);
        EXPECT_EQ(core.query(a.virtual_trade_id)->net_profit_minor, 750);
        EXPECT_FALSE(core.query(0));
        EXPECT_FALSE(core.query(3));
        EXPECT_FALSE(core.query(std::string{}));
        EXPECT_FALSE(core.query("real-broker-1"));
        core.shutdown();
        runs.push_back(core.history());
    }
    for (std::size_t i = 0; i < runs[0].size(); ++i) {
        EXPECT_EQ(runs[0][i].virtual_trade_id, runs[1][i].virtual_trade_id);
        EXPECT_EQ(runs[0][i].outcome, runs[1][i].outcome);
        EXPECT_EQ(runs[0][i].open_ms, runs[1][i].open_ms);
        EXPECT_EQ(runs[0][i].settlement_ms, runs[1][i].settlement_ms);
        EXPECT_EQ(runs[0][i].callback_ms, runs[1][i].callback_ms);
        EXPECT_EQ(runs[0][i].net_profit_minor, runs[1][i].net_profit_minor);
    }
}

TEST(SimulatorCoreTest, CallbackExceptionsReentrancyAndClosureRelease) {
    SimulatorCore core(account(), {}, snapshot(), quotes());
    auto owned = std::make_shared<int>(1);
    const std::weak_ptr<int> weak = owned;
    core.enqueue(order(), [owned, &core](const auto&) {
        EXPECT_FALSE(core.process());
        EXPECT_TRUE(core.enqueue(order()));
        throw std::runtime_error("consumer callback");
    });
    owned.reset();
    core.advance_to(60000);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(core.callback_errors(), 1u);
    EXPECT_EQ(core.history().size(), 2u);
    core.shutdown();
    EXPECT_EQ(core.account().exposure_minor, 1000);
}

TEST(SimulatorCoreTest, CallbackMayRequestShutdownWithoutRecursiveDispatch) {
    SimulatorCore core(account(), {}, snapshot(), quotes());
    core.enqueue(order(), [&](const auto&) { core.shutdown(); });
    core.advance_to(100000);
    EXPECT_TRUE(core.is_stopped());
    EXPECT_EQ(core.now_ms(), 60000);
}

TEST(SimulatorCoreTest, RejectsInvalidConfigurationAndOversizedFixtureOrCommand) {
    EXPECT_THROW(SimulatorQuoteFixture({{"EURUSD", -1, 100}}), std::invalid_argument);
    EXPECT_THROW(SimulatorQuoteFixture({{"EURUSD", 0, 0}}), std::invalid_argument);
    EXPECT_THROW(SimulatorQuoteFixture({{"EURUSD", 0, 1}, {"EURUSD", 1, 2}}, 1), std::invalid_argument);
    EXPECT_THROW(SimulatorCore(account(-1), {}, snapshot(), quotes()), std::invalid_argument);
    EXPECT_THROW(SimulatorCore(account(), {-1, 0, 0, 1}, snapshot(), quotes()), std::invalid_argument);
    EXPECT_THROW(SimulatorCore(account(), {0, 0, 0, 0}, snapshot(), quotes()), std::invalid_argument);
    auto invalid = account();
    invalid.currency = static_cast<CurrencyType>(999);
    EXPECT_THROW(SimulatorCore(invalid, {}, snapshot(), quotes()), std::invalid_argument);
    SimulatorCore core(account(), {}, snapshot(), quotes());
    EXPECT_FALSE(core.enqueue(order(OrderType::BUY, 100, 60, std::string(65, 'X'))));
    EXPECT_TRUE(core.history().empty());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
