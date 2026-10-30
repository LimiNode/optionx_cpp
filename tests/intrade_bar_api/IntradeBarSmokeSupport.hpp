#pragma once

#include <optionx_cpp/platforms.hpp>

#include "../common/SmokeTestUtils.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace optionx::tests::intrade_bar_smoke {

using Platform = optionx::platforms::IntradeBarPlatform;
using optionx::tests::smoke::config_value;
using optionx::tests::smoke::getenv_or_empty;
using optionx::tests::smoke::option_double_or;
using optionx::tests::smoke::option_i64_or;
using optionx::tests::smoke::option_signed_int_or;
using optionx::tests::smoke::option_value_or;
using optionx::tests::smoke::opposite_account_type;
using optionx::tests::smoke::opposite_currency;
using optionx::tests::smoke::parse_bool;
using optionx::tests::smoke::parse_double;
using optionx::tests::smoke::parse_enum_or;
using optionx::tests::smoke::parse_i64;
using optionx::tests::smoke::parse_int;
using optionx::tests::smoke::parse_signed_int;
using optionx::tests::smoke::read_env_file;
using optionx::tests::smoke::trim;

struct IntradeBarSmokeConfig {
    std::string email;
    std::string password;
    std::string proxy_server;
    std::string proxy_auth;
    std::string proxy_type = "HTTP";
    std::string host = "https://intrade.bar";
    optionx::AccountType account_type = optionx::AccountType::DEMO;
    optionx::CurrencyType currency = optionx::CurrencyType::USD;
    bool auto_find_domain = false;
    int domain_index_min = 0;
    int domain_index_max = 1000;
    int64_t auth_timeout_ms = 90000;
    int64_t price_timeout_ms = 30000;
    int64_t trade_open_timeout_ms = 45000;
    int64_t trade_result_timeout_ms = time_shield::sec_to_ms(7 * time_shield::SEC_PER_MIN);
    int64_t balance_check_period_ms = time_shield::MS_PER_15_SEC;
    int64_t disconnected_domain_retry_period_ms = time_shield::MS_PER_15_SEC;
    int64_t settings_switch_timeout_ms = 120000;
    int64_t settings_switch_retry_timeout_ms = time_shield::MS_PER_10_MIN;
    int64_t settings_switch_retry_delay_ms = time_shield::MS_PER_15_SEC;
    int64_t settings_switch_active_trade_buffer_ms = time_shield::MS_PER_5_SEC;
    int64_t active_trades_close_buffer_ms = time_shield::MS_PER_SEC;
    int64_t active_trades_sync_period_ms = time_shield::MS_PER_15_SEC;
    int64_t order_interval_ms = 1000;
    optionx::platforms::intrade_bar::TradeHistorySource trade_history_source =
        optionx::platforms::intrade_bar::TradeHistorySource::CSV;
    std::string quote_symbol = "EURUSD";
    std::string trade_symbol = "EURUSD";
    optionx::OrderType trade_order_type = optionx::OrderType::BUY;
    double trade_amount = 1.0;
    int64_t trade_duration_sec = 60;
    bool allow_trade = false;
    bool allow_real_trade = false;

    bool has_credentials() const {
        return !email.empty() && !password.empty();
    }

    bool has_proxy() const {
        return !proxy_server.empty() && !proxy_auth.empty();
    }
};

inline bool require_live_config(
        const IntradeBarSmokeConfig& config,
        std::ostream& err) {
    if (!config.has_credentials()) {
        err << "Missing OPTIONX_INTRADE_BAR_EMAIL/OPTIONX_INTRADE_BAR_PASSWORD.\n";
        return false;
    }
    if (!config.has_proxy()) {
        err << "Refusing to contact broker without proxy settings.\n";
        return false;
    }
    return true;
}

inline void apply_combined_proxy(
        const std::string& combined_proxy,
        IntradeBarSmokeConfig& config) {
    if (combined_proxy.empty()) return;
    std::vector<std::string> parts;
    std::stringstream stream(combined_proxy);
    std::string part;
    while (std::getline(stream, part, ':')) {
        parts.push_back(part);
    }
    if (parts.size() < 4) return;
    config.proxy_server = parts[0] + ":" + parts[1];
    config.proxy_auth = parts[2] + ":" + parts[3];
}

