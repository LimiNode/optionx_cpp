#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_HPP_INCLUDED

/// \file TelegramSignalBridge.hpp
/// \brief Converts Telegram message events into OptionX TradeSignal objects.

#include "bridges/BaseBridge.hpp"
#include "bridges/detail/BridgeTradeSignalValidation.hpp"
#include "bridges/telegram/TelegramSignalBridgeConfig.hpp"
#include "data/trading/trade_state_traits.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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
            struct AntiMartingaleGroupState {
                std::uint32_t next_step = 0;
                bool pending_trade = false;
                SignalId pending_signal_id = 0;
            };

            struct SourceChainState {
                TelegramRawMessage raw;
                TelegramParsedSignal parsed;
                TelegramSourceChainRule rule;
                std::int32_t step = -1;
                bool waiting = false;
                std::chrono::steady_clock::time_point deadline;
            };

            // Source adapters may invoke messages concurrently. Serialize the
            // full intake path and hold contiguous sequence steps as pending
            // until their allocator and callback complete.
            std::recursive_mutex intake_mutex;
            std::mutex mutex;
            bridge_status_callback_t status_callback;
            BaseBridge::trade_signal_callback_t trade_signal_callback;
            BaseBridge::signal_report_callback_t signal_report_callback;
            BaseBridge::signal_id_allocator_t signal_id_allocator;
            std::shared_ptr<TelegramMessageSource> source;
            std::deque<std::string> dedupe_order;
            std::unordered_set<std::string> dedupe_keys;
            std::unordered_map<std::string, std::int32_t> martingale_steps;
            std::unordered_set<std::string> pending_martingale_sequences;
            std::unordered_map<std::string, AntiMartingaleGroupState>
                anti_martingale_groups;
            std::unordered_map<SignalId, std::string> anti_martingale_signal_groups;
            std::unordered_map<std::string, SourceChainState> source_chain_groups;
            std::unordered_map<std::string, std::string> source_chain_signal_groups;
            std::unordered_map<std::string, std::int32_t> assumed_source_chain_steps;
            std::condition_variable source_chain_cv;
            std::thread source_chain_thread;
            std::thread::id source_chain_thread_id;
            bool source_chain_stop = false;
            bool source_chain_join_pending = false;
            std::shared_ptr<const TelegramSignalBridgeConfig> active_config;
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

        void update_trade_result(
                const TradeRequest& request,
                const TradeResult& result) override {
            if (!is_terminal_trade_state(result.trade_state) || request.signal_id == 0) {
                return;
            }

            std::lock_guard<std::mutex> lock(m_state->mutex);
            const auto config = m_state->active_config;
            if (!config || !config->anti_martingale_enabled) {
                return;
            }
            const auto signal_group = m_state->anti_martingale_signal_groups.find(
                request.signal_id);
            if (signal_group == m_state->anti_martingale_signal_groups.end()) {
                return;
            }
            const auto group = m_state->anti_martingale_groups.find(signal_group->second);
            if (group == m_state->anti_martingale_groups.end() ||
                !group->second.pending_trade ||
                group->second.pending_signal_id != request.signal_id) {
                return;
            }

            m_state->anti_martingale_signal_groups.erase(signal_group);
            auto& state = group->second;
            state.pending_trade = false;
            state.pending_signal_id = 0;
            if (is_win(result.trade_state) &&
                state.next_step < config->anti_martingale_max_steps) {
                ++state.next_step;
            }
            else {
                state.next_step = 0;
            }
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
                std::unique_lock<std::mutex> lock(m_state->mutex);
                if (m_state->source_chain_join_pending) {
                    if (m_state->source_chain_thread_id ==
                        std::this_thread::get_id()) {
                        return;
                    }
                    m_state->source_chain_cv.wait(lock, [state = m_state] {
                        return !state->source_chain_join_pending;
                    });
                }
                if (m_state->running) {
                    return;
                }
                m_state->running = true;
                m_state->source = source;
                m_state->dedupe_keys.clear();
                m_state->dedupe_order.clear();
                m_state->martingale_steps.clear();
                m_state->pending_martingale_sequences.clear();
                m_state->anti_martingale_groups.clear();
                m_state->anti_martingale_signal_groups.clear();
                m_state->source_chain_groups.clear();
                m_state->source_chain_signal_groups.clear();
                m_state->assumed_source_chain_steps.clear();
                m_state->source_chain_stop = false;
                m_state->active_config = config;
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
                start_source_chain_watchdog(m_state, config);
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
                m_state->anti_martingale_groups.clear();
                m_state->anti_martingale_signal_groups.clear();
                m_state->source_chain_groups.clear();
                m_state->source_chain_signal_groups.clear();
                m_state->assumed_source_chain_steps.clear();
                m_state->source_chain_stop = true;
                m_state->source_chain_join_pending =
                    m_state->source_chain_thread.joinable();
                m_state->active_config.reset();
            }
            m_state->source_chain_cv.notify_all();
            const bool watchdog_joined =
                join_source_chain_watchdog(m_state, was_running);
            if (source) {
                try {
                    source->stop();
                }
                catch (...) {
                    notify_status(BridgeStatus::CONNECTION_ERROR,
                                  "Telegram message source threw during stop.");
                }
            }
            if (was_running && watchdog_joined) {
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
            {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                m_state->running = running;
                if (!running) {
                    m_state->source.reset();
                    m_state->anti_martingale_groups.clear();
                    m_state->anti_martingale_signal_groups.clear();
                    m_state->source_chain_groups.clear();
                    m_state->source_chain_signal_groups.clear();
                    m_state->assumed_source_chain_steps.clear();
                    m_state->source_chain_stop = true;
                    m_state->source_chain_join_pending =
                        m_state->source_chain_thread.joinable();
                    m_state->active_config.reset();
                }
            }
            m_state->source_chain_cv.notify_all();
            if (!running) {
                join_source_chain_watchdog(m_state, false);
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

        static double anti_martingale_amount(
                const TelegramSignalBridgeConfig& config,
                const std::uint32_t step) {
            auto amount = config.fixed_amount;
            for (std::uint32_t index = 0; index < step; ++index) {
                const auto maximum_before_multiplier =
                    config.anti_martingale_max_amount /
                    config.anti_martingale_multiplier;
                if (amount >= maximum_before_multiplier) {
                    return config.anti_martingale_max_amount;
                }
                amount *= config.anti_martingale_multiplier;
            }
            return std::min(amount, config.anti_martingale_max_amount);
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
                received_time_ms <= raw.date_ms) {
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

        static BridgeSignalReport make_stale_signal_report(
                const TelegramSignalBridgeConfig& config,
                const TelegramRawMessage& raw,
                const TelegramParsedSignal& parsed,
                const std::string& dedupe_key,
                const std::int64_t received_time_ms) {
            auto report = make_signal_report(
                config, raw, parsed, dedupe_key, received_time_ms,
                BridgeSignalReportStatus::REJECTED,
                "stale_signal", "Telegram signal exceeded the configured maximum age.");
            report.context = {
                {"age_ms", received_time_ms - raw.date_ms},
                {"max_age_ms", static_cast<std::int64_t>(
                    config.max_signal_age_seconds) * 1000},
            };
            return report;
        }

        static void rollback_dispatch_state(
                const std::shared_ptr<RuntimeState>& state,
                const std::string& dedupe_key,
                const std::string& sequence_key,
                const bool martingale_step_recorded,
                const std::optional<std::int32_t>& previous_martingale_step,
                const std::string& anti_martingale_key,
                const bool anti_martingale_pending) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->dedupe_keys.erase(dedupe_key);
            const auto dedupe = std::find(
                state->dedupe_order.begin(), state->dedupe_order.end(), dedupe_key);
            if (dedupe != state->dedupe_order.end()) {
                state->dedupe_order.erase(dedupe);
            }
            if (martingale_step_recorded) {
                if (previous_martingale_step) {
                    state->martingale_steps[sequence_key] = *previous_martingale_step;
                }
                else {
                    state->martingale_steps.erase(sequence_key);
                }
                state->pending_martingale_sequences.erase(sequence_key);
            }
            if (anti_martingale_pending) {
                const auto group = state->anti_martingale_groups.find(anti_martingale_key);
                if (group != state->anti_martingale_groups.end()) {
                    group->second.pending_trade = false;
                    group->second.pending_signal_id = 0;
                }
            }
        }

        static void rollback_assumed_source_chain_step(
                const std::shared_ptr<RuntimeState>& state,
                const std::string& source_chain_key,
                const std::int32_t expected_step) {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto assumed = state->assumed_source_chain_steps.find(source_chain_key);
            if (assumed != state->assumed_source_chain_steps.end() &&
                assumed->second == expected_step) {
                state->assumed_source_chain_steps.erase(assumed);
            }
        }

        static bool register_anti_martingale_dispatch(
                const std::shared_ptr<RuntimeState>& state,
                const std::string& anti_martingale_key,
                const SignalId signal_id) {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto group = state->anti_martingale_groups.find(anti_martingale_key);
            if (group == state->anti_martingale_groups.end() ||
                !group->second.pending_trade || group->second.pending_signal_id != 0 ||
                state->anti_martingale_signal_groups.find(signal_id) !=
                    state->anti_martingale_signal_groups.end()) {
                return false;
            }
            group->second.pending_signal_id = signal_id;
            state->anti_martingale_signal_groups.emplace(signal_id, anti_martingale_key);
            return true;
        }

        static void commit_martingale_dispatch_state(
                const std::shared_ptr<RuntimeState>& state,
                const std::string& sequence_key,
                const bool martingale_step_recorded) {
            if (!martingale_step_recorded) {
                return;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            state->pending_martingale_sequences.erase(sequence_key);
        }

        static const TelegramSourceChainRule* source_chain_rule_for(
                const TelegramSignalBridgeConfig& config,
                const TelegramParsedSignal& signal) {
            if (!signal.martingale_step) {
                return nullptr;
            }
            const auto rule = std::find_if(
                config.source_chain_rules.begin(), config.source_chain_rules.end(),
                [&](const auto& candidate) {
                    return candidate.signal_name == signal.signal_name;
                });
            return rule == config.source_chain_rules.end() ? nullptr : &*rule;
        }

        static std::string source_chain_key(
                const TelegramRawMessage& raw,
                const TelegramParsedSignal& signal,
                const TelegramSourceChainRule& rule) {
            return martingale_key(raw, signal) + "|" + rule.name;
        }

        static void remember_source_chain_signal(
                const std::shared_ptr<RuntimeState>& state,
                const TelegramSignalBridgeConfig& config,
                const TelegramRawMessage& raw,
                const TelegramParsedSignal& signal) {
            const auto* rule = source_chain_rule_for(config, signal);
            if (!rule || *signal.martingale_step < 0 ||
                static_cast<std::uint32_t>(*signal.martingale_step) > rule->max_step) {
                return;
            }
            const auto key = source_chain_key(raw, signal, *rule);
            std::lock_guard<std::mutex> lock(state->mutex);
            auto existing = state->source_chain_groups.find(key);
            if (*signal.martingale_step == 0) {
                // A successfully dispatched explicit first step is the only way to
                // start a new source series after an assumed continuation.
                state->assumed_source_chain_steps.erase(key);
            }
            if (*signal.martingale_step != 0 &&
                (existing == state->source_chain_groups.end() ||
                 existing->second.step + 1 != *signal.martingale_step)) {
                return;
            }
            if (existing != state->source_chain_groups.end()) {
                state->source_chain_signal_groups.erase(
                    existing->second.parsed.source_message_identity);
            }
            RuntimeState::SourceChainState next;
            next.raw = raw;
            next.parsed = signal;
            next.rule = *rule;
            next.step = *signal.martingale_step;
            state->source_chain_groups[key] = std::move(next);
            state->source_chain_signal_groups[signal.source_message_identity] = key;
        }

        static void process_source_chain_outcomes(
                const std::shared_ptr<RuntimeState>& state,
                const TelegramParsedMessage& parsed) {
            bool wake_watchdog = false;
            for (const auto& outcome : parsed.outcomes) {
                if (outcome.reply_to_message_identity.empty()) {
                    continue;
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                const auto signal_group = state->source_chain_signal_groups.find(
                    outcome.reply_to_message_identity);
                if (signal_group == state->source_chain_signal_groups.end()) {
                    continue;
                }
                const auto group = state->source_chain_groups.find(signal_group->second);
                if (group == state->source_chain_groups.end() ||
                    group->second.parsed.source_message_identity !=
                        outcome.reply_to_message_identity) {
                    continue;
                }
                const auto fields_match =
                    (outcome.symbol.empty() || outcome.symbol == group->second.parsed.symbol) &&
                    (outcome.order_type == OrderType::UNKNOWN ||
                     outcome.order_type == group->second.parsed.order_type);
                if (!fields_match || outcome.result != group->second.rule.continuation_result ||
                    static_cast<std::uint32_t>(group->second.step) >=
                        group->second.rule.max_step) {
                    state->source_chain_signal_groups.erase(signal_group);
                    state->source_chain_groups.erase(group);
                    continue;
                }
                group->second.waiting = true;
                group->second.deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(group->second.rule.timeout_seconds);
                wake_watchdog = true;
            }
            if (wake_watchdog) {
                state->source_chain_cv.notify_all();
            }
        }

        static BridgeSignalReport make_source_chain_report(
                const TelegramSignalBridgeConfig& config,
                const RuntimeState::SourceChainState& chain,
                const std::int64_t received_time_ms) {
            BridgeSignalReport report;
            report.bridge_id = config.bridge_id;
            report.bridge_type = BridgeType::TELEGRAM_SIGNAL;
            report.status = BridgeSignalReportStatus::SUSPICIOUS;
            report.reason_code = "expected_source_step_missing";
            report.message = "Telegram source did not publish the expected contiguous step in time.";
            report.event_id = chain.parsed.source_message_identity;
            report.symbol = chain.parsed.symbol;
            report.signal_name = chain.parsed.signal_name;
            report.raw_payload = chain.raw.to_json();
            report.context = {
                {"rule", chain.rule.name},
                {"last_source_step", chain.step},
                {"expected_source_step", chain.step + 1},
                {"timeout_seconds", chain.rule.timeout_seconds},
                {"action", telegram_source_chain_action_name(chain.rule.action)},
            };
            report.received_time_ms = received_time_ms;
            report.source_time_ms = chain.parsed.source_date_ms;
            return report;
        }

        static void dispatch_assumed_source_chain_signal(
                const std::shared_ptr<RuntimeState>& state,
                const TelegramSignalBridgeConfig& config,
                const RuntimeState::SourceChainState& chain) {
            const auto expected_step = chain.step + 1;
            if (chain.parsed.option_type != OptionType::SPRINT ||
                chain.parsed.duration == 0) {
                auto report = make_source_chain_report(config, chain, current_time_ms());
                report.reason_code = "assumed_signal_unsupported_expiry";
                report.message = "Assumed source-chain signals require a SPRINT duration.";
                emit_report(state, std::move(report));
                return;
            }

            auto signal = std::make_unique<TradeSignal>();
            signal->bridge_id = config.bridge_id;
            signal->symbol = chain.parsed.symbol;
            signal->order_type = chain.parsed.order_type;
            signal->option_type = chain.parsed.option_type;
            signal->duration = chain.parsed.duration;
            signal->signal_name = chain.parsed.signal_name;
            signal->comment = chain.raw.text;
            signal->source_time_ms = current_time_ms();
            signal->amount = config.fixed_amount;
            signal->mm_step = expected_step;
            signal->is_assumed = true;
            signal->assumed_reason = "expected_source_step_missing";
            signal->assumed_source_identity = chain.parsed.source_message_identity;
            signal->assumed_source_step = expected_step;
            const auto dedupe_key = "telegram:assumed:" +
                chain.parsed.source_message_identity + ":" +
                std::to_string(expected_step);
            signal->unique_hash = dedupe_key;
            const auto sequence_key = martingale_key(chain.raw, chain.parsed);
            const auto source_key = source_chain_key(chain.raw, chain.parsed, chain.rule);

            try {
                detail::validate_executable_trade_signal(
                    *signal, "Assumed Telegram source-chain signal", true);
            }
            catch (const std::exception& error) {
                auto report = make_source_chain_report(config, chain, current_time_ms());
                report.status = BridgeSignalReportStatus::INVALID;
                report.reason_code = "assumed_signal_invalid";
                report.message = error.what();
                emit_report(state, std::move(report));
                return;
            }

            BaseBridge::signal_id_allocator_t allocator;
            BaseBridge::trade_signal_callback_t callback;
            std::optional<std::int32_t> previous_step;
            std::string anti_martingale_key;
            bool anti_martingale_pending = false;
            bool martingale_step_recorded = false;
            bool out_of_sequence = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->running ||
                    state->dedupe_keys.find(dedupe_key) != state->dedupe_keys.end()) {
                    out_of_sequence = true;
                }
                else {
                    const auto current = state->source_chain_groups.find(source_key);
                    if (current != state->source_chain_groups.end() &&
                        current->second.parsed.source_message_identity !=
                            chain.parsed.source_message_identity) {
                        out_of_sequence = true;
                    }
                }
                if (!out_of_sequence && config.anti_martingale_enabled) {
                    anti_martingale_key = sequence_key;
                    auto& anti_martingale = state->anti_martingale_groups[
                        anti_martingale_key];
                    if (anti_martingale.pending_trade) {
                        out_of_sequence = true;
                    }
                    else {
                        signal->amount = anti_martingale_amount(
                            config, anti_martingale.next_step);
                        signal->mm_type = MmSystemType::ANTI_MARTINGALE_SIGNAL;
                        signal->mm_step = static_cast<std::int32_t>(
                            anti_martingale.next_step);
                        signal->mm_group_hash = anti_martingale_key;
                        signal->mm_group_name = chain.parsed.signal_name;
                        anti_martingale.pending_trade = true;
                        anti_martingale_pending = true;
                    }
                }
                else if (!out_of_sequence) {
                    const auto existing = state->martingale_steps.find(sequence_key);
                    if (state->pending_martingale_sequences.find(sequence_key) !=
                            state->pending_martingale_sequences.end() ||
                        existing == state->martingale_steps.end() ||
                        existing->second + 1 != expected_step) {
                        out_of_sequence = true;
                    }
                    else {
                        previous_step = existing->second;
                        state->martingale_steps[sequence_key] = expected_step;
                        state->pending_martingale_sequences.insert(sequence_key);
                        martingale_step_recorded = true;
                    }
                }
                if (!out_of_sequence) {
                    // Preserve a fail-closed tombstone before invoking external
                    // callbacks. A late Telegram step must not duplicate this
                    // assumed execution or resume a source series we can no
                    // longer reply-correlate.
                    state->assumed_source_chain_steps[source_key] = expected_step;
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
            if (out_of_sequence) {
                auto report = make_source_chain_report(config, chain, current_time_ms());
                report.reason_code = "assumed_signal_out_of_sequence";
                report.message = "Assumed source-chain signal no longer matches bridge state.";
                emit_report(state, std::move(report));
                return;
            }
            try {
                signal->signal_id = allocator();
                if (signal->signal_id == 0) {
                    throw std::runtime_error("Telegram signal ID allocator returned zero.");
                }
            }
            catch (const std::exception& error) {
                rollback_dispatch_state(
                    state, dedupe_key, sequence_key, martingale_step_recorded, previous_step,
                    anti_martingale_key, anti_martingale_pending);
                rollback_assumed_source_chain_step(state, source_key, expected_step);
                auto report = make_source_chain_report(config, chain, current_time_ms());
                report.status = BridgeSignalReportStatus::INTAKE_ERROR;
                report.reason_code = "assumed_signal_id_allocation_failed";
                report.message = error.what();
                emit_report(state, std::move(report));
                return;
            }
            if (!callback) {
                rollback_dispatch_state(
                    state, dedupe_key, sequence_key, martingale_step_recorded, previous_step,
                    anti_martingale_key, anti_martingale_pending);
                rollback_assumed_source_chain_step(state, source_key, expected_step);
                auto report = make_source_chain_report(config, chain, current_time_ms());
                report.status = BridgeSignalReportStatus::INTAKE_ERROR;
                report.reason_code = "assumed_signal_callback_missing";
                report.message = "Assumed source-chain signal requires a trade signal callback.";
                emit_report(state, std::move(report));
                return;
            }
            if (anti_martingale_pending && !register_anti_martingale_dispatch(
                    state, anti_martingale_key, signal->signal_id)) {
                rollback_dispatch_state(
                    state, dedupe_key, sequence_key, martingale_step_recorded, previous_step,
                    anti_martingale_key, anti_martingale_pending);
                rollback_assumed_source_chain_step(state, source_key, expected_step);
                auto report = make_source_chain_report(config, chain, current_time_ms());
                report.status = BridgeSignalReportStatus::INTAKE_ERROR;
                report.reason_code = "assumed_signal_id_collision";
                report.message = "Assumed source-chain signal requires a unique pending signal ID.";
                emit_report(state, std::move(report));
                return;
            }
            try {
                callback(std::move(signal));
            }
            catch (...) {
                auto report = make_source_chain_report(config, chain, current_time_ms());
                report.status = BridgeSignalReportStatus::INTAKE_ERROR;
                report.reason_code = "ambiguous_assumed_dispatch_failure";
                report.message = "Assumed source-chain signal callback threw after dispatch reservation.";
                emit_report(state, std::move(report));
                return;
            }
            commit_martingale_dispatch_state(
                state, sequence_key, martingale_step_recorded);
        }

        static void source_chain_watchdog_loop(
                const std::shared_ptr<RuntimeState>& state,
                const std::shared_ptr<const TelegramSignalBridgeConfig>& config) {
            std::unique_lock<std::mutex> lock(state->mutex);
            while (!state->source_chain_stop) {
                auto next = std::chrono::steady_clock::time_point::max();
                for (const auto& item : state->source_chain_groups) {
                    if (item.second.waiting && item.second.deadline < next) {
                        next = item.second.deadline;
                    }
                }
                if (next == std::chrono::steady_clock::time_point::max()) {
                    state->source_chain_cv.wait(lock, [&] {
                        return state->source_chain_stop || std::any_of(
                            state->source_chain_groups.begin(), state->source_chain_groups.end(),
                            [](const auto& item) { return item.second.waiting; });
                    });
                    continue;
                }
                if (state->source_chain_cv.wait_until(lock, next) !=
                    std::cv_status::timeout) {
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                std::vector<RuntimeState::SourceChainState> expired;
                for (auto it = state->source_chain_groups.begin();
                     it != state->source_chain_groups.end();) {
                    if (it->second.waiting && it->second.deadline <= now) {
                        state->source_chain_signal_groups.erase(
                            it->second.parsed.source_message_identity);
                        expired.push_back(std::move(it->second));
                        it = state->source_chain_groups.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
                lock.unlock();
                for (const auto& chain : expired) {
                    if (chain.rule.action == TelegramSourceChainAction::EMIT_ASSUMED_SIGNAL) {
                        dispatch_assumed_source_chain_signal(state, *config, chain);
                    }
                    else {
                        emit_report(state, make_source_chain_report(
                            *config, chain, current_time_ms()));
                    }
                }
                lock.lock();
            }
        }

        static void start_source_chain_watchdog(
                const std::shared_ptr<RuntimeState>& state,
                const std::shared_ptr<const TelegramSignalBridgeConfig>& config) {
            if (config->source_chain_rules.empty()) {
                return;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->source_chain_thread.joinable() && !state->source_chain_stop) {
                state->source_chain_thread = std::thread(
                    [state, config] {
                        {
                            std::lock_guard<std::mutex> state_lock(state->mutex);
                            state->source_chain_thread_id = std::this_thread::get_id();
                        }
                        source_chain_watchdog_loop(state, config);
                    });
            }
        }

        static bool join_source_chain_watchdog(
                const std::shared_ptr<RuntimeState>& state,
                const bool notify_stopped) {
            std::thread watchdog;
            bool self_join = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->source_chain_thread.joinable()) {
                    return !state->source_chain_join_pending;
                }
                self_join =
                    state->source_chain_thread.get_id() == std::this_thread::get_id();
                state->source_chain_join_pending = true;
                state->source_chain_thread_id = state->source_chain_thread.get_id();
                watchdog = std::move(state->source_chain_thread);
            }
            if (!watchdog.joinable()) {
                return true;
            }
            if (self_join) {
                std::thread(
                    [state, watchdog = std::move(watchdog), notify_stopped]() mutable {
                        watchdog.join();
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->source_chain_join_pending = false;
                            state->source_chain_thread_id = {};
                        }
                        state->source_chain_cv.notify_all();
                        if (notify_stopped) {
                            notify_status(state, BridgeStatus::SERVER_STOPPED, {});
                        }
                    }).detach();
                return false;
            }
            watchdog.join();
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->source_chain_join_pending = false;
                state->source_chain_thread_id = {};
            }
            state->source_chain_cv.notify_all();
            return true;
        }

        static void process_message(
                const std::shared_ptr<RuntimeState>& state,
                const TelegramSignalBridgeConfig& config,
                const TelegramSignalParser& parser,
                const TelegramRawMessage& raw) {
            std::lock_guard<std::recursive_mutex> intake_lock(state->intake_mutex);
            try {
                raw.validate();
                const auto received_time_ms = current_time_ms();
                const auto parsed = parser.parse(raw);
                process_source_chain_outcomes(state, parsed);
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
                    const auto* source_chain_rule = source_chain_rule_for(config, parsed_signal);
                    const auto source_chain_key_value = source_chain_rule
                        ? source_chain_key(raw, parsed_signal, *source_chain_rule)
                        : std::string();

                    if (config.martingale_policy != TelegramMartingalePolicy::CONTIGUOUS_STEPS &&
                        is_stale(config, raw, received_time_ms)) {
                        emit_report(state, make_stale_signal_report(
                            config, raw, parsed_signal, dedupe_key, received_time_ms));
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
                    std::string anti_martingale_key;
                    bool anti_martingale_pending = false;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        if (!state->running) {
                            return;
                        }
                        if (state->dedupe_keys.find(dedupe_key) != state->dedupe_keys.end()) {
                            duplicate = true;
                        }
                        else if (source_chain_rule && parsed_signal.martingale_step &&
                                 *parsed_signal.martingale_step > 0 &&
                                 state->assumed_source_chain_steps.find(
                                     source_chain_key_value) !=
                                     state->assumed_source_chain_steps.end()) {
                            policy_report = make_signal_report(
                                config, raw, parsed_signal, dedupe_key, received_time_ms,
                                BridgeSignalReportStatus::IGNORED,
                                "source_chain_assumed_step",
                                "Telegram source step follows an assumed continuation and is ignored.");
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
                                 TelegramMartingalePolicy::CONTIGUOUS_STEPS) {
                            if (!parsed_signal.martingale_step) {
                                policy_report = make_signal_report(
                                    config, raw, parsed_signal, dedupe_key, received_time_ms,
                                    BridgeSignalReportStatus::REJECTED,
                                    "martingale_step_missing",
                                    "Telegram martingale policy requires an explicit step.");
                            }
                            else {
                                sequence_key = martingale_key(raw, parsed_signal);
                                const auto existing = state->martingale_steps.find(sequence_key);
                                if (state->pending_martingale_sequences.find(sequence_key) !=
                                    state->pending_martingale_sequences.end()) {
                                    policy_report = make_signal_report(
                                        config, raw, parsed_signal, dedupe_key, received_time_ms,
                                        BridgeSignalReportStatus::REJECTED,
                                        "martingale_step_pending",
                                        "Telegram martingale predecessor has not completed.");
                                }
                                else if (*parsed_signal.martingale_step == 0 &&
                                         is_stale(config, raw, received_time_ms)) {
                                    policy_report = make_stale_signal_report(
                                        config, raw, parsed_signal, dedupe_key, received_time_ms);
                                }
                                else if (*parsed_signal.martingale_step != 0 &&
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
                                    state->martingale_steps[sequence_key] =
                                        *parsed_signal.martingale_step;
                                    state->pending_martingale_sequences.insert(sequence_key);
                                    martingale_step_recorded = true;
                                }
                            }
                        }
                        if (!duplicate && !policy_report && config.anti_martingale_enabled) {
                            anti_martingale_key = martingale_key(raw, parsed_signal);
                            auto& anti_martingale = state->anti_martingale_groups[
                                anti_martingale_key];
                            if (anti_martingale.pending_trade) {
                                policy_report = make_signal_report(
                                    config, raw, parsed_signal, dedupe_key, received_time_ms,
                                    BridgeSignalReportStatus::REJECTED,
                                    "anti_martingale_pending_result",
                                    "Telegram anti-martingale awaits the broker result for this group.");
                            }
                            else {
                                signal->amount = anti_martingale_amount(
                                    config, anti_martingale.next_step);
                                signal->mm_type = MmSystemType::ANTI_MARTINGALE_SIGNAL;
                                signal->mm_step = static_cast<std::int32_t>(
                                    anti_martingale.next_step);
                                signal->mm_group_hash = anti_martingale_key;
                                signal->mm_group_name = parsed_signal.signal_name;
                                anti_martingale.pending_trade = true;
                                anti_martingale_pending = true;
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
                            previous_martingale_step, anti_martingale_key,
                            anti_martingale_pending);
                        emit_report(state, make_signal_report(
                            config, raw, parsed_signal, dedupe_key, received_time_ms,
                            BridgeSignalReportStatus::INTAKE_ERROR,
                            "signal_id_allocation_failed", error.what()));
                        continue;
                    }
                    if (anti_martingale_pending && !callback) {
                        rollback_dispatch_state(
                            state, dedupe_key, sequence_key, martingale_step_recorded,
                            previous_martingale_step, anti_martingale_key,
                            anti_martingale_pending);
                        emit_report(state, make_signal_report(
                            config, raw, parsed_signal, dedupe_key, received_time_ms,
                            BridgeSignalReportStatus::INTAKE_ERROR,
                            "trade_signal_callback_missing",
                            "Telegram anti-martingale requires a trade signal callback."));
                        continue;
                    }
                    if (anti_martingale_pending && !register_anti_martingale_dispatch(
                            state, anti_martingale_key, signal->signal_id)) {
                        rollback_dispatch_state(
                            state, dedupe_key, sequence_key, martingale_step_recorded,
                            previous_martingale_step, anti_martingale_key,
                            anti_martingale_pending);
                        emit_report(state, make_signal_report(
                            config, raw, parsed_signal, dedupe_key, received_time_ms,
                            BridgeSignalReportStatus::INTAKE_ERROR,
                            "signal_id_collision",
                            "Telegram anti-martingale requires unique pending signal IDs."));
                        continue;
                    }
                    if (callback) {
                        try {
                            callback(std::move(signal));
                        }
                        catch (...) {
                            emit_report(state, make_signal_report(
                                config, raw, parsed_signal, dedupe_key, received_time_ms,
                                BridgeSignalReportStatus::INTAKE_ERROR,
                                "ambiguous_dispatch_failure",
                                "Telegram trade signal callback threw after dispatch reservation."));
                            continue;
                        }
                    }
                    // Publish the source generation before releasing the
                    // martingale reservation. An expired older generation can
                    // then never reserve a synthetic continuation for this key.
                    remember_source_chain_signal(state, config, raw, parsed_signal);
                    commit_martingale_dispatch_state(
                        state, sequence_key, martingale_step_recorded);
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
