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
    policy.rules.back().overrides.max_active_symbol = 2;
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::NONE);
    EXPECT_EQ(policy.resolve(request("EURUSD", OrderType::BUY, 64800)).values.max_active_symbol,
              2u);

    policy.rules.back().id = "close-window";
    EXPECT_EQ(policy.validate(), SimulatorPolicyError::DUPLICATE_RULE_ID);
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
