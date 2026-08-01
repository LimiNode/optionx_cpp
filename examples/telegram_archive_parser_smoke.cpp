/// \file telegram_archive_parser_smoke.cpp
/// \brief Demonstrates replaying exported Telegram messages through the parser.

#include <optionx_cpp/bridges/telegram.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

optionx::bridges::telegram::TelegramRawMessage make_message(
        std::int64_t message_id,
        std::string text,
        std::int64_t reply_to_message_id = 0);

std::vector<optionx::bridges::telegram::TelegramRawMessage> demo_archive();

void print_parsed(
        const optionx::bridges::telegram::TelegramParsedMessage& parsed);

} // namespace

int main() {
    // In production these records arrive one at a time from
    // tg-client-stdio::WorkerClient::stream_messages(). Keeping the replay
    // loop source-independent makes parser tests and backtests deterministic.
    optionx::bridges::telegram::TelegramSignalParser parser;
    for (const auto& raw : demo_archive()) {
        print_parsed(parser.parse(raw));
    }
    return 0;
}

namespace {

optionx::bridges::telegram::TelegramRawMessage make_message(
        const std::int64_t message_id,
        std::string text,
        const std::int64_t reply_to_message_id) {
    optionx::bridges::telegram::TelegramRawMessage message;
    message.chat_id = "-1001234567890";
    message.chat_title = "Demo archive";
    message.message_id = message_id;
    message.date_ms = 1800000000000 + message_id * 1000;
    message.reply_to_message_id = reply_to_message_id;
    message.text = std::move(text);
    return message;
}

std::vector<optionx::bridges::telegram::TelegramRawMessage> demo_archive() {
    return {
        make_message(100, "EURUSD BUY 5m"),
        make_message(101, "EURUSD WIN", 100),
        make_message(102, "USDJPY BUY 2 weeks"),
    };
}

void print_parsed(
        const optionx::bridges::telegram::TelegramParsedMessage& parsed) {
    std::cout << "message=" << parsed.raw.message_identity()
              << " signals=" << parsed.signals.size()
              << " outcomes=" << parsed.outcomes.size()
              << " diagnostics=" << parsed.diagnostics.size() << '\n';
    for (const auto& signal : parsed.signals) {
        std::cout << "  signal=" << signal.symbol
                  << " direction=" << optionx::to_str(signal.order_type)
                  << " duration=" << signal.duration << '\n';
    }
    for (const auto& outcome : parsed.outcomes) {
        std::cout << "  outcome=" << outcome.symbol
                  << " result=" << static_cast<int>(outcome.result) << '\n';
    }
    for (const auto& diagnostic : parsed.diagnostics) {
        std::cout << "  diagnostic=" << diagnostic.code << '\n';
    }
}

} // namespace