inline IntradeBarSmokeConfig load_config() {
    std::unordered_map<std::string, std::string> file_values;
    const std::string config_file = getenv_or_empty("OPTIONX_INTRADE_BAR_CONFIG_FILE");
    if (!config_file.empty()) {
        file_values = read_env_file(config_file);
    }

    IntradeBarSmokeConfig config;
    config.email = config_value(file_values, "OPTIONX_INTRADE_BAR_EMAIL");
    config.password = config_value(file_values, "OPTIONX_INTRADE_BAR_PASSWORD");
    config.proxy_server = config_value(file_values, "OPTIONX_INTRADE_BAR_PROXY_SERVER");
    config.proxy_auth = config_value(file_values, "OPTIONX_INTRADE_BAR_PROXY_AUTH");
    config.proxy_type = config_value(file_values, "OPTIONX_INTRADE_BAR_PROXY_TYPE", "HTTP");
    config.host = config_value(file_values, "OPTIONX_INTRADE_BAR_HOST", config.host);
    config.account_type = parse_enum_or(
        config_value(file_values, "OPTIONX_INTRADE_BAR_ACCOUNT_TYPE", "DEMO"),
        config.account_type);
    config.currency = parse_enum_or(
        config_value(file_values, "OPTIONX_INTRADE_BAR_CURRENCY", "USD"),
        config.currency);
    config.auto_find_domain = parse_bool(
        config_value(file_values, "OPTIONX_INTRADE_BAR_AUTO_FIND_DOMAIN", "0"),
        false);
    config.domain_index_min = parse_signed_int(
        config_value(file_values, "OPTIONX_INTRADE_BAR_DOMAIN_MIN", "0"),
        0);
    config.domain_index_max = parse_int(
        config_value(file_values, "OPTIONX_INTRADE_BAR_DOMAIN_MAX", "1000"),
        1000);
    config.auth_timeout_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_AUTH_TIMEOUT_MS", "90000"),
        90000);
    config.price_timeout_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_PRICE_TIMEOUT_MS", "30000"),
        30000);
    config.trade_open_timeout_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_TRADE_OPEN_TIMEOUT_MS", "45000"),
        45000);
    config.trade_result_timeout_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_TRADE_RESULT_TIMEOUT_MS", "420000"),
        time_shield::sec_to_ms(7 * time_shield::SEC_PER_MIN));
    config.balance_check_period_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_BALANCE_CHECK_PERIOD_MS", "15000"),
        time_shield::MS_PER_15_SEC);
    config.disconnected_domain_retry_period_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_DISCONNECTED_DOMAIN_RETRY_PERIOD_MS", "15000"),
        time_shield::MS_PER_15_SEC);
    config.settings_switch_timeout_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_SETTINGS_SWITCH_TIMEOUT_MS", "120000"),
        120000);
    config.settings_switch_retry_timeout_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_SETTINGS_SWITCH_RETRY_TIMEOUT_MS", "600000"),
        time_shield::MS_PER_10_MIN);
    config.settings_switch_retry_delay_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_SETTINGS_SWITCH_RETRY_DELAY_MS", "15000"),
        time_shield::MS_PER_15_SEC);
    config.settings_switch_active_trade_buffer_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_SETTINGS_SWITCH_ACTIVE_TRADE_BUFFER_MS", "5000"),
        time_shield::MS_PER_5_SEC);
    config.active_trades_close_buffer_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_ACTIVE_TRADES_CLOSE_BUFFER_MS", "1000"),
        time_shield::MS_PER_SEC);
    config.active_trades_sync_period_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_ACTIVE_TRADES_SYNC_PERIOD_MS", "15000"),
        time_shield::MS_PER_15_SEC);
    config.order_interval_ms = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_ORDER_INTERVAL_MS", "1000"),
        1000);
    config.trade_history_source =
        optionx::platforms::intrade_bar::trade_history_source_from_string(
            config_value(file_values, "OPTIONX_INTRADE_BAR_TRADE_HISTORY_SOURCE", "CSV"),
            config.trade_history_source);
    config.quote_symbol = config_value(
        file_values,
        "OPTIONX_INTRADE_BAR_QUOTE_SYMBOL",
        config.quote_symbol);
    config.trade_symbol = config_value(
        file_values,
        "OPTIONX_INTRADE_BAR_TRADE_SYMBOL",
        config.trade_symbol);
    config.trade_order_type = parse_enum_or(
        config_value(file_values, "OPTIONX_INTRADE_BAR_TRADE_ORDER_TYPE", "BUY"),
        config.trade_order_type);
    config.trade_amount = parse_double(
        config_value(file_values, "OPTIONX_INTRADE_BAR_TRADE_AMOUNT", "1.0"),
        1.0);
    config.trade_duration_sec = parse_i64(
        config_value(file_values, "OPTIONX_INTRADE_BAR_TRADE_DURATION_SEC", "60"),
        60);
    config.allow_trade = parse_bool(
        config_value(file_values, "OPTIONX_INTRADE_BAR_ALLOW_TRADE", "0"),
        false);
    config.allow_real_trade = parse_bool(
        config_value(file_values, "OPTIONX_INTRADE_BAR_ALLOW_REAL_TRADE", "0"),
        false);

    apply_combined_proxy(
        config_value(file_values, "OPTIONX_INTRADE_BAR_PROXY"),
        config);
    return config;
}

