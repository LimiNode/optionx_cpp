#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_HPP_INCLUDED

/// \file TelegramSignalBridge.hpp
/// \brief Converts Telegram message events into OptionX TradeSignal objects.

#include "bridges/BaseBridge.hpp"
#include "bridges/detail/BridgeTradeSignalValidation.hpp"
#include "bridges/telegram/TelegramSignalBridgeConfig.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace optionx::bridges::telegram {

    /// \class TelegramMessageSource
    /// \brief Minimal live-message source boundary used by the Telegram bridge.
    ///
    /// A concrete adapter may be backed by tg-client-stdio, a test fixture, or
    /// another Telegram client. The source must stop invoking callbacks before
    /// `stop()` returns.
    class TelegramMessageSource {
    public:
        using message_callback_t = std::function<void(const TelegramRawMessage&)>;
        using error_callback_t = std::function<void(const std::string&)>;

        virtual ~TelegramMessageSource() = default;
        virtual bool start(message_callback_t on_message,
                           error_callback_t on_error) = 0;
        virtual void stop() noexcept = 0;
    };

    /// \class TelegramSignalBridge
    /// \brief Publishes executable signals parsed from live Telegram messages.
    class TelegramSignalBridge final : public BaseBridge {
    private:
        struct RuntimeState {
            std::mutex mutex;
            bridge_status_callback_t status_callback;
            BaseBridge::trade_signal_callback_t trade_signal_callback;
            BaseBridge::signal_report_callback_t signal_report_callback;
            BaseBridge::signal_id_allocator_t signal_id_allocator;
            std::shared_ptr<TelegramMessageSource> source;
            std::deque<std::string> dedupe_order;
            std::unordered_set<std::string> dedupe_keys;
            bool running = false;
        };

    public:
        explicit TelegramSignalBridge(
                std::shared_ptr<TelegramMessageSource> source = {})
            : m_state(std::make_shared<RuntimeState>()),
              m_source(std::move(source)) {}

        ~TelegramSignalBridge() override {
            shutdown();
        }

        /// \brief Replaces the live source while the bridge is stopped.
        bool set_message_source(std::shared_ptr<TelegramMessageSource> source) {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->running) {
                return false;
            }
            m_source = std::move(source);
            return true;
        }

        bool configure(std::unique_ptr<IBridgeConfig> config) override {
            if (!config) {
                return false;
            }
            const auto* typed = dynamic_cast<const TelegramSignalBridgeConfig*>(config.get());
            if (!typed) {
                config->dispatch_callbacks(false, "Invalid Telegram signal bridge config type.");
                return false;
            }
            auto next_config = std::make_shared<TelegramSignalBridgeConfig>(*typed);
            const auto validation = next_config->validate();
            config->dispatch_callbacks(validation.first, validation.second);
            if (!validation.first) {
                return false;
            }
            std::lock_guard<std::mutex> lock(m_config_mutex);
            m_config = std::move(next_config);
            return true;
        }

        bridge_status_callback_t& on_status_update() override {
            return m_state->status_callback;
        }

        trade_signal_callback_t& on_trade_signal() override {
            return m_state->trade_signal_callback;
        }

        signal_report_callback_t& on_signal_report() override {
            return m_state->signal_report_callback;
        }

        signal_id_allocator_t& on_signal_id() override {
            return m_state->signal_id_allocator;
        }

        void update_account_info(const AccountInfoUpdate& info) override {
            (void)info;
        }

        void run() override {
            const auto config = get_config();
            if (!config) {
                notify_status(BridgeStatus::SERVER_START_FAILED,
                              "Telegram bridge is not configured.");
                return;
            }
            auto source = get_source();
            if (!source) {
                notify_status(BridgeStatus::SERVER_START_FAILED,
                              "Telegram bridge has no message source.");
                return;
            }
            if (!get_signal_id_allocator()) {
                notify_status(BridgeStatus::SERVER_START_FAILED,
                              "Telegram bridge requires a signal ID allocator.");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                if (m_state->running) {
                    return;
                }
                m_state->running = true;
                m_state->source = source;
                m_state->dedupe_keys.clear();
                m_state->dedupe_order.clear();
            }

            try {
                const auto parser = TelegramSignalParser(config->parser);
                const bool started = source->start(
                    [state = m_state, config, parser](const TelegramRawMessage& raw) {
                        process_message(state, *config, parser, raw);
                    },
                    [state = m_state](const std::string& message) {
                        notify_status(state, BridgeStatus::CONNECTION_ERROR, message);
                    });
                if (!started) {
                    set_running(false);
                    notify_status(BridgeStatus::SERVER_START_FAILED,
                                  "Telegram message source failed to start.");
                    return;
                }
                notify_status(BridgeStatus::SERVER_STARTED, {});
            }
            catch (const std::exception& error) {
                set_running(false);
                notify_status(BridgeStatus::SERVER_START_FAILED, error.what());
            }
            catch (...) {
                set_running(false);
                notify_status(BridgeStatus::SERVER_START_FAILED,
                              "Telegram message source threw an unknown exception.");
            }
        }

        void shutdown() override {
            std::shared_ptr<TelegramMessageSource> source;
            bool was_running = false;
            {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                was_running = m_state->running;
                m_state->running = false;
                source = m_state->source;
                m_state->source.reset();
            }
            if (source) {
                try {
                    source->stop();
                }
                catch (...) {
                    notify_status(BridgeStatus::CONNECTION_ERROR,
                                  "Telegram message source threw during stop.");
                }
            }
            if (was_running) {
                notify_status(BridgeStatus::SERVER_STOPPED, {});
            }
        }

    private:
        std::shared_ptr<const TelegramSignalBridgeConfig> get_config() const {
            std::lock_guard<std::mutex> lock(m_config_mutex);
            return m_config;
        }

        std::shared_ptr<TelegramMessageSource> get_source() const {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_source;
        }

        BaseBridge::signal_id_allocator_t get_signal_id_allocator() const {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_state->signal_id_allocator;
        }

        void set_running(const bool running) {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->running = running;
            if (!running) {
                m_state->source.reset();
            }
        }

        static void notify_status(
                const std::shared_ptr<RuntimeState>& state,
                const BridgeStatus status,
                const std::string& message) {
            bridge_status_callback_t callback;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                callback = state->status_callback;
            }
            if (callback) {
                try {
                    callback({status, {}, message});
                }
                catch (...) {
                }
            }
        }

        void notify_status(const BridgeStatus status, const std::string& message) const {
            notify_status(m_state, status, message);
        }

        static void emit_report(
                const std::shared_ptr<RuntimeState>& state,
                BridgeSignalReport report) {
            signal_report_callback_t callback;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                callback = state->signal_report_callback;
            }
            if (callback) {
                try {
                    callback(report);
                }
                catch (...) {
                }
            }
        }

        static std::string make_dedupe_key(
                const TelegramRawMessage& raw,
                const TelegramParsedSignal& parsed,
                const std::size_t index) {
            return raw.message_identity() + "|" + std::to_string(index) + "|" +
                parsed.symbol + "|" + optionx::to_str(parsed.order_type) + "|" +
                optionx::to_str(parsed.option_type) + "|" +
                std::to_string(parsed.duration) + "|" +
                std::to_string(parsed.expiry_time);
        }

        static void process_message(
                const std::shared_ptr<RuntimeState>& state,
                const TelegramSignalBridgeConfig& config,
                const TelegramSignalParser& parser,
                const TelegramRawMessage& raw) {
            try {
                raw.validate();
                const auto parsed = parser.parse(raw);
                for (const auto& diagnostic : parsed.diagnostics) {
                    BridgeSignalReport report;
                    report.bridge_id = config.bridge_id;
                    report.bridge_type = BridgeType::TELEGRAM_SIGNAL;
                    report.status = BridgeSignalReportStatus::INVALID;
                    report.reason_code = diagnostic.code;
                    report.message = diagnostic.message;
                    report.event_id = raw.message_identity();
                    report.raw_payload = raw.to_json();
                    report.context = {
                        {"offset", diagnostic.offset},
                        {"length", diagnostic.length},
                    };
                    emit_report(state, std::move(report));
                }

                for (std::size_t index = 0; index < parsed.signals.size(); ++index) {
                    const auto& parsed_signal = parsed.signals[index];
                    auto signal = std::make_unique<TradeSignal>();
                    signal->bridge_id = config.bridge_id;
                    signal->symbol = parsed_signal.symbol;
                    signal->order_type = parsed_signal.order_type;
                    signal->option_type = parsed_signal.option_type;
                    signal->duration = parsed_signal.duration;
                    signal->expiry_time = parsed_signal.expiry_time;
                    signal->signal_name = parsed_signal.signal_name;
                    signal->comment = raw.text;
                    signal->amount = config.fixed_amount;
                    const auto dedupe_key = make_dedupe_key(raw, parsed_signal, index);
                    signal->unique_hash = dedupe_key;

                    detail::validate_executable_trade_signal(
                        *signal, "Telegram signal", true);

                    BaseBridge::signal_id_allocator_t allocator;
                    BaseBridge::trade_signal_callback_t callback;
                    bool duplicate = false;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        if (!state->running) {
                            return;
                        }
                        if (state->dedupe_keys.find(dedupe_key) != state->dedupe_keys.end()) {
                            duplicate = true;
                        }
                        else {
                            state->dedupe_keys.insert(dedupe_key);
                            state->dedupe_order.push_back(dedupe_key);
                            while (state->dedupe_order.size() > config.dedupe_cache_size) {
                                state->dedupe_keys.erase(state->dedupe_order.front());
                                state->dedupe_order.pop_front();
                            }
                            allocator = state->signal_id_allocator;
                            callback = state->trade_signal_callback;
                        }
                    }
                    if (duplicate) {
                        BridgeSignalReport report;
                        report.bridge_id = config.bridge_id;
                        report.bridge_type = BridgeType::TELEGRAM_SIGNAL;
                        report.status = BridgeSignalReportStatus::DUPLICATE;
                        report.reason_code = "duplicate_message";
                        report.message = "Telegram signal was already dispatched.";
                        report.event_id = raw.message_identity();
                        report.dedupe_key = dedupe_key;
                        report.symbol = parsed_signal.symbol;
                        report.signal_name = parsed_signal.signal_name;
                        report.raw_payload = raw.to_json();
                        emit_report(state, std::move(report));
                        continue;
                    }

                    try {
                        signal->signal_id = allocator();
                        if (signal->signal_id == 0) {
                            throw std::runtime_error("Telegram signal ID allocator returned zero.");
                        }
                    }
                    catch (const std::exception& error) {
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->dedupe_keys.erase(dedupe_key);
                        }
                        emit_report(state, BridgeSignalReport{
                            config.bridge_id,
                            BridgeType::TELEGRAM_SIGNAL,
                            BridgeSignalReportStatus::INTAKE_ERROR,
                            "signal_id_allocation_failed",
                            error.what(),
                            {},
                            raw.message_identity(),
                            dedupe_key,
                            parsed_signal.symbol,
                            parsed_signal.signal_name,
                            {},
                            raw.to_json(),
                            {},
                            {},
                            0,
                            raw.date_ms,
                        });
                        continue;
                    }
                    if (callback) {
                        try {
                            callback(std::move(signal));
                        }
                        catch (...) {
                            emit_report(state, BridgeSignalReport{
                                config.bridge_id,
                                BridgeType::TELEGRAM_SIGNAL,
                                BridgeSignalReportStatus::INTAKE_ERROR,
                                "trade_signal_callback_failed",
                                "Telegram trade signal callback threw.",
                                {},
                                raw.message_identity(),
                                dedupe_key,
                                parsed_signal.symbol,
                                parsed_signal.signal_name,
                                {},
                                raw.to_json(),
                                {},
                                {},
                                0,
                                raw.date_ms,
                            });
                        }
                    }
                }
            }
            catch (const std::exception& error) {
                emit_report(state, BridgeSignalReport{
                    config.bridge_id,
                    BridgeType::TELEGRAM_SIGNAL,
                    BridgeSignalReportStatus::INVALID,
                    "telegram_message_parse_failed",
                    error.what(),
                    {},
                    raw.message_identity(),
                    {},
                    {},
                    {},
                    {},
                    raw.to_json(),
                    {},
                    {},
                    0,
                    raw.date_ms,
                });
            }
        }

        std::shared_ptr<RuntimeState> m_state;
        mutable std::mutex m_config_mutex;
        std::shared_ptr<const TelegramSignalBridgeConfig> m_config;
        std::shared_ptr<TelegramMessageSource> m_source;
    };

} // namespace optionx::bridges::telegram

#endif // OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_HPP_INCLUDED
