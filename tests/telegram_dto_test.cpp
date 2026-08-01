#include <gtest/gtest.h>

#include "optionx_cpp/bridges/telegram.hpp"

namespace {

optionx::bridges::telegram::TelegramRawMessage make_message() {
    optionx::bridges::telegram::TelegramRawMessage message;
    message.chat_id = "-10042";
    message.chat_title = "Signals";
    message.topic_id = "7";
    message.message_id = 123;
    message.date_ms = 1800000000000;
    message.edit_date_ms = 1800000001000;
    message.sender_id = "99";
    message.reply_to_message_id = 120;
    message.grouped_id = "album-1";
    message.text = "EURUSD BUY 5m";
    message.media = nlohmann::json::array({nlohmann::json{{"kind", "photo"}}});
    return message;
}

} // namespace

TEST(TelegramRawMessage, BuildsStableMessageRevisionAndReplyIdentities) {
    const auto message = make_message();

    EXPECT_EQ(message.message_identity(), "telegram:-10042:7:123");
    EXPECT_EQ(message.revision_identity(), "telegram:-10042:7:123:1800000001000");
    EXPECT_EQ(message.reply_to_message_identity(), "telegram:-10042:7:120");
}

TEST(TelegramRawMessage, RoundTripsWorkerPayload) {
    const auto original = make_message();
    const auto restored =
        optionx::bridges::telegram::TelegramRawMessage::from_json(original.to_json());

    EXPECT_EQ(restored.chat_id, original.chat_id);
    EXPECT_EQ(restored.message_id, original.message_id);
    EXPECT_EQ(restored.edit_date_ms, original.edit_date_ms);
    EXPECT_EQ(restored.reply_to_message_id, original.reply_to_message_id);
    EXPECT_EQ(restored.media, original.media);
}

TEST(TelegramRawMessage, RejectsInvalidIdentityAndMedia) {
    auto invalid = make_message();
    invalid.message_id = 0;
    EXPECT_THROW(invalid.validate(), std::invalid_argument);

    invalid = make_message();
    invalid.media = nlohmann::json::object();
    EXPECT_THROW(invalid.validate(), std::invalid_argument);
}

TEST(TelegramParsedMessage, KeepsSignalsOutcomesAndDiagnosticsSeparate) {
    optionx::bridges::telegram::TelegramParsedMessage parsed;
    parsed.raw = make_message();
    parsed.signals.push_back({
        parsed.raw.message_identity(),
        "EURUSD",
        optionx::OrderType::BUY,
        optionx::OptionType::SPRINT,
        300,
        0,
        "morning",
        parsed.raw.text,
    });
    parsed.outcomes.push_back({
        parsed.raw.message_identity(),
        parsed.raw.reply_to_message_identity(),
        "EURUSD",
        optionx::OrderType::BUY,
        optionx::bridges::telegram::TelegramOutcomeResult::WIN,
        0,
        "morning",
        "WIN",
    });
    parsed.diagnostics.push_back({
        "unmatched_expiry",
        "expiry was not recognized",
        12,
        2,
    });

    ASSERT_EQ(parsed.signals.size(), 1u);
    ASSERT_EQ(parsed.outcomes.size(), 1u);
    ASSERT_EQ(parsed.diagnostics.size(), 1u);
    EXPECT_EQ(parsed.signals.front().duration, 300u);
    EXPECT_EQ(parsed.outcomes.front().result,
              optionx::bridges::telegram::TelegramOutcomeResult::WIN);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
