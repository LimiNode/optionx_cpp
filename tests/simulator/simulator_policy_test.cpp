#include <optionx_cpp/simulator_policy.hpp>

#include <gtest/gtest.h>

using namespace optionx;
using namespace optionx::platforms;

namespace {

SimulatorPolicy example_policy() {
    SimulatorPolicy policy;
    policy.defaults.available = true;
    policy.defaults.min_stake_minor = 100;
    policy.defaults.max_stake_minor = 10000;
    policy.defaults.max_active_global = 10;
    policy.symbols["EURUSD"].payout_ppm = 750000;
    policy.symbols["EURUSD"].max_active_symbol = 3;
    policy.symbols["GBPUSD"].payout_ppm = 800000;
    policy.excluded_expiries.insert(120);

    SimulatorRule rule;
    rule.id = "close-window";
    rule.priority = 10;
    rule.selector.symbol = "EURUSD";
    rule.selector.direction = OrderType::BUY;
    rule.selector.utc_window = SimulatorTimeWindow{64500, 65100};
    rule.overrides.payout_ppm = 500000;
    rule.overrides.max_stake_minor = 2000;
    policy.rules.push_back(rule);
    return policy;
}

SimulatorPolicyRequest request(const std::string& symbol, OrderType direction,
                               std::uint32_t second, std::uint32_t duration = 60) {
    return {symbol, direction, second, duration};
}

} // namespace

TEST(SimulatorPolicyTest, ExampleBoundaryTableAndPartialOverrides) {
    const auto policy = example_policy();
    ASSERT_EQ(policy.validate(), SimulatorPolicyError::NONE);

    const auto before = policy.resolve(request("EURUSD", OrderType::BUY, 64499));
    EXPECT_EQ(before.error, SimulatorPolicyError::NONE);
    EXPECT_EQ(before.values.payout_ppm, 750000);
    EXPECT_EQ(before.values.max_stake_minor, 10000);
    EXPECT_TRUE(before.applied_rule_ids.empty());

    const auto start = policy.resolve(request("EURUSD", OrderType::BUY, 64500));
    EXPECT_EQ(start.error, SimulatorPolicyError::NONE);
    EXPECT_EQ(start.values.payout_ppm, 500000);
    EXPECT_EQ(start.values.max_stake_minor, 2000);
    EXPECT_EQ(start.values.min_stake_minor, 100);
    EXPECT_EQ(start.values.max_active_global, 10u);
    EXPECT_EQ(start.values.max_active_symbol, 3u);
    ASSERT_EQ(start.applied_rule_ids.size(), 1u);
    EXPECT_EQ(start.applied_rule_ids.front(), "close-window");

    const auto last = policy.resolve(request("EURUSD", OrderType::BUY, 65099));
    EXPECT_EQ(last.values.payout_ppm, 500000);
    const auto end = policy.resolve(request("EURUSD", OrderType::BUY, 65100));
    EXPECT_EQ(end.values.payout_ppm, 750000);
    const auto sell = policy.resolve(request("EURUSD", OrderType::SELL, 64800));
    EXPECT_EQ(sell.values.payout_ppm, 750000);
    const auto gbp = policy.resolve(request("GBPUSD", OrderType::BUY, 64800));
    EXPECT_EQ(gbp.values.payout_ppm, 800000);
    EXPECT_FALSE(gbp.values.max_active_symbol.has_value());
}

TEST(SimulatorPolicyTest, ExpiryWhitelistAndMalformedRequestFailClosed) {
    const auto policy = example_policy();
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 0, 60)).error,
              SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 0, 30000)).error,
              SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 0, 120)).error,
              SimulatorPolicyError::INVALID_DURATION);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 0, 59)).error,
              SimulatorPolicyError::INVALID_DURATION);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 0, 30001)).error,
              SimulatorPolicyError::INVALID_DURATION);
    EXPECT_EQ(policy.resolve(request("UNKNOWN", OrderType::BUY, 0)).error,
              SimulatorPolicyError::INVALID_SYMBOL);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::UNKNOWN, 0)).error,
              SimulatorPolicyError::INVALID_DIRECTION);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 86400)).error,
              SimulatorPolicyError::INVALID_TIME);
}

TEST(SimulatorPolicyTest, WrappedWindowAndDisjointBoundary) {
    auto policy = example_policy();
    SimulatorRule night;
    night.id = "night";
    night.priority = 20;
    night.selector.utc_window = SimulatorTimeWindow{23 * 3600, 3600};
    night.overrides.available = false;
    policy.rules.push_back(night);
    ASSERT_EQ(policy.validate(), SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 23 * 3600)).error,
              SimulatorPolicyError::UNAVAILABLE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 0)).error,
              SimulatorPolicyError::UNAVAILABLE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 3599)).error,
              SimulatorPolicyError::UNAVAILABLE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 3600)).error,
              SimulatorPolicyError::NONE);
}

