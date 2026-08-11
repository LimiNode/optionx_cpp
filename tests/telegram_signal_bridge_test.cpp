#include <gtest/gtest.h>

#include "optionx_cpp/bridges/telegram.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

class FakeMessageSource final
    : public optionx::bridges::telegram::TelegramMessageSource {
public:
    bool start(message_callback_t on_message, error_callback_t on_error) override {
        (void)on_error;
        m_on_message = std::move(on_message);
        started = true;
        return true;
    }

    void stop() noexcept override {
        stopped = true;
        m_on_message = {};
    }

    void emit(optionx::bridges::telegram::TelegramRawMessage message) {
        if (m_on_message) {
            m_on_message(message);
        }
    }

    bool started = false;
    bool stopped = false;

private:
    message_callback_t m_on_message;
};

optionx::bridges::telegram::TelegramRawMessage make_message() {
    optionx::bridges::telegram::TelegramRawMessage message;
    message.chat_id = "-10042";
    message.chat_title = "Signals";
    message.message_id = 123;
    message.date_ms = 1800000000000;
    message.text = "EURUSD BUY 5m";
    return message;
}

std::unique_ptr<optionx::bridges::telegram::TelegramSignalBridgeConfig> config() {
    auto value = std::make_unique<
        optionx::bridges::telegram::TelegramSignalBridgeConfig>();
    value->bridge_id = 17;
    value->fixed_amount = 1.0;
    return value;
}

} // namespace

TEST(TelegramSignalBridge, PublishesParsedSignalAndReportsDuplicate) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    ASSERT_TRUE(bridge.configure(config()));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::vector<optionx::BridgeSignalReport> reports;
    std::int64_t next_signal_id = 100;
    bridge.on_signal_id() = [&] { return ++next_signal_id; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        signals.push_back(std::move(signal));
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    ASSERT_TRUE(source->started);
    source->emit(make_message());
    source->emit(make_message());

    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals.front()->signal_id, 101);
    EXPECT_EQ(signals.front()->bridge_id, 17);
    EXPECT_EQ(signals.front()->symbol, "EURUSD");
    EXPECT_EQ(signals.front()->duration, 300u);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.front().status,
              optionx::BridgeSignalReportStatus::DUPLICATE);
    EXPECT_EQ(reports.front().reason_code, "duplicate_message");

    bridge.shutdown();
    EXPECT_TRUE(source->stopped);
}

TEST(TelegramSignalBridge, RejectsInvalidConfigurationBeforeStartingSource) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto invalid = config();
    invalid->fixed_amount = 0.0;

    EXPECT_FALSE(bridge.configure(std::move(invalid)));
    bridge.run();
    EXPECT_FALSE(source->started);
}

TEST(TelegramSignalBridgeConfig, RoundTripsParserRecognitionSettings) {
    auto original = config();
    nlohmann::json serialized;
    original->to_json(serialized);

    optionx::bridges::telegram::TelegramSignalBridgeConfig restored;
    restored.from_json(serialized);

    EXPECT_EQ(restored.parser.symbol_pattern, original->parser.symbol_pattern);
    EXPECT_EQ(restored.parser.otc_symbol_suffix, original->parser.otc_symbol_suffix);
    EXPECT_EQ(restored.parser.expiry_policy.mode,
              original->parser.expiry_policy.mode);
    EXPECT_EQ(restored.parser.expiry_policy.custom_duration_seconds,
              original->parser.expiry_policy.custom_duration_seconds);
    ASSERT_EQ(restored.parser.direction_rules.size(),
              original->parser.direction_rules.size());
    ASSERT_EQ(restored.parser.outcome_rules.size(),
              original->parser.outcome_rules.size());
    EXPECT_EQ(restored.parser.direction_rules.front().pattern,
              original->parser.direction_rules.front().pattern);
    EXPECT_EQ(restored.parser.outcome_rules.back().fixed_result,
              optionx::bridges::telegram::TelegramOutcomeResult::LOSS);
    EXPECT_EQ(restored.parser.outcome_rules.back().direction_group,
              original->parser.outcome_rules.back().direction_group);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
