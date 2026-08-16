#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_HPP_INCLUDED

/// \file TelegramSignalBridge.hpp
/// \brief Converts Telegram message events into OptionX TradeSignal objects.

#include "bridges/BaseBridge.hpp"
#include "bridges/detail/BridgeTradeSignalValidation.hpp"
#include "bridges/telegram/TelegramSignalBridgeConfig.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
            std::unordered_map<std::string, std::int32_t> martingale_steps;
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
                m_state->martingale_steps.clear();
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

        static std::string martingale_key(
                const TelegramRawMessage& raw,
                const TelegramParsedSignal& parsed) {
            return raw.chat_id + "|" + raw.topic_id + "|" + parsed.symbol + "|" +
                optionx::to_str(parsed.order_type) + "|" + parsed.signal_name;
        }

        static std::int64_t current_time_ms() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        static bool is_stale(
                const TelegramSignalBridgeConfig& config,
                const TelegramRawMessage& raw,
                const std::int64_t received_time_ms) {
            if (config.max_signal_age_seconds == 0 || raw.date_ms <= 0 ||
                received_time_ms <= raw.date_ms ||
                config.martingale_policy == TelegramMartingalePolicy::CONTIGUOUS_STEPS) {
                return false;
            }
            const auto max_age_ms = static_cast<std::int64_t>(
                config.max_signal_age_seconds) * 1000;
            return received_time_ms - raw.date_ms > max_age_ms;
        }

        static BridgeSignalReport make_signal_report(
                const TelegramSignalBridgeConfig& config,
                const TelegramRawMessage& raw,
                const TelegramParsedSignal& parsed,
                const std::string& dedupe_key,
                const std::int64_t received_time_ms,
                const BridgeSignalReportStatus status,
                std::string reason_code,
                std::string message) {
            BridgeSignalReport report;
            report.bridge_id = config.bridge_id;
            report.bridge_type = BridgeType::TELEGRAM_SIGNAL;
            report.status = status;
            report.reason_code = std::move(reason_code);
            report.message = std::move(message);
            report.event_id = raw.message_identity();
            report.dedupe_key = dedupe_key;
            report.symbol = parsed.symbol;
            report.signal_name = parsed.signal_name;
            report.raw_payload = raw.to_json();
            report.received_time_ms = received_time_ms;
            report.source_time_ms = raw.date_ms;
            return report;
        }

        static void rollback_dispatch_state(
                const std::shared_ptr<RuntimeState>& state,
                const std::string& dedupe_key,
                const std::string& sequence_key,
                const bool martingale_step_recorded,
                const std::optional<std::int32_t>& previous_martingale_step) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->dedupe_keys.erase(dedupe_key);
            const auto dedupe = std::find(
                state->dedupe_order.begin(), state->dedupe_order.end(), dedupe_key);
            if (dedupe != state->dedupe_order.end()) {
                state->dedupe_order.erase(dedupe);
            }
            if (!martingale_step_recorded) {
                return;
            }
            if (previous_martingale_step) {
                state->martingale_steps[sequence_key] = *previous_martingale_step;
            }
            else {
                state->martingale_steps.erase(sequence_key);
            }
        }

        static void process_message(
                const std::shared_ptr<RuntimeState>& state,
                const TelegramSignalBridgeConfig& config,
                const TelegramSignalParser& parser,
                const TelegramRawMessage& raw) {
            try {
                raw.validate();
                const auto received_time_ms = current_time_ms();
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
                    report.received_time_ms = received_time_ms;
                    report.source_time_ms = raw.date_ms;
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
                    signal->source_time_ms = parsed_signal.source_date_ms;
                    if (parsed_signal.martingale_step) {
                        signal->mm_step = *parsed_signal.martingale_step;
                    }
                    signal->amount = config.fixed_amount;
                    const auto dedupe_key = make_dedupe_key(raw, parsed_signal, index);
                    signal->unique_hash = dedupe_key;

                    if (is_stale(config, raw, received_time_ms)) {
                        BridgeSignalReport report;
                        report.bridge_id = config.bridge_id;
                        report.bridge_type = BridgeType::TELEGRAM_SIGNAL;
                        report.status = BridgeSignalReportStatus::REJECTED;
                        report.reason_code = "stale_signal";
                        report.message = "Telegram signal exceeded the configured maximum age.";
                        report.event_id = raw.message_identity();
                        report.dedupe_key = dedupe_key;
                        report.symbol = parsed_signal.symbol;
                        report.signal_name = parsed_signal.signal_name;
                        report.raw_payload = raw.to_json();
                        report.context = {
                            {"age_ms", received_time_ms - raw.date_ms},
                            {"max_age_ms", static_cast<std::int64_t>(
                                config.max_signal_age_seconds) * 1000},
                        };
                        report.received_time_ms = received_time_ms;
                        report.source_time_ms = raw.date_ms;
                        emit_report(state, std::move(report));
                        continue;
                    }

                    detail::validate_executable_trade_signal(
                        *signal, "Telegram signal", true);

                    BaseBridge::signal_id_allocator_t allocator;
                    BaseBridge::trade_signal_callback_t callback;
                    bool duplicate = false;
                    std::optional<BridgeSignalReport> policy_report;
                    std::string sequence_key;
                    bool martingale_step_recorded = false;
                    std::optional<std::int32_t> previous_martingale_step;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        if (!state->running) {
                            return;
                        }
                        if (state->dedupe_keys.find(dedupe_key) != state->dedupe_keys.end()) {
                            duplicate = true;
                        }
                        else if (config.martingale_policy ==
                                 TelegramMartingalePolicy::FIRST_SIGNAL_ONLY &&
                                 parsed_signal.martingale_step &&
                                 *parsed_signal.martingale_step > 0) {
                            policy_report = make_signal_report(
                                config, raw, parsed_signal, dedupe_key, received_time_ms,
                                BridgeSignalReportStatus::IGNORED,
                                "martingale_step_filtered",
                                "Telegram martingale step is disabled by policy.");
                            policy_report->context = {{"martingale_step", *parsed_signal.martingale_step}};
                        }
                        else if (config.martingale_policy ==
                                 TelegramMartingalePolicy::CONTIGUOUS_STEPS &&
                                 !parsed_signal.martingale_step) {
                            policy_report = make_signal_report(
                                config, raw, parsed_signal, dedupe_key, received_time_ms,
                                BridgeSignalReportStatus::REJECTED,
                                "martingale_step_missing",
                                "Telegram martingale policy requires an explicit step.");
                        }
                        else if (config.martingale_policy ==
                                 TelegramMartingalePolicy::CONTIGUOUS_STEPS) {
                            sequence_key = martingale_key(raw, parsed_signal);
                            const auto existing = state->martingale_steps.find(sequence_key);
                            if (*parsed_signal.martingale_step != 0 &&
                                (existing == state->martingale_steps.end() ||
                                 *parsed_signal.martingale_step != existing->second + 1)) {
                                policy_report = make_signal_report(
                                    config, raw, parsed_signal, dedupe_key, received_time_ms,
                                    BridgeSignalReportStatus::REJECTED,
                                    "martingale_step_out_of_sequence",
                                    "Telegram martingale step is not contiguous.");
                                policy_report->context = {
                                    {"martingale_step", *parsed_signal.martingale_step},
                                    {"previous_step", existing == state->martingale_steps.end()
                                        ? -1 : existing->second},
                                };
                            }
                            else {
                                if (existing != state->martingale_steps.end()) {
                                    previous_martingale_step = existing->second;
                                }
                                state->martingale_steps[sequence_key] = *parsed_signal.martingale_step;
                                martingale_step_recorded = true;
                            }
                        }
                        if (!duplicate && !policy_report) {
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
                        emit_report(state, make_signal_report(
                            config, raw, parsed_signal, dedupe_key, received_time_ms,
                            BridgeSignalReportStatus::DUPLICATE,
                            "duplicate_message", "Telegram signal was already dispatched."));
                        continue;
                    }
                    if (policy_report) {
                        emit_report(state, std::move(*policy_report));
                        continue;
                    }

                    try {
                        signal->signal_id = allocator();
                        if (signal->signal_id == 0) {
                            throw std::runtime_error("Telegram signal ID allocator returned zero.");
                        }
                    }
                    catch (const std::exception& error) {
                        rollback_dispatch_state(
                            state, dedupe_key, sequence_key, martingale_step_recorded,
                            previous_martingale_step);
                        emit_report(state, make_signal_report(
                            config, raw, parsed_signal, dedupe_key, received_time_ms,
                            BridgeSignalReportStatus::INTAKE_ERROR,
                            "signal_id_allocation_failed", error.what()));
                        continue;
                    }
                    if (callback) {
                        try {
                            callback(std::move(signal));
                        }
                        catch (...) {
                            rollback_dispatch_state(
                                state, dedupe_key, sequence_key, martingale_step_recorded,
                                previous_martingale_step);
                            emit_report(state, make_signal_report(
                                config, raw, parsed_signal, dedupe_key, received_time_ms,
                                BridgeSignalReportStatus::INTAKE_ERROR,
                                "trade_signal_callback_failed",
                                "Telegram trade signal callback threw."));
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