TEST(SimulatorPolicyTest, PrioritySpecificityAndDurationThreshold) {
    auto policy = example_policy();
    SimulatorRule global;
    global.id = "global";
    global.priority = 1;
    global.overrides.payout_ppm = 650000;
    policy.rules.push_back(global);
    SimulatorRule duration;
    duration.id = "long-eurusd";
    duration.priority = 11;
    duration.selector.symbol = "EURUSD";
    duration.selector.expiry = SimulatorExpiryRange{300, 30000};
    duration.overrides.payout_ppm = 600000;
    policy.rules.push_back(duration);
    ASSERT_EQ(policy.validate(), SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 64800, 60)).values.payout_ppm,
              500000);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 64800, 300)).values.payout_ppm,
              600000);
    EXPECT_EQ(policy.resolve(request("GBPUSD", OrderType::BUY, 64800, 300)).values.payout_ppm,
              650000);
}

TEST(SimulatorPolicyTest, RejectsAmbiguousWritersButAllowsIndependentFields) {
    auto policy = example_policy();
    SimulatorRule rule = policy.rules.front();
    rule.id = "overlap";
    rule.overrides.max_stake_minor = 1500;
    EXPECT_EQ((policy.rules.push_back(rule), policy.validate()),
              SimulatorPolicyError::AMBIGUOUS_RULES);

    policy.rules.back().overrides = {};
    policy.rules.back().overrides.min_stake_minor = 200;
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 64800)).values.min_stake_minor,
              200);

    policy.rules.back().id = "close-window";
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::DUPLICATE_RULE_ID);
}

TEST(SimulatorPolicyTest, AdjacentWindowsDoNotConflictAndUnknownRuleSymbolFails) {
    auto policy = example_policy();
    SimulatorRule adjacent = policy.rules.front();
    adjacent.id = "after-close";
    adjacent.selector.utc_window = SimulatorTimeWindow{65100, 65700};
    policy.rules.push_back(adjacent);
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 65100)).applied_rule_ids,
              std::vector<std::string>{"after-close"});

    policy.rules.back().selector.symbol = "EURUSX";
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);
}

TEST(SimulatorPolicyTest, GlobalCapIsTimeOnlyAndIdenticalAcrossSymbols) {
    auto policy = example_policy();
    SimulatorRule timed;
    timed.id = "account-evening-cap";
    timed.priority = 20;
    timed.selector.utc_window = SimulatorTimeWindow{64800, 68400};
    timed.overrides.max_active_global = 2;
    policy.rules.push_back(timed);
    ASSERT_EQ(policy.validate(), SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 64800)).values.max_active_global, 2u);
    EXPECT_EQ(policy.resolve(request("GBPUSD", OrderType::SELL, 64800)).values.max_active_global, 2u);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 68400)).values.max_active_global, 10u);

    policy.rules.back().selector.symbol = "EURUSD";
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);
    policy.rules.back().selector.symbol.reset();
    policy.rules.back().selector.direction = OrderType::BUY;
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);
    policy.rules.back().selector.direction.reset();
    policy.rules.back().selector.expiry = SimulatorExpiryRange{60, 300};
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);
    policy.rules.pop_back();
    policy.symbols["EURUSD"].max_active_global = 2;
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);
}

TEST(SimulatorPolicyTest, RejectsCrossRankEffectiveStakeContradiction) {
    auto policy = example_policy();
    SimulatorRule minimum;
    minimum.id = "large-minimum";
    minimum.priority = 1;
    minimum.selector.symbol = "EURUSD";
    minimum.selector.direction = OrderType::SELL;
    minimum.overrides.min_stake_minor = 5000;
    policy.rules.push_back(minimum);

    SimulatorRule maximum;
    maximum.id = "small-maximum";
    maximum.priority = 2;
    maximum.selector.expiry = SimulatorExpiryRange{60, 120};
    maximum.overrides.max_stake_minor = 2000;
    policy.rules.push_back(maximum);
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 0, 60)).error,
              SimulatorPolicyError::INVALID_CONFIG);

    policy.rules.back().selector.symbol = "GBPUSD";
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::NONE);
}

TEST(SimulatorPolicyTest, DayEndAndMidnightWindowsUseHalfOpenBounds) {
    auto policy = example_policy();
    SimulatorRule full_day;
    full_day.id = "full-day";
    full_day.selector.utc_window = SimulatorTimeWindow{0, 86400};
    full_day.overrides.payout_ppm = 700000;
    policy.rules.push_back(full_day);
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("GBPUSD", OrderType::SELL, 0)).values.payout_ppm, 700000);
    EXPECT_EQ(policy.resolve(request("GBPUSD", OrderType::SELL, 86399)).values.payout_ppm, 700000);

    policy.rules.back().selector.utc_window = SimulatorTimeWindow{86399, 86400};
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("GBPUSD", OrderType::SELL, 86398)).values.payout_ppm, 800000);
    EXPECT_EQ(policy.resolve(request("GBPUSD", OrderType::SELL, 86399)).values.payout_ppm, 700000);

    policy.rules.back().selector.utc_window = SimulatorTimeWindow{86399, 0};
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("GBPUSD", OrderType::SELL, 86399)).values.payout_ppm, 700000);
    EXPECT_EQ(policy.resolve(request("GBPUSD", OrderType::SELL, 0)).values.payout_ppm, 800000);
    policy.rules.back().selector.utc_window = SimulatorTimeWindow{86400, 0};
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);
    policy.rules.back().selector.utc_window = SimulatorTimeWindow{0, 0};
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);
}

