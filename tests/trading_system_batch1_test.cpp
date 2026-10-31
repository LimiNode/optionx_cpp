#include <gtest/gtest.h>

#include <optionx_cpp/trading_system.hpp>

namespace {

optionx::trading_system::TradeIntent make_intent() {
    optionx::trading_system::TradeIntent intent;
    intent.intent_id = 41;
    intent.origin_signal_id = 7;
    intent.execution_group.expected_members = 1;
    intent.cardinality = optionx::trading_system::ExecutionCardinality::EXACTLY_ONE;
    intent.idempotency_key = "intent-41";
    intent.request.symbol = "EURUSD";
    intent.request.signal_id = 7;
    intent.request.amount = 10.0;
    return intent;
}

TEST(TradingSystemBatch1, IntentCloneCopiesIdentityAndRequest) {
    const auto original = make_intent();
    auto clone = original.clone();

    EXPECT_EQ(clone.intent_id, original.intent_id);
    EXPECT_EQ(clone.idempotency_key, original.idempotency_key);
    EXPECT_EQ(clone.request.symbol, original.request.symbol);
    EXPECT_DOUBLE_EQ(clone.request.amount, original.request.amount);
}

TEST(TradingSystemBatch1, IntentRoundTripsThroughJson) {
    const auto original = make_intent();
    const auto encoded = nlohmann::json(original);
    const auto restored = encoded.get<optionx::trading_system::TradeIntent>();

    EXPECT_TRUE(restored.valid());
    EXPECT_EQ(restored.intent_id, original.intent_id);
    EXPECT_EQ(restored.cardinality, original.cardinality);
    EXPECT_EQ(restored.request.signal_id, original.request.signal_id);
    EXPECT_EQ(restored.request.symbol, original.request.symbol);
}

TEST(TradingSystemBatch1, MissingNewFieldsUseStableDefaults) {
    const nlohmann::json legacy = {
        {"intent_id", 9},
        {"idempotency_key", "legacy-9"}
    };

    const auto restored = legacy.get<optionx::trading_system::TradeIntent>();

    EXPECT_TRUE(restored.valid());
    EXPECT_EQ(restored.series_step, -1);
    EXPECT_EQ(restored.status, optionx::trading_system::IntentStatus::PROPOSED);
}

TEST(TradingSystemBatch1, InvalidIdentityIsRejected) {
    auto intent = make_intent();
    intent.intent_id = 0;
    EXPECT_FALSE(intent.valid());

    intent = make_intent();
    intent.idempotency_key.clear();
    EXPECT_FALSE(intent.valid());

    optionx::trading_system::TradeSeries series;
    EXPECT_FALSE(series.valid());
}

TEST(TradingSystemBatch1, SeriesBindingRequiresMatchingStep) {
    auto intent = make_intent();
    intent.series_id = 12;
    intent.series_step = -1;
    EXPECT_FALSE(intent.valid());

    intent.series_step = 0;
    EXPECT_TRUE(intent.valid());
}

TEST(TradingSystemBatch1, RevisionMustMatchBeforeAdvance) {
    auto intent = make_intent();
    EXPECT_TRUE(intent.revision_matches(0));
    EXPECT_TRUE(intent.advance_revision(0));
    EXPECT_EQ(intent.state_revision, 1U);
    EXPECT_FALSE(intent.advance_revision(0));

    optionx::trading_system::TradeSeries series;
    series.series_id = 3;
    EXPECT_TRUE(series.advance_revision(0));
    EXPECT_FALSE(series.advance_revision(0));
}

TEST(TradingSystemBatch1, CardinalityPoliciesValidateIdentityInvariants) {
    using optionx::trading_system::ExecutionCardinality;
    using optionx::trading_system::ExecutionGroupIdentity;

    ExecutionGroupIdentity zero_or_one;
    EXPECT_TRUE(zero_or_one.valid_for(ExecutionCardinality::ZERO_OR_ONE));

    ExecutionGroupIdentity exactly_one;
    exactly_one.expected_members = 1;
    EXPECT_TRUE(exactly_one.valid_for(ExecutionCardinality::EXACTLY_ONE));

    ExecutionGroupIdentity multiple;
    multiple.group_id = 100;
    multiple.expected_members = 2;
    EXPECT_TRUE(multiple.valid_for(ExecutionCardinality::MULTIPLE));

    ExecutionGroupIdentity split;
    split.group_id = 101;
    split.expected_members = 3;
    EXPECT_TRUE(split.valid_for(ExecutionCardinality::SPLIT_GROUP));

    EXPECT_FALSE(zero_or_one.valid_for(ExecutionCardinality::EXACTLY_ONE));
    EXPECT_FALSE(exactly_one.valid_for(ExecutionCardinality::MULTIPLE));
    split.expected_members = 1;
    EXPECT_FALSE(split.valid_for(ExecutionCardinality::SPLIT_GROUP));
}

TEST(TradingSystemBatch1, ExistingRequestJsonContractRemainsAvailable) {
    optionx::TradeRequest request;
    request.symbol = "EURUSD";
    request.signal_id = 7;
    request.amount = 10.0;

    const auto encoded = nlohmann::json(request);
    EXPECT_EQ(encoded.at("symbol"), "EURUSD");
    EXPECT_EQ(encoded.at("signal_id"), 7);
    EXPECT_DOUBLE_EQ(encoded.at("amount").get<double>(), 10.0);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
