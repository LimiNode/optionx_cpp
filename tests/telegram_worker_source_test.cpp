#include <gtest/gtest.h>

#include "optionx_cpp/bridges/telegram.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

class FakeWorkerClient {
public:
    using handler_t = std::function<void(const nlohmann::json&)>;

    bool start_listening(
            const std::vector<std::string>& chats,
            handler_t handler,
            const std::vector<std::string>& topics) {
        selected_chats = chats;
        selected_topics = topics;
        m_handler = std::move(handler);
        return true;
    }

    bool stop_listening() {
        stopped = true;
        m_handler = {};
        return true;
    }

    void emit_message() {
        m_handler(nlohmann::json{
            {"message_type", "event"},
            {"operation", "message.received"},
            {"request_id", 0},
            {"payload", {
                {"message", {
                    {"chat_id", "-10042"},
                    {"chat_title", "Signals"},
                    {"topic_id", "7"},
                    {"message_id", 12},
                    {"date_ms", 1800000000000LL},
                    {"text", "EURUSD BUY 5m"},
                    {"media", nlohmann::json::array()},
                }}
            }}
        });
    }

    void emit_error() {
        m_handler(nlohmann::json{
            {"message_type", "error"},
            {"request_id", 0},
            {"payload", {{"message", "listener failed"}}},
        });
    }

    std::vector<std::string> selected_chats;
    std::vector<std::string> selected_topics;
    bool stopped = false;

private:
    handler_t m_handler;
};

} // namespace

TEST(TelegramWorkerMessageSource, AdaptsLiveRecordsAndErrors) {
    FakeWorkerClient worker;
    optionx::bridges::telegram::TelegramWorkerSourceConfig config;
    config.chats = {"-10042"};
    config.topic_ids = {"7"};
    optionx::bridges::telegram::TelegramWorkerMessageSource source(worker, config);

    std::vector<optionx::bridges::telegram::TelegramRawMessage> messages;
    std::vector<std::string> errors;
    ASSERT_TRUE(source.start(
        [&](const auto& message) { messages.push_back(message); },
        [&](const auto& error) { errors.push_back(error); }));
    worker.emit_message();
    worker.emit_error();

    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages.front().message_identity(), "telegram:-10042:7:12");
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors.front(), "listener failed");
    EXPECT_EQ(worker.selected_chats, config.chats);
    EXPECT_EQ(worker.selected_topics, config.topic_ids);

    source.stop();
    EXPECT_TRUE(worker.stopped);
}

TEST(TelegramWorkerMessageSource, RejectsEmptyChatSelection) {
    FakeWorkerClient worker;
    optionx::bridges::telegram::TelegramWorkerMessageSource source(
        worker, {});
    EXPECT_FALSE(source.start([](const auto&) {}, [](const auto&) {}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
