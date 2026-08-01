/// \file telegram_signal_bridge_smoke.cpp
/// \brief Demonstrates Telegram parsing and bridge dispatch with a fake source.

#include <optionx_cpp/bridges/telegram.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

class DemoMessageSource final
    : public optionx::bridges::telegram::TelegramMessageSource {
public:
    bool start(message_callback_t on_message, error_callback_t on_error) override;
    void stop() noexcept override;
    void emit(const optionx::bridges::telegram::TelegramRawMessage& message);

private:
    message_callback_t m_on_message;
};

std::unique_ptr<optionx::bridges::telegram::TelegramSignalBridgeConfig>
make_config();

optionx::bridges::telegram::TelegramRawMessage make_message(
        std::string text,
        std::int64_t message_id);

} // namespace

int main() {
    // The production source can be an adapter around tg-client-stdio. A fake
    // source keeps this example deterministic and runnable without Telegram
    // credentials or an authorized session.
    auto source = std::make_shared<DemoMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    if (!bridge.configure(make_config())) {
        std::cerr << "failed to configure Telegram bridge\n";
        return 1;
    }

    std::int64_t next_signal_id = 0;
    bridge.on_signal_id() = [&next_signal_id]() {
        return ++next_signal_id;
    };
    bridge.on_status_update() = [](const optionx::BridgeStatusUpdate& update) {
        std::cout << "status=" << optionx::to_str(update.status);
        if (!update.message.empty()) {
            std::cout << " message=" << update.message;
        }
        std::cout << '\n';
    };
    bridge.on_signal_report() = [](const optionx::BridgeSignalReport& report) {
        std::cout << "report=" << report.reason_code
                  << " status=" << optionx::to_str(report.status) << '\n';
    };
    bridge.on_trade_signal() = [](std::unique_ptr<optionx::TradeSignal> signal) {
        std::cout << "signal=" << signal->symbol
                  << " direction=" << optionx::to_str(signal->order_type)
                  << " duration=" << signal->duration
                  << " id=" << signal->signal_id << '\n';
    };

    bridge.run();
    source->emit(make_message("EURUSD BUY 5m", 1));
    source->emit(make_message("EURUSD BUY 5m", 1));
    bridge.shutdown();
    return 0;
}

namespace {

bool DemoMessageSource::start(message_callback_t on_message, error_callback_t on_error) {
    (void)on_error;
    m_on_message = std::move(on_message);
    return true;
}

void DemoMessageSource::stop() noexcept {
    m_on_message = {};
}

void DemoMessageSource::emit(
        const optionx::bridges::telegram::TelegramRawMessage& message) {
    if (m_on_message) {
        m_on_message(message);
    }
}

std::unique_ptr<optionx::bridges::telegram::TelegramSignalBridgeConfig>
make_config() {
    auto config = std::make_unique<
        optionx::bridges::telegram::TelegramSignalBridgeConfig>();
    config->bridge_id = 9001;
    config->fixed_amount = 1.0;
    return config;
}

optionx::bridges::telegram::TelegramRawMessage make_message(
        std::string text,
        const std::int64_t message_id) {
    optionx::bridges::telegram::TelegramRawMessage message;
    message.chat_id = "demo-signals";
    message.chat_title = "Demo Telegram Signals";
    message.message_id = message_id;
    message.date_ms = 1800000000000;
    message.text = std::move(text);
    return message;
}

} // namespace
