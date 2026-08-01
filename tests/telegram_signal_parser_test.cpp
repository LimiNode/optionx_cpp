#include <gtest/gtest.h>

#include "optionx_cpp/bridges/telegram.hpp"

namespace {

optionx::bridges::telegram::TelegramRawMessage message_with_text(std::string text) {
    optionx::bridges::telegram::TelegramRawMessage message;
    message.chat_id = "-10042";
    message.chat_title = "Morning signals";
    message.message_id = 123;
    message.date_ms = 1800000000000;
    message.text = std::move(text);
    return message;
}

} // namespace

TEST(TelegramSignalParser, ParsesExecutableSignalAndNormalizesExpiry) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("EURUSD BUY 5m"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_EQ(parsed.signals[0].symbol, "EURUSD");
    EXPECT_EQ(parsed.signals[0].order_type, optionx::OrderType::BUY);
    EXPECT_EQ(parsed.signals[0].option_type, optionx::OptionType::SPRINT);
    EXPECT_EQ(parsed.signals[0].duration, 300u);
    EXPECT_EQ(parsed.signals[0].source_message_identity, "telegram:-10042:0:123");
    EXPECT_TRUE(parsed.outcomes.empty());
}

TEST(TelegramSignalParser, ParsesMultipleSignalsAndOutcomeSeparately) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("EURUSD BUY 5m\nGBPUSD SELL 10m\nEURUSD WIN"));

    ASSERT_EQ(parsed.signals.size(), 2u);
    EXPECT_EQ(parsed.signals[0].symbol, "EURUSD");
    EXPECT_EQ(parsed.signals[1].symbol, "GBPUSD");
    ASSERT_EQ(parsed.outcomes.size(), 1u);
    EXPECT_EQ(parsed.outcomes[0].symbol, "EURUSD");
    EXPECT_EQ(parsed.outcomes[0].result,
              optionx::bridges::telegram::TelegramOutcomeResult::WIN);
}

TEST(TelegramSignalParser, RejectsUnsupportedExpiryUnitFailClosed) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("USDJPY BUY 2 weeks"));

    EXPECT_TRUE(parsed.signals.empty());
    ASSERT_EQ(parsed.diagnostics.size(), 1u);
    EXPECT_EQ(parsed.diagnostics.front().code, "unmatched_expiry");
}

TEST(TelegramSignalParser, RejectsConflictingOverlappingRules) {
    auto config = optionx::bridges::telegram::TelegramParserConfig{};
    config.signal_rules = {
        {"sprint", R"((EURUSD)\s+(BUY))", 1, 2, 0, 0, optionx::OptionType::SPRINT},
        {"classic", R"((EURUSD)\s+(BUY))", 1, 2, 0, 0, optionx::OptionType::CLASSIC},
    };
    optionx::bridges::telegram::TelegramSignalParser parser(std::move(config));
    const auto parsed = parser.parse(message_with_text("EURUSD BUY"));

    EXPECT_TRUE(parsed.signals.empty());
    ASSERT_EQ(parsed.diagnostics.size(), 1u);
    EXPECT_EQ(parsed.diagnostics.front().code, "ambiguous_overlapping_signal");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