inline std::unique_ptr<optionx::platforms::intrade_bar::AuthData> make_auth_data(
        const IntradeBarSmokeConfig& config) {
    auto auth_data = std::make_unique<optionx::platforms::intrade_bar::AuthData>();
    auth_data->set_email_password(config.email, config.password);
    auth_data->host = config.host;
    auth_data->account_type = config.account_type;
    auth_data->currency = config.currency;
    auth_data->auto_find_domain = config.auto_find_domain;
    auth_data->domain_index_min = config.domain_index_min;
    auth_data->domain_index_max = config.domain_index_max;
    auth_data->balance_check_period_ms = config.balance_check_period_ms;
    auth_data->disconnected_domain_retry_period_ms =
        config.disconnected_domain_retry_period_ms;
    auth_data->settings_switch_retry_timeout_ms = config.settings_switch_retry_timeout_ms;
    auth_data->settings_switch_retry_delay_ms = config.settings_switch_retry_delay_ms;
    auth_data->settings_switch_active_trade_buffer_ms =
        config.settings_switch_active_trade_buffer_ms;
    auth_data->active_trades_close_buffer_ms =
        config.active_trades_close_buffer_ms;
    auth_data->active_trades_sync_period_ms =
        config.active_trades_sync_period_ms;
    auth_data->order_interval_ms = config.order_interval_ms;
    auth_data->trade_history_source = config.trade_history_source;
    auth_data->proxy_server = config.proxy_server;
    auth_data->proxy_auth = config.proxy_auth;
    auth_data->proxy_type = parse_enum_or<kurlyk::ProxyType>(
        config.proxy_type,
        kurlyk::ProxyType::PROXY_HTTP);
    return auth_data;
}

inline std::string normalize_quote_symbol(std::string symbol) {
    return optionx::platforms::intrade_bar::normalize_symbol_name(std::move(symbol));
}

template<class Predicate>
bool pump_until(
        Platform& platform,
        Predicate predicate,
        int64_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        platform.process();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    platform.process();
    return predicate();
}

struct AutoDomainSelection {
    bool received = false;
    bool success = false;
    std::string selected_host;
};

class PriceUpdateCapture : public optionx::utils::EventMediator {
public:
    explicit PriceUpdateCapture(optionx::utils::EventBus& bus)
        : optionx::utils::EventMediator(bus) {
        subscribe<optionx::events::PriceUpdateEvent>(
            [this](const optionx::events::PriceUpdateEvent& event) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_batches = event.get_tick_batches();
                ++m_update_count;
                LOGIT_INFO("Intrade Bar smoke price update: batches=", m_batches.size());
            });
    }

    void on_event(const optionx::utils::Event* const) override {
    }

    std::vector<optionx::SingleTick> ticks() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<optionx::SingleTick> ticks;
        for (const auto& batch : m_batches) {
            for (const auto& item : batch.items) {
                ticks.emplace_back(
                    item,
                    batch.symbol,
                    batch.provider,
                    batch.price_digits,
                    batch.volume_digits);
            }
        }
        return ticks;
    }

    std::size_t update_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_update_count;
    }

    bool has_symbol(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& batch : m_batches) {
            if (batch.symbol == symbol && !batch.empty()) return true;
        }
        return false;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<optionx::events::TickUpdateBatch> m_batches;
    std::size_t m_update_count = 0;
};