TEST(SimulatorPolicyTest, RuleOrderDoesNotChangeResolutionAndInvalidEnumIsRejected) {
    auto policy = example_policy();
    SimulatorRule payout;
    payout.id = "payout-only";
    payout.priority = 20;
    payout.selector.symbol = "EURUSD";
    payout.selector.utc_window = SimulatorTimeWindow{64800, 65000};
    payout.overrides.payout_ppm = 900000;
    policy.rules.push_back(payout);
    const auto first = policy.resolve(request("EURUSD", OrderType::BUY, 64800));
    ASSERT_EQ(first.error, SimulatorPolicyError::NONE);
    EXPECT_EQ(first.values.max_active_global, 10u);
    EXPECT_EQ(first.values.max_active_symbol, 3u);
    EXPECT_EQ(first.values.max_stake_minor, 2000);
    std::reverse(policy.rules.begin(), policy.rules.end());
    const auto reversed = policy.resolve(request("EURUSD", OrderType::BUY, 64800));
    EXPECT_EQ(reversed.error, SimulatorPolicyError::NONE);
    EXPECT_EQ(reversed.values.payout_ppm, first.values.payout_ppm);
    EXPECT_EQ(reversed.applied_rule_ids, first.applied_rule_ids);

    const auto invalid = static_cast<OrderType>(999);
    EXPECT_EQ(policy.resolve(request("EURUSD", invalid, 64800)).error,
              SimulatorPolicyError::INVALID_DIRECTION);
    policy.rules.back().selector.direction = invalid;
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);
}

TEST(SimulatorPolicyTest, RejectsInvalidConfigurationAndKeepsEnumDirection) {
    auto policy = example_policy();
    policy.rules.front().selector.utc_window = SimulatorTimeWindow{10, 10};
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);
    policy = example_policy();
    policy.symbols["EURUSD"].max_stake_minor = 50;
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::INVALID_CONFIG);

    // The legacy CALL/PUT string mode disagrees with TradeStateManager's BUY/SELL direction.
    EXPECT_EQ(to_str(OrderType::BUY, 2), "PUT");
    EXPECT_EQ(to_str(OrderType::SELL, 2), "CALL");
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 64800)).error,
              SimulatorPolicyError::INVALID_CONFIG);
}

TEST(SimulatorPolicySnapshotTest, MatchesConservativeResolverIncludingInvalidRequests) {
    const auto policy = example_policy();
    const auto snapshot = SimulatorPolicySnapshot::create(policy);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->version(), policy.version);
    for (const auto& symbol : {"EURUSD", "GBPUSD", "MISSING"}) {
        for (const auto direction : {OrderType::BUY, OrderType::SELL, OrderType::UNKNOWN}) {
            for (const auto time : {0u, 64499u, 64500u, 65099u, 65100u, 86399u, 86400u}) {
                for (const auto duration : {59u, 60u, 120u, 300u, 30000u, 30001u}) {
                    const auto facts = request(symbol, direction, time, duration);
                    const auto a = policy.resolve(facts);
                    const auto b = snapshot->resolve(facts);
                    EXPECT_EQ(a.error, b.error);
                    EXPECT_EQ(a.values.available, b.values.available);
                    EXPECT_EQ(a.values.payout_ppm, b.values.payout_ppm);
                    EXPECT_EQ(a.values.min_stake_minor, b.values.min_stake_minor);
                    EXPECT_EQ(a.values.max_stake_minor, b.values.max_stake_minor);
                    EXPECT_EQ(a.values.max_active_global, b.values.max_active_global);
                    EXPECT_EQ(a.values.max_active_symbol, b.values.max_active_symbol);
                    EXPECT_EQ(a.applied_rule_ids, b.applied_rule_ids);
                }
            }
        }
    }
}

TEST(SimulatorPolicySnapshotTest, OwnsCopyAndRejectsInvalidUpdates) {
    auto policy = example_policy();
    const auto snapshot = SimulatorPolicySnapshot::create(policy);
    ASSERT_TRUE(snapshot);
    policy.version = 2;
    policy.symbols["EURUSD"].payout_ppm = -1;
    SimulatorPolicyError error = SimulatorPolicyError::NONE;
    EXPECT_FALSE(SimulatorPolicySnapshot::create(policy, &error));
    EXPECT_EQ(error, SimulatorPolicyError::INVALID_CONFIG);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 0)).error, error);
    EXPECT_EQ(snapshot->resolve(request("EURUSD", OrderType::BUY, 0)).values.payout_ppm, 750000);
    EXPECT_EQ(snapshot->version(), 1u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
