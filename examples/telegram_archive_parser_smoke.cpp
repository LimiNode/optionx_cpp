/// \file telegram_archive_parser_smoke.cpp
/// \brief Demonstrates replaying exported Telegram messages through the parser.

#include <optionx_cpp/bridges/telegram.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

optionx::bridges::telegram::TelegramRawMessage make_message(
        std::int64_t message_id,
        std::string text,
        std::int64_t reply_to_message_id = 0);

std::vector<optionx::bridges::telegram::TelegramRawMessage> demo_archive();

struct ParseTotals {
    std::size_t messages = 0;
    std::size_t signals = 0;
    std::size_t outcomes = 0;
    std::size_t diagnostics = 0;
    std::size_t unparsed = 0;
};

void print_parsed(
        const optionx::bridges::telegram::TelegramParsedMessage& parsed);

void update_totals(
        const optionx::bridges::telegram::TelegramParsedMessage& parsed,
        ParseTotals& totals);

int replay_jsonl(
        const std::string& input_path,
        const optionx::bridges::telegram::TelegramSignalParser& parser,
        ParseTotals& totals);

} // namespace

int main(int argc, char** argv) {
    // In production these records arrive one at a time from
    // tg-client-stdio::WorkerClient::stream_messages(). Keeping the replay
    // loop source-independent makes parser tests and backtests deterministic.
    optionx::bridges::telegram::TelegramSignalParser parser;
    ParseTotals totals;
    if (argc == 1) {
        for (const auto& raw : demo_archive()) {
            const auto parsed = parser.parse(raw);
            print_parsed(parsed);
            update_totals(parsed, totals);
        }
    }
    else if (argc == 3 && std::string(argv[1]) == "--input") {
        if (replay_jsonl(argv[2], parser, totals) != 0) {
            return 1;
        }
    }
    else {
        std::cerr << "usage: telegram_archive_parser_smoke [--input <messages.jsonl>]\n";
        return 2;
    }
    std::cout << "messages=" << totals.messages
              << " signals=" << totals.signals
              << " outcomes=" << totals.outcomes
              << " diagnostics=" << totals.diagnostics
              << " unparsed=" << totals.unparsed << '\n';
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

int replay_jsonl(
        const std::string& input_path,
        const optionx::bridges::telegram::TelegramSignalParser& parser,
        ParseTotals& totals) {
    std::ifstream input(input_path);
    if (!input) {
        std::cerr << "failed to open JSONL input: " << input_path << '\n';
        return 1;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        try {
            const auto raw = optionx::bridges::telegram::TelegramRawMessage::from_json(
                nlohmann::json::parse(line));
            const auto parsed = parser.parse(raw);
            print_parsed(parsed);
            update_totals(parsed, totals);
        }
        catch (const std::exception& error) {
            std::cerr << "invalid JSONL record at line " << line_number
                      << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}

void update_totals(
        const optionx::bridges::telegram::TelegramParsedMessage& parsed,
        ParseTotals& totals) {
    ++totals.messages;
    totals.signals += parsed.signals.size();
    totals.outcomes += parsed.outcomes.size();
    totals.diagnostics += parsed.diagnostics.size();
    if (parsed.signals.empty() && parsed.outcomes.empty() && parsed.diagnostics.empty()) {
        ++totals.unparsed;
    }
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
                  << " duration=" << signal.duration
                  << " source_date_ms=" << signal.source_date_ms;
        if (signal.martingale_step) {
            std::cout << " martingale_step=" << *signal.martingale_step;
        }
        std::cout << '\n';
    }
    for (const auto& outcome : parsed.outcomes) {
        std::cout << "  outcome=" << outcome.symbol
                  << " result=" << static_cast<int>(outcome.result)
                  << " source_date_ms=" << outcome.source_date_ms;
        if (outcome.martingale_step) {
            std::cout << " martingale_step=" << *outcome.martingale_step;
        }
        std::cout << '\n';
    }
    for (const auto& diagnostic : parsed.diagnostics) {
        std::cout << "  diagnostic=" << diagnostic.code << '\n';
    }
}

} // namespace