class AutoDomainSelectionCapture : public optionx::utils::EventMediator {
public:
    explicit AutoDomainSelectionCapture(optionx::utils::EventBus& bus)
        : optionx::utils::EventMediator(bus) {
        subscribe<optionx::events::AutoDomainSelectedEvent>(
            [this](const optionx::events::AutoDomainSelectedEvent& event) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_selection.received = true;
                m_selection.success = event.success;
                m_selection.selected_host = event.selected_host;
                LOGIT_INFO(
                    "Intrade Bar smoke auto domain selected: success=",
                    event.success,
                    ", host=",
                    event.selected_host);
            });
    }

    void on_event(const optionx::utils::Event* const) override {
    }

    AutoDomainSelection selection() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_selection;
    }

private:
    mutable std::mutex m_mutex;
    AutoDomainSelection m_selection;
};

struct ConnectAttempt {
    bool callback_received = false;
    bool success = false;
    std::string reason;
    int64_t elapsed_ms = 0;
};

struct TickHistoryAttempt {
    bool accepted = false;
    bool callback_received = false;
    optionx::TickHistoryResult result;
    int64_t elapsed_ms = 0;
};

struct TradeOpenAttempt {
    bool accepted = false;
    bool callback_received = false;
    optionx::TradeState state = optionx::TradeState::UNKNOWN;
    std::string error_desc;
    int64_t option_id = 0;
    int64_t open_date = 0;
    int64_t close_date = 0;
    double open_price = 0.0;
    int64_t elapsed_ms = 0;
};

struct TradeLifecycleAttempt {
    bool accepted = false;
    bool callback_received = false;
    bool open_received = false;
    bool terminal_received = false;
    optionx::TradeResult result;
    std::string error_desc;
    int64_t elapsed_ms = 0;
};

struct TradeResultFetchAttempt {
    bool accepted = false;
    bool callback_received = false;
    optionx::TradeResult result;
    int64_t elapsed_ms = 0;
};

struct TradeHistoryFetchAttempt {
    bool accepted = false;
    bool callback_received = false;
    bool success = false;
    long status_code = optionx::TradeHistoryResult::NO_RESPONSE_STATUS;
    std::string error_desc;
    std::vector<optionx::TradeRecord> records;
    int64_t elapsed_ms = 0;
};

class IntradeBarSmokeRuntime {
public:
    explicit IntradeBarSmokeRuntime(IntradeBarSmokeConfig config)
        : m_config(std::move(config)),
          m_price_capture(m_platform.event_bus()),
          m_domain_capture(m_platform.event_bus()) {
        m_platform.on_account_info() = [this](const optionx::AccountInfoUpdate& update) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_last_account = update.account_info;
            ++m_account_update_count;
            if (update.status == optionx::AccountUpdateStatus::OPEN_TRADES_CHANGED &&
                update.account_info) {
                const int64_t open_trades = update.account_info->get_info<int64_t>(
                    optionx::AccountInfoType::OPEN_TRADES);
                m_open_trades_updates.push_back(open_trades);
                LOGIT_INFO(
                    "Intrade Bar open trades update: open_trades=",
                    open_trades,
                    ", updates=",
                    m_open_trades_updates.size());
            }
            LOGIT_INFO(
                "Intrade Bar account update: status=",
                optionx::to_str(update.status),
                ", message=",
                update.message);
        };
    }

    ~IntradeBarSmokeRuntime() {
        m_platform.shutdown();
    }

    Platform& platform() {
        return m_platform;
    }

    const IntradeBarSmokeConfig& config() const {
        return m_config;
    }

    bool start() {
        if (m_started) return true;
        LOGIT_INFO("Intrade Bar smoke runtime starting. host=", m_config.host);
        m_platform.run(false);
        m_started = m_platform.configure_auth(make_auth_data(m_config));
        return m_started;
    }

    ConnectAttempt connect() {
        return connect(m_config.auth_timeout_ms);
    }

    ConnectAttempt connect(int64_t timeout_ms) {
        LOGIT_SCOPE_INFO("intradebar.smoke.connect");
        ConnectAttempt attempt;
        if (!start()) {
            attempt.reason = "Failed to configure auth.";
            return attempt;
        }

        std::atomic<bool> done{false};
        const auto started_at = std::chrono::steady_clock::now();
        m_platform.connect([&](optionx::ConnectionResult result) {
            attempt.success = result.success;
            attempt.reason = std::move(result.reason);
            attempt.callback_received = true;
            done = true;
            LOGIT_INFO(
                "Intrade Bar smoke connect callback: success=",
                attempt.success,
                ", reason=",
                attempt.reason);
        });

        pump_until(m_platform, [&] { return done.load(); }, timeout_ms);
        if (attempt.success) {
            pump_until(m_platform, [&] { return m_platform.is_connected(); }, 1000);
        }
        attempt.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at).count();
        return attempt;
    }

    ConnectAttempt disconnect(int64_t timeout_ms = 10000) {
        LOGIT_SCOPE_INFO("intradebar.smoke.disconnect");
        ConnectAttempt attempt;
        std::atomic<bool> done{false};
        const auto started_at = std::chrono::steady_clock::now();
        m_platform.disconnect([&](optionx::ConnectionResult result) {
            attempt.success = result.success;
            attempt.reason = std::move(result.reason);
            attempt.callback_received = true;
            done = true;
            LOGIT_INFO(
                "Intrade Bar smoke disconnect callback: success=",
                attempt.success,
                ", reason=",
                attempt.reason);
        });
        pump_until(m_platform, [&] { return done.load(); }, timeout_ms);
        attempt.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at).count();
        return attempt;
    }

    bool wait_for_price_update(std::string symbol = {}, int64_t timeout_ms = 0) {
        LOGIT_SCOPE_INFO("intradebar.smoke.price");
        if (timeout_ms <= 0) timeout_ms = m_config.price_timeout_ms;
        symbol = normalize_quote_symbol(std::move(symbol));
        const std::size_t initial_count = m_price_capture.update_count();
        return pump_until(
            m_platform,
            [&] {
                if (m_price_capture.update_count() <= initial_count) return false;
                return symbol.empty() || m_price_capture.has_symbol(symbol);
            },
            timeout_ms);
    }

    std::vector<optionx::SingleTick> latest_ticks() const {
        return m_price_capture.ticks();
    }

    TickHistoryAttempt fetch_tick_history(
            optionx::TickHistoryRequest request,
            int64_t timeout_ms) {
        TickHistoryAttempt attempt;
        std::atomic<bool> done{false};
        const auto started_at = std::chrono::steady_clock::now();
        attempt.accepted = m_platform.fetch_tick_history(
            request,
            [&](optionx::TickHistoryResult result) {
                attempt.result = std::move(result);
                attempt.callback_received = true;
                done.store(true, std::memory_order_release);
            });
        pump_until(
            m_platform,
            [&] { return done.load(std::memory_order_acquire); },
            timeout_ms);
        attempt.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at).count();
        return attempt;
    }

    AutoDomainSelection latest_domain_selection() const {
        return m_domain_capture.selection();
    }

    std::shared_ptr<optionx::BaseAccountInfoData> last_account() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_account;
    }

    std::size_t account_update_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_account_update_count;
    }

    std::size_t open_trades_update_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_open_trades_updates.size();
    }

    int64_t latest_open_trades_update() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_open_trades_updates.empty() ? -1 : m_open_trades_updates.back();
    }

    std::vector<int64_t> open_trades_updates() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_open_trades_updates;
    }

    int64_t current_open_trades() {
        return m_platform.get_info<int64_t>(optionx::AccountInfoType::OPEN_TRADES);
    }

    void pump_for(int64_t timeout_ms) {
        pump_until(
            m_platform,
            [] {
                return false;
            },
            timeout_ms);
    }

    bool has_account_settings(
            optionx::AccountType account_type,
            optionx::CurrencyType currency) {
        return m_platform.get_info<optionx::AccountType>(optionx::AccountInfoType::ACCOUNT_TYPE) == account_type &&
            m_platform.get_info<optionx::CurrencyType>(optionx::AccountInfoType::CURRENCY) == currency;
    }

    bool wait_for_account_settings_after(
            optionx::AccountType account_type,
            optionx::CurrencyType currency,
            std::size_t previous_update_count,
            int64_t timeout_ms) {
        return pump_until(
            m_platform,
            [&] {
                return account_update_count() > previous_update_count &&
                    has_account_settings(account_type, currency);
            },
            timeout_ms);
    }

    bool wait_for_open_trades_update_after(
            std::size_t previous_update_count,
            int64_t timeout_ms) {
        return pump_until(
            m_platform,
            [&] {
                return open_trades_update_count() > previous_update_count;
            },
            timeout_ms);
    }

    bool wait_for_open_trades_at_least_after(
            int64_t expected_open_trades,
            std::size_t previous_update_count,
            int64_t timeout_ms) {
        return pump_until(
            m_platform,
            [&] {
                return open_trades_update_count() > previous_update_count &&
                    latest_open_trades_update() >= expected_open_trades;
            },
            timeout_ms);
    }

    bool wait_for_open_trades_below_after(
            int64_t previous_open_trades,
            std::size_t previous_update_count,
            int64_t timeout_ms) {
        return pump_until(
            m_platform,
            [&] {
                return open_trades_update_count() > previous_update_count &&
                    latest_open_trades_update() < previous_open_trades;
            },
            timeout_ms);
    }

    void request_balance_refresh() {
        LOGIT_INFO("Intrade Bar smoke requesting one balance refresh.");
        m_platform.event_bus().notify_async(
            std::make_unique<optionx::events::BalanceRequestEvent>());
    }

    TradeOpenAttempt open_trade_and_wait(
            const std::string& symbol,
            double amount,
            optionx::OrderType order_type,
            std::uint32_t duration_sec,
            int64_t timeout_ms) {
        LOGIT_SCOPE_INFO("intradebar.smoke.open_trade");
        struct SharedAttempt {
            mutable std::mutex mutex;
            TradeOpenAttempt attempt;
            std::atomic<bool> done{false};
        };

        auto shared = std::make_shared<SharedAttempt>();
        const auto started_at = std::chrono::steady_clock::now();
        const auto elapsed_ms = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at).count();
        };
        const auto snapshot = [&] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->done.store(true, std::memory_order_release);
            shared->attempt.elapsed_ms = elapsed_ms();
            return shared->attempt;
        };

        auto request = std::make_unique<optionx::TradeRequest>();
        request->symbol = symbol;
        request->amount = amount;
        request->option_type = optionx::OptionType::SPRINT;
        request->order_type = order_type;
        request->duration = duration_sec;
        request->account_type = m_config.account_type;
        request->currency = m_config.currency;
        request->comment = "optionx_cpp intrade_bar smoke cli";
        request->add_callback([shared](
                std::unique_ptr<optionx::TradeRequest>,
                std::unique_ptr<optionx::TradeResult> result) {
            const bool opens_finished =
                result->trade_state == optionx::TradeState::OPEN_SUCCESS ||
                result->trade_state == optionx::TradeState::OPEN_ERROR;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                if (!shared->done.load(std::memory_order_acquire)) {
                    shared->attempt.callback_received = true;
                    shared->attempt.state = result->trade_state;
                    shared->attempt.error_desc = result->error_desc;
                    shared->attempt.option_id = result->option_id;
                    shared->attempt.open_date = result->open_date;
                    shared->attempt.close_date = result->close_date;
                    shared->attempt.open_price = result->open_price;
                    if (opens_finished) {
                        shared->done.store(true, std::memory_order_release);
                    }
                }
            }
            LOGIT_INFO(
                "Intrade Bar smoke trade callback: state=",
                optionx::to_str(result->trade_state),
                ", option_id=",
                result->option_id,
                ", error=",
                result->error_desc);
        });

        const bool accepted = m_platform.place_trade(std::move(request));
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->attempt.accepted = accepted;
            if (!accepted) {
                shared->attempt.error_desc = "Platform refused trade request before sending.";
            }
        }
        if (!accepted) {
            return snapshot();
        }

        pump_until(
            m_platform,
            [&] { return shared->done.load(std::memory_order_acquire); },
            timeout_ms);
        return snapshot();
    }

    TradeLifecycleAttempt open_trade_and_wait_for_result(
            const std::string& symbol,
            double amount,
            optionx::OrderType order_type,
            std::uint32_t duration_sec,
            int64_t timeout_ms) {
        LOGIT_SCOPE_INFO("intradebar.smoke.open_trade_wait_result");
        struct SharedAttempt {
            mutable std::mutex mutex;
            TradeLifecycleAttempt attempt;
            std::atomic<bool> done{false};
        };

        auto shared = std::make_shared<SharedAttempt>();
        const auto started_at = std::chrono::steady_clock::now();
        const auto elapsed_ms = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at).count();
        };
        const auto snapshot = [&] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->attempt.elapsed_ms = elapsed_ms();
            return shared->attempt;
        };

        auto request = std::make_unique<optionx::TradeRequest>();
        request->symbol = symbol;
        request->amount = amount;
        request->option_type = optionx::OptionType::SPRINT;
        request->order_type = order_type;
        request->duration = duration_sec;
        request->account_type = m_config.account_type;
        request->currency = m_config.currency;
        request->comment = "optionx_cpp intrade_bar smoke cli result check";
        request->add_callback([shared](
                std::unique_ptr<optionx::TradeRequest>,
                std::unique_ptr<optionx::TradeResult> result) {
            if (!result) return;
            const bool open_finished =
                result->trade_state == optionx::TradeState::OPEN_SUCCESS ||
                result->trade_state == optionx::TradeState::OPEN_ERROR;
            const bool terminal = optionx::is_terminal_trade_state(result->trade_state);
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->attempt.callback_received = true;
                shared->attempt.result = *result;
                shared->attempt.error_desc = result->error_desc;
                if (open_finished) {
                    shared->attempt.open_received = true;
                }
                if (terminal) {
                    shared->attempt.terminal_received = true;
                    shared->done.store(true, std::memory_order_release);
                }
            }
            LOGIT_INFO(
                "Intrade Bar smoke lifecycle callback: state=",
                optionx::to_str(result->trade_state),
                ", option_id=",
                result->option_id,
                ", profit=",
                result->profit,
                ", error=",
                result->error_desc);
        });

        const bool accepted = m_platform.place_trade(std::move(request));
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->attempt.accepted = accepted;
            if (!accepted) {
                shared->attempt.error_desc = "Platform refused trade request before sending.";
                shared->done.store(true, std::memory_order_release);
            }
        }
        if (!accepted) return snapshot();

        pump_until(
            m_platform,
            [&] { return shared->done.load(std::memory_order_acquire); },
            timeout_ms);
        return snapshot();
    }

    TradeResultFetchAttempt fetch_trade_result_and_wait(
            optionx::TradeResultQuery query,
            std::unique_ptr<optionx::TradeResult> result,
            int64_t timeout_ms) {
        LOGIT_SCOPE_INFO("intradebar.smoke.fetch_trade_result");
        struct SharedAttempt {
            mutable std::mutex mutex;
            TradeResultFetchAttempt attempt;
            std::atomic<bool> done{false};
        };

        auto shared = std::make_shared<SharedAttempt>();
        const auto started_at = std::chrono::steady_clock::now();
        const auto elapsed_ms = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at).count();
        };

        const bool accepted = m_platform.fetch_trade_result(
            std::move(query),
            std::move(result),
            [shared](std::unique_ptr<optionx::TradeResult> fetched_result) {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->attempt.callback_received = true;
                if (fetched_result) {
                    shared->attempt.result = *fetched_result;
                }
                shared->done.store(true, std::memory_order_release);
                LOGIT_INFO(
                    "Intrade Bar smoke fetch result callback: state=",
                    optionx::to_str(shared->attempt.result.trade_state),
                    ", option_id=",
                    shared->attempt.result.option_id,
                    ", profit=",
                    shared->attempt.result.profit,
                    ", error=",
                    shared->attempt.result.error_desc);
            });
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->attempt.accepted = accepted;
            if (!accepted) {
                shared->done.store(true, std::memory_order_release);
            }
        }

        pump_until(
            m_platform,
            [&] { return shared->done.load(std::memory_order_acquire); },
            timeout_ms);

        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->attempt.elapsed_ms = elapsed_ms();
        return shared->attempt;
    }

    TradeHistoryFetchAttempt fetch_trade_history_and_wait(
            optionx::TradeHistoryRequest request,
            int64_t timeout_ms) {
        LOGIT_SCOPE_INFO("intradebar.smoke.fetch_trade_history");
        struct SharedAttempt {
            mutable std::mutex mutex;
            TradeHistoryFetchAttempt attempt;
            std::atomic<bool> done{false};
        };

        auto shared = std::make_shared<SharedAttempt>();
        const auto started_at = std::chrono::steady_clock::now();
        const auto elapsed_ms = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at).count();
        };

        const bool accepted = m_platform.fetch_trade_history(
            request,
            [shared](optionx::TradeHistoryResult result) {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->attempt.callback_received = true;
                shared->attempt.success = result.success;
                shared->attempt.status_code = result.status_code;
                shared->attempt.error_desc = std::move(result.error_desc);
                shared->attempt.records = std::move(result.records);
                shared->done.store(true, std::memory_order_release);
                LOGIT_INFO(
                    "Intrade Bar smoke history callback: success=",
                    shared->attempt.success,
                    ", status_code=",
                    shared->attempt.status_code,
                    ", error=",
                    shared->attempt.error_desc,
                    ", records=",
                    shared->attempt.records.size());
                for (const auto& record : shared->attempt.records) {
                    LOGIT_INFO(
                        "Intrade Bar smoke history record: option_id=",
                        record.option_id,
                        ", symbol=",
                        record.symbol,
                        ", state=",
                        optionx::to_str(record.trade_state),
                        ", amount=",
                        record.amount,
                        ", profit=",
                        record.profit,
                        ", open_date=",
                        record.open_date,
                        ", close_date=",
                        record.close_date);
                }
            });
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->attempt.accepted = accepted;
            if (!accepted) {
                shared->done.store(true, std::memory_order_release);
            }
        }

        pump_until(
            m_platform,
            [&] { return shared->done.load(std::memory_order_acquire); },
            timeout_ms);

        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->attempt.elapsed_ms = elapsed_ms();
        return shared->attempt;
    }

private:
    IntradeBarSmokeConfig m_config;
    Platform m_platform;
    PriceUpdateCapture m_price_capture;
    AutoDomainSelectionCapture m_domain_capture;
    mutable std::mutex m_mutex;
    std::shared_ptr<optionx::BaseAccountInfoData> m_last_account;
    std::size_t m_account_update_count = 0;
    std::vector<int64_t> m_open_trades_updates;
    bool m_started = false;
};

inline bool connect_or_report(
        IntradeBarSmokeRuntime& runtime,
        std::ostream& out,
        std::ostream& err) {
    const auto connect = runtime.connect();
    out << "auth callback=" << connect.callback_received
        << " success=" << connect.success
        << " elapsed_ms=" << connect.elapsed_ms << '\n';
    if (!connect.success) {
        err << "auth failed: " << connect.reason << '\n';
        return false;
    }
    return true;
}

inline bool remove_saved_session(const IntradeBarSmokeConfig& config) {
    return optionx::storage::ServiceSessionDB::get_instance().remove_session(
        optionx::to_str(optionx::PlatformType::INTRADE_BAR),
        config.email);
}

inline std::optional<std::string> saved_session(const IntradeBarSmokeConfig& config) {
    return optionx::storage::ServiceSessionDB::get_instance().get_session_value(
        optionx::to_str(optionx::PlatformType::INTRADE_BAR),
        config.email);
}

inline void print_account(std::ostream& out, Platform& platform) {
    out << "connected=" << platform.is_connected()
        << " account_type=" << optionx::to_str(platform.get_info<optionx::AccountType>(optionx::AccountInfoType::ACCOUNT_TYPE))
        << " currency=" << optionx::to_str(platform.get_info<optionx::CurrencyType>(optionx::AccountInfoType::CURRENCY))
        << " balance=" << platform.get_info<double>(optionx::AccountInfoType::BALANCE)
        << " open_trades=" << platform.get_info<int64_t>(optionx::AccountInfoType::OPEN_TRADES)
        << '\n';
}

} // namespace optionx::tests::intrade_bar_smoke
