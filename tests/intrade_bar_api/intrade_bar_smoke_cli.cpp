#define OPTIONX_LOG_UNIQUE_FILE_INDEX 2

#include "IntradeBarSmokeSupport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace smoke = optionx::tests::intrade_bar_smoke;

namespace {

struct CliOptions {
    std::string command = "help";
    std::map<std::string, std::string> values;
    bool confirm = false;
    bool allow_real = false;
};

void print_usage(std::ostream& out) {
    out
        << "Usage:\n"
        << "  intrade_bar_smoke_cli auth\n"
        << "  intrade_bar_smoke_cli auth-cache\n"
        << "  intrade_bar_smoke_cli show-account\n"
        << "  intrade_bar_smoke_cli domain-check [--domain-min=-1] [--domain-max=1000]\n"
        << "  intrade_bar_smoke_cli quotes [--symbol=EURUSD]\n"
        << "  intrade_bar_smoke_cli market-stream [--symbols=BTCUSDT,EURUSD] [--ticks] [--bars] [--transport=WEBSOCKET|POLLING|AUTO|HYBRID] [--seconds=30] [--timeframe=60] [--backfill-minutes=30]\n"
        << "  intrade_bar_smoke_cli history [--source=CSV|HTML|HTML_CSV] [--days=14|--all] [--time-field=CLOSE_DATE] [--comment=...]\n"
        << "  intrade_bar_smoke_cli switch-check --confirm [--account-type=DEMO] [--currency=USD]\n"
        << "  intrade_bar_smoke_cli open-trade --confirm [--symbol=EURUSD] [--amount=1] [--duration=60] [--buy|--sell]\n"
        << "  intrade_bar_smoke_cli open-trades-sync-check --confirm [--symbol=BTCUSDT|EURUSD] [--amount=1] [--duration=300] [--count=1] [--interval-ms=15000] [--order-interval-ms=1000] [--buy|--sell]\n"
        << "  intrade_bar_smoke_cli open-check-result --confirm [--symbol=BTCUSDT] [--amount=1] [--duration=300] [--buy|--sell]\n"
        << "\n"
        << "Configuration comes from environment variables or OPTIONX_INTRADE_BAR_CONFIG_FILE.\n"
        << "Credentials always require proxy settings before any broker request is sent.\n";
}

CliOptions parse_args(int argc, char** argv) {
    CliOptions options;
    if (argc >= 2) options.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--confirm") {
            options.confirm = true;
        } else if (arg == "--allow-real") {
            options.allow_real = true;
        } else if (arg == "--all") {
            options.values["all"] = "1";
        } else if (arg == "--buy") {
            options.values["order"] = "BUY";
        } else if (arg == "--sell") {
            options.values["order"] = "SELL";
        } else if (arg == "--ticks") {
            options.values["ticks"] = "1";
        } else if (arg == "--bars") {
            options.values["bars"] = "1";
        } else if (arg == "--no-backfill") {
            options.values["backfill"] = "0";
        } else if (arg.rfind("--", 0) == 0) {
            const auto eq = arg.find('=');
            if (eq != std::string::npos) {
                options.values[arg.substr(2, eq - 2)] = arg.substr(eq + 1);
            }
        }
    }
    return options;
}

std::string upper_ascii(std::string value) {
    value = optionx::utils::trim_copy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::vector<std::string> split_csv_symbols(const std::string& value) {
    std::vector<std::string> symbols;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = optionx::utils::trim_copy(std::move(item));
        if (!item.empty()) {
            symbols.push_back(smoke::normalize_quote_symbol(item));
        }
    }
    return symbols;
}

double price_from_tick(
        const optionx::Tick& tick,
        optionx::BarPriceSource price_source) {
    switch (price_source) {
    case optionx::BarPriceSource::BID:
        return tick.bid;
    case optionx::BarPriceSource::ASK:
        return tick.ask;
    case optionx::BarPriceSource::LAST:
        return tick.mid_price();
    case optionx::BarPriceSource::MID:
    case optionx::BarPriceSource::UNKNOWN:
    default:
        return tick.mid_price();
    }
}

const char* trade_history_time_field_to_string(optionx::TradeRecordTimeField value) noexcept {
    switch (value) {
    case optionx::TradeRecordTimeField::PLACE_DATE:
        return "PLACE_DATE";
    case optionx::TradeRecordTimeField::SEND_DATE:
        return "SEND_DATE";
    case optionx::TradeRecordTimeField::OPEN_DATE:
        return "OPEN_DATE";
    case optionx::TradeRecordTimeField::CLOSE_DATE:
        return "CLOSE_DATE";
    case optionx::TradeRecordTimeField::AUTO:
    default:
        return "AUTO";
    }
}

optionx::TradeRecordTimeField parse_trade_history_time_field(
        const std::string& value,
        optionx::TradeRecordTimeField fallback) {
    const std::string normalized = upper_ascii(value);
    if (normalized.empty()) return fallback;
    if (normalized == "AUTO") return optionx::TradeRecordTimeField::AUTO;
    if (normalized == "PLACE_DATE" || normalized == "PLACE") return optionx::TradeRecordTimeField::PLACE_DATE;
    if (normalized == "SEND_DATE" || normalized == "SEND") return optionx::TradeRecordTimeField::SEND_DATE;
    if (normalized == "OPEN_DATE" || normalized == "OPEN") return optionx::TradeRecordTimeField::OPEN_DATE;
    if (normalized == "CLOSE_DATE" || normalized == "CLOSE") return optionx::TradeRecordTimeField::CLOSE_DATE;
    return fallback;
}

const char* time_range_mode_to_string(optionx::TimeRangeMode value) noexcept {
    switch (value) {
    case optionx::TimeRangeMode::NONE:
        return "NONE";
    case optionx::TimeRangeMode::HALF_OPEN:
        return "HALF_OPEN";
    case optionx::TimeRangeMode::CLOSED:
    default:
        return "CLOSED";
    }
}

optionx::TimeRangeMode parse_time_range_mode(
        const std::string& value,
        optionx::TimeRangeMode fallback) {
    const std::string normalized = upper_ascii(value);
    if (normalized.empty()) return fallback;
    if (normalized == "NONE" || normalized == "ALL") return optionx::TimeRangeMode::NONE;
    if (normalized == "HALF_OPEN" || normalized == "HALF-OPEN") return optionx::TimeRangeMode::HALF_OPEN;
    if (normalized == "CLOSED") return optionx::TimeRangeMode::CLOSED;
    return fallback;
}

bool restore_settings_direct(
        const smoke::IntradeBarSmokeConfig& config,
        std::ostream& out,
        std::ostream& err) {
    out << "restore_direct=1\n";
    smoke::IntradeBarSmokeRuntime runtime(config);
    if (!smoke::connect_or_report(runtime, out, err)) return false;
    const bool restored = runtime.has_account_settings(
        config.account_type,
        config.currency);
    out << "restore_ok=" << restored << ' ';
    smoke::print_account(out, runtime.platform());
    runtime.disconnect();
    return restored;
}

int run_auth(smoke::IntradeBarSmokeConfig config) {
    if (!smoke::require_live_config(config, std::cerr)) return 2;
    smoke::IntradeBarSmokeRuntime runtime(std::move(config));
    if (!smoke::connect_or_report(runtime, std::cout, std::cerr)) return 1;
    smoke::print_account(std::cout, runtime.platform());
    runtime.disconnect();
    return 0;
}

int run_auth_cache(smoke::IntradeBarSmokeConfig config) {
    if (!smoke::require_live_config(config, std::cerr)) return 2;
    smoke::remove_saved_session(config);

    smoke::ConnectAttempt fresh;
    {
        smoke::IntradeBarSmokeRuntime runtime(config);
        fresh = runtime.connect();
        runtime.disconnect();
    }
    if (!fresh.success) {
        std::cerr << "fresh auth failed: " << fresh.reason << '\n';
        return 1;
    }

    const bool session_saved = smoke::saved_session(config).has_value();
    smoke::ConnectAttempt cached;
    {
        smoke::IntradeBarSmokeRuntime runtime(config);
        cached = runtime.connect();
        runtime.disconnect();
    }
    if (!cached.success) {
        std::cerr << "cached auth failed: " << cached.reason << '\n';
        return 1;
    }

    std::cout << "fresh_ms=" << fresh.elapsed_ms
              << " cached_ms=" << cached.elapsed_ms
              << " session_saved=" << session_saved
              << " faster=" << (cached.elapsed_ms < fresh.elapsed_ms)
              << '\n';
    return cached.elapsed_ms < fresh.elapsed_ms ? 0 : 3;
}

int run_domain_check(smoke::IntradeBarSmokeConfig config, const CliOptions& options) {
    if (!smoke::require_live_config(config, std::cerr)) return 2;

    config.auto_find_domain = true;
    config.domain_index_min = smoke::option_signed_int_or(
        options.values,
        "domain-min",
        config.domain_index_min);
    config.domain_index_max = static_cast<int>(smoke::option_i64_or(
        options.values,
        "domain-max",
        config.domain_index_max));
    const int64_t timeout_ms = smoke::option_i64_or(
        options.values,
        "timeout-ms",
        config.auth_timeout_ms);

    std::cout << "domain_check auto_find_domain=" << config.auto_find_domain
              << " domain_min=" << config.domain_index_min
              << " domain_max=" << config.domain_index_max
              << " timeout_ms=" << timeout_ms << '\n';

    smoke::IntradeBarSmokeRuntime runtime(config);
    const auto connect = runtime.connect(timeout_ms);
    std::cout << "auth callback=" << connect.callback_received
              << " success=" << connect.success
              << " elapsed_ms=" << connect.elapsed_ms << '\n';

    const auto selection = runtime.latest_domain_selection();
    std::cout << "domain selected=" << selection.received
              << " success=" << selection.success
              << " host=" << selection.selected_host << '\n';

    if (!connect.success) {
        std::cerr << "auth failed: " << connect.reason << '\n';
        runtime.disconnect();
        return 1;
    }

    smoke::print_account(std::cout, runtime.platform());
    runtime.disconnect();
    return selection.received && selection.success ? 0 : 1;
}

int run_quotes(smoke::IntradeBarSmokeConfig config, const CliOptions& options) {
    if (!smoke::require_live_config(config, std::cerr)) return 2;
    const std::string requested_symbol = smoke::option_value_or(
        options.values,
        "symbol",
        config.quote_symbol);
    const std::string symbol = smoke::normalize_quote_symbol(requested_symbol);

    smoke::IntradeBarSmokeRuntime runtime(std::move(config));
    if (!smoke::connect_or_report(runtime, std::cout, std::cerr)) return 1;
    if (!runtime.wait_for_price_update(symbol)) {
        std::cerr << "timed out waiting for price update: " << symbol << '\n';
        runtime.disconnect();
        return 1;
    }

    const auto ticks = runtime.latest_ticks();
    for (const auto& tick : ticks) {
        if (tick.symbol != symbol) continue;
        std::cout << tick.symbol;
        if (requested_symbol != symbol) {
            std::cout << " requested=" << requested_symbol;
        }
        std::cout
                  << " bid=" << std::setprecision(12) << tick.tick.bid
                  << " ask=" << std::setprecision(12) << tick.tick.ask
                  << " mid=" << std::setprecision(12) << tick.mid_price()
                  << '\n';
    }
    runtime.disconnect();
    return 0;
}

class CliBarAggregator {
public:
    CliBarAggregator(
            optionx::BarTimeframe timeframe,
            optionx::BarPriceSource price_source)
        : m_timeframe(timeframe),
          m_price_source(price_source) {}

    std::optional<optionx::Bar> update(const std::string& symbol, const optionx::Tick& tick) {
        if (m_timeframe <= 0) return std::nullopt;
        const std::uint64_t width_ms =
            static_cast<std::uint64_t>(m_timeframe) * 1000ULL;
        const std::uint64_t tick_time = tick.time_ms != 0
            ? tick.time_ms
            : static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        const std::uint64_t bar_time = (tick_time / width_ms) * width_ms;
        const double price = price_from_tick(tick, m_price_source);

        auto& bar = m_bars[symbol];
        if (bar.time_ms != bar_time) {
            bar = optionx::Bar(price, price, price, price, tick.volume, bar_time, 0);
            optionx::mark_live_payload(bar.flags);
            bar.set_flag(optionx::MarketDataFlags::INCOMPLETE);
            bar.set_price_type(optionx::market_price_type_from_bar_price_source(m_price_source));
            return bar;
        }

        bar.high = std::max(bar.high, price);
        bar.low = std::min(bar.low, price);
        bar.close = price;
        bar.volume += tick.volume;
        optionx::mark_live_payload(bar.flags);
        bar.set_flag(optionx::MarketDataFlags::INCOMPLETE);
        return bar;
    }

private:
    optionx::BarTimeframe m_timeframe = 60;
    optionx::BarPriceSource m_price_source = optionx::BarPriceSource::MID;
    std::unordered_map<std::string, optionx::Bar> m_bars;
};

void print_subscription_result_line(
        std::ostream& out,
        const char* prefix,
        const optionx::market_data::MarketDataSubscriptionResult& result) {
    out << prefix
        << " status=" << optionx::market_data::to_str(result.status)
        << " type=" << optionx::market_data::to_str(result.subscription.stream_type)
        << " symbol=" << result.subscription.symbol
        << " id=" << result.subscription.id
        << " provider=" << result.subscription.provider_id
        << " transport=" << optionx::market_data::to_str(result.subscription.transport);
    if (!result.error_message.empty()) {
        out << " error=\"" << result.error_message << "\"";
    }
    out << '\n';
}

void print_bar_line(
        std::ostream& out,
        const char* prefix,
        const std::string& symbol,
        optionx::BarTimeframe timeframe,
        const optionx::Bar& bar,
        std::uint32_t price_digits,
        std::uint32_t volume_digits) {
    out << prefix
        << " symbol=" << symbol
        << " timeframe=" << timeframe
        << " price_digits=" << price_digits
        << " volume_digits=" << volume_digits
        << " time_ms=" << bar.time_ms
        << " open=" << std::setprecision(12) << bar.open
        << " high=" << std::setprecision(12) << bar.high
        << " low=" << std::setprecision(12) << bar.low
        << " close=" << std::setprecision(12) << bar.close
        << " volume=" << std::setprecision(12) << bar.volume
        << " price_type=" << optionx::to_str(bar.price_type())
        << " flags=" << optionx::market_data_flags_to_string(bar.flags)
        << '\n';
}

int run_market_stream(smoke::IntradeBarSmokeConfig config, const CliOptions& options) {
    if (!smoke::require_live_config(config, std::cerr)) return 2;

    auto symbols = split_csv_symbols(smoke::option_value_or(
        options.values,
        "symbols",
        "BTCUSDT"));
    if (symbols.empty()) {
        std::cerr << "--symbols must contain at least one symbol.\n";
        return 2;
    }

    const std::string type = upper_ascii(smoke::option_value_or(
        options.values,
        "type",
        ""));
    bool wants_ticks = smoke::parse_bool(
        smoke::option_value_or(options.values, "ticks", "0"),
        false);
    bool wants_bars = smoke::parse_bool(
        smoke::option_value_or(options.values, "bars", "0"),
        false);
    if (type == "TICKS" || type == "TICK") wants_ticks = true;
    if (type == "BARS" || type == "BAR") wants_bars = true;
    if (type == "BOTH" || type == "ALL") {
        wants_ticks = true;
        wants_bars = true;
    }
    if (!wants_ticks && !wants_bars) {
        wants_ticks = true;
    }

    const auto transport = optionx::market_data::market_data_transport_from_string(
        smoke::option_value_or(options.values, "transport", "WEBSOCKET"),
        optionx::market_data::MarketDataTransport::WEBSOCKET);
    const auto timeframe_i64 = smoke::option_i64_or(options.values, "timeframe", 60);
    if (timeframe_i64 <= 0 ||
        timeframe_i64 > static_cast<int64_t>((std::numeric_limits<optionx::BarTimeframe>::max)())) {
        std::cerr << "--timeframe must be a positive number of seconds within BarTimeframe range.\n";
        return 2;
    }
    const auto timeframe = static_cast<optionx::BarTimeframe>(timeframe_i64);
    const auto price_source = optionx::bar_price_source_from_string(
        smoke::option_value_or(options.values, "price", "MID"),
        optionx::BarPriceSource::MID);
    const int64_t run_seconds = smoke::option_i64_or(options.values, "seconds", 30);
    const int64_t timeout_ms = std::max<int64_t>(1000, run_seconds * 1000);
    const bool backfill = smoke::parse_bool(
        smoke::option_value_or(options.values, "backfill", wants_bars ? "1" : "0"),
        wants_bars);
    const int64_t backfill_minutes = smoke::option_i64_or(
        options.values,
        "backfill-minutes",
        30);

    std::mutex out_mutex;
    std::atomic<std::size_t> tick_batches{0};
    std::atomic<std::size_t> tick_items{0};
    std::atomic<std::size_t> bar_updates{0};
    std::atomic<std::size_t> status_updates{0};
    std::atomic<std::size_t> history_batches{0};
    CliBarAggregator aggregator(timeframe, price_source);

    smoke::IntradeBarSmokeRuntime runtime(config);
    auto& platform = runtime.platform();

    platform.on_market_data_status() =
        [&out_mutex, &status_updates](optionx::market_data::MarketDataStatusUpdate update) {
            ++status_updates;
            std::lock_guard<std::mutex> lock(out_mutex);
            std::cout << "[status]"
                      << " type=" << optionx::market_data::to_str(update.type)
                      << " symbol=" << update.symbol
                      << " transport=" << optionx::market_data::to_str(update.transport)
                      << " status=" << optionx::market_data::to_str(update.status);
            if (!update.message.empty()) {
                std::cout << " message=\"" << update.message << "\"";
            }
            std::cout << '\n';
        };

    platform.on_tick_data() =
        [&out_mutex,
         &tick_batches,
         &tick_items,
         &bar_updates,
         &aggregator,
         wants_ticks,
         wants_bars,
         timeframe](std::unique_ptr<optionx::market_data::TickDataBatch> batch) {
            if (!batch) return;
            ++tick_batches;
            tick_items += batch->items.size();
            for (const auto& tick : batch->items) {
                if (wants_ticks) {
                    std::lock_guard<std::mutex> lock(out_mutex);
                    std::cout << "[tick]"
                              << " symbol=" << batch->symbol
                              << " price_digits=" << batch->price_digits
                              << " volume_digits=" << batch->volume_digits
                              << " time_ms=" << tick.time_ms
                              << " bid=" << std::setprecision(12) << tick.bid
                              << " ask=" << std::setprecision(12) << tick.ask
                              << " last=" << std::setprecision(12) << tick.last
                              << " volume=" << std::setprecision(12) << tick.volume
                              << " flags=" << optionx::market_data_flags_to_string(tick.flags)
                              << " subscription=" << batch->subscription.id
                              << '\n';
                }
                if (wants_bars) {
                    const auto bar = aggregator.update(batch->symbol, tick);
                    if (bar) {
                        ++bar_updates;
                        std::lock_guard<std::mutex> lock(out_mutex);
                        print_bar_line(
                            std::cout,
                            "[bar-update]",
                            batch->symbol,
                            timeframe,
                            *bar,
                            batch->price_digits,
                            batch->volume_digits);
                    }
                }
            }
        };

    platform.on_bar_data() =
        [&out_mutex, &history_batches](std::unique_ptr<optionx::market_data::BarDataBatch> batch) {
            if (!batch) return;
            ++history_batches;
            std::lock_guard<std::mutex> lock(out_mutex);
            std::cout << "[bar-batch]"
                      << " symbol=" << batch->symbol
                      << " timeframe=" << batch->timeframe
                      << " price_digits=" << batch->price_digits
                      << " volume_digits=" << batch->volume_digits
                      << " items=" << batch->items.size()
                      << " subscription=" << batch->subscription.id
                      << '\n';
            for (const auto& bar : batch->items) {
                print_bar_line(
                    std::cout,
                    "  [bar]",
                    batch->symbol,
                    batch->timeframe,
                    bar,
                    batch->price_digits,
                    batch->volume_digits);
            }
        };

    if (!runtime.start()) {
        std::cerr << "Failed to configure Intrade Bar runtime.\n";
        return 1;
    }

    optionx::market_data::MarketDataSubscriptionBatch tick_batch;
    for (const auto& symbol : symbols) {
        tick_batch.subscribe_ticks(optionx::market_data::TickSubscriptionRequest(symbol, transport));
    }

    optionx::market_data::MarketDataSubscriptionBatchResult tick_subscriptions;
    const bool tick_batch_accepted = platform.apply_subscriptions(
        std::move(tick_batch),
        [&tick_subscriptions, &out_mutex](optionx::market_data::MarketDataSubscriptionBatchResult result) {
            tick_subscriptions = std::move(result);
            std::lock_guard<std::mutex> lock(out_mutex);
            std::cout << "[subscribe-ticks]"
                      << " status=" << optionx::market_data::to_str(tick_subscriptions.status)
                      << " results=" << tick_subscriptions.results.size();
            if (!tick_subscriptions.error_message.empty()) {
                std::cout << " error=\"" << tick_subscriptions.error_message << "\"";
            }
            std::cout << '\n';
            for (const auto& item : tick_subscriptions.results) {
                print_subscription_result_line(std::cout, "  [tick-subscription]", item);
            }
        });
    if (!tick_batch_accepted) {
        std::cerr << "Tick subscription batch was rejected.\n";
        return 1;
    }

    if (wants_bars) {
        for (const auto& symbol : symbols) {
            optionx::market_data::MarketDataSubscriptionResult bar_subscription;
            platform.subscribe_bars(
                optionx::market_data::BarSubscriptionRequest(
                    symbol,
                    timeframe,
                    price_source,
                    transport),
                [&bar_subscription, &out_mutex](optionx::market_data::MarketDataSubscriptionResult result) {
                    bar_subscription = std::move(result);
                    std::lock_guard<std::mutex> lock(out_mutex);
                    print_subscription_result_line(std::cout, "[bar-subscription]", bar_subscription);
                });
        }

        if (backfill && backfill_minutes > 0) {
            optionx::market_data::MarketDataContinuityService continuity(platform);
            const auto now = static_cast<std::int64_t>(std::time(nullptr));
            const auto from = now - backfill_minutes * time_shield::SEC_PER_MIN;
            for (const auto& symbol : symbols) {
                continuity.request_bar_history_batch(
                    optionx::BarHistoryRequest(symbol, timeframe, from, now, price_source),
                    {},
                    platform.on_bar_data(),
                    [&out_mutex, symbol](optionx::BarHistoryResult result) {
                        std::lock_guard<std::mutex> lock(out_mutex);
                        std::cout << "[backfill-error]"
                                  << " symbol=" << symbol
                                  << " status_code=" << result.status_code
                                  << " error=\"" << result.error_desc << "\""
                                  << '\n';
                    },
                    true);
            }
        }
    }

    std::cout << "market_stream symbols=";
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) std::cout << ',';
        std::cout << symbols[i];
    }
    std::cout << " ticks=" << wants_ticks
              << " bars=" << wants_bars
              << " transport=" << optionx::market_data::to_str(transport)
              << " timeframe=" << timeframe
              << " price=" << optionx::to_str(price_source)
              << " seconds=" << run_seconds
              << " backfill=" << backfill
              << '\n';

    const auto connect = runtime.connect(config.auth_timeout_ms);
    std::cout << "auth callback=" << connect.callback_received
              << " success=" << connect.success
              << " elapsed_ms=" << connect.elapsed_ms
              << '\n';
    if (!connect.success) {
        std::cerr << "auth failed: " << connect.reason << '\n';
        runtime.disconnect();
        return 1;
    }

    smoke::print_account(std::cout, platform);
    runtime.pump_for(timeout_ms);
    runtime.disconnect();

    std::cout << "market_stream_summary"
              << " tick_batches=" << tick_batches.load()
              << " tick_items=" << tick_items.load()
              << " bar_updates=" << bar_updates.load()
              << " bar_batches=" << history_batches.load()
              << " status_updates=" << status_updates.load()
              << '\n';

    return tick_items.load() > 0 || bar_updates.load() > 0 || history_batches.load() > 0
        ? 0
        : 1;
}

int run_switch_check(smoke::IntradeBarSmokeConfig config, const CliOptions& options) {
    if (!smoke::require_live_config(config, std::cerr)) return 2;
    if (!options.confirm) {
        std::cerr << "switch-check temporarily changes broker account settings; pass --confirm.\n";
        return 2;
    }

    const auto target_account_type = smoke::parse_enum_or<optionx::AccountType>(
        smoke::option_value_or(
            options.values,
            "account-type",
            optionx::to_str(config.account_type)),
        config.account_type);
    const auto target_currency = smoke::parse_enum_or<optionx::CurrencyType>(
        smoke::option_value_or(
            options.values,
            "currency",
            optionx::to_str(config.currency)),
        config.currency);
    const auto default_source_account_type =
        smoke::opposite_account_type(target_account_type);
    const auto default_source_currency =
        smoke::opposite_currency(target_currency);
    const auto source_account_type = smoke::parse_enum_or<optionx::AccountType>(
        smoke::option_value_or(
            options.values,
            "from-account-type",
            optionx::to_str(default_source_account_type)),
        default_source_account_type);
    const auto source_currency = smoke::parse_enum_or<optionx::CurrencyType>(
        smoke::option_value_or(
            options.values,
            "from-currency",
            optionx::to_str(default_source_currency)),
        default_source_currency);
    const int64_t timeout_ms = smoke::option_i64_or(
        options.values,
        "timeout-ms",
        config.settings_switch_timeout_ms);

    if (source_account_type == target_account_type && source_currency == target_currency) {
        std::cerr << "switch-check source settings must differ from target settings.\n";
        return 2;
    }
    if (source_currency == target_currency) {
        std::cerr << "switch-check source currency must differ from target currency "
                  << "so the connected balance poll can detect the mismatch.\n";
        return 2;
    }

    smoke::IntradeBarSmokeConfig target_config = config;
    target_config.account_type = target_account_type;
    target_config.currency = target_currency;

    smoke::IntradeBarSmokeConfig source_config = config;
    source_config.account_type = source_account_type;
    source_config.currency = source_currency;

    std::cout << "target account_type=" << optionx::to_str(target_account_type)
              << " currency=" << optionx::to_str(target_currency)
              << " balance_check_period_ms=" << target_config.balance_check_period_ms
              << " timeout_ms=" << timeout_ms << '\n';

    smoke::IntradeBarSmokeRuntime target_runtime(target_config);
    if (!smoke::connect_or_report(target_runtime, std::cout, std::cerr)) return 1;
    const bool initial_target_ok = target_runtime.has_account_settings(
        target_account_type,
        target_currency);
    std::cout << "initial_target_ok=" << initial_target_ok << ' ';
    smoke::print_account(std::cout, target_runtime.platform());
    if (!initial_target_ok) {
        target_runtime.disconnect();
        return 1;
    }

    {
        std::cout << "source account_type=" << optionx::to_str(source_account_type)
                  << " currency=" << optionx::to_str(source_currency) << '\n';
        smoke::IntradeBarSmokeRuntime source_runtime(source_config);
        if (!smoke::connect_or_report(source_runtime, std::cout, std::cerr)) {
            target_runtime.disconnect();
            restore_settings_direct(target_config, std::cout, std::cerr);
            return 1;
        }
        const bool source_ok = source_runtime.has_account_settings(
            source_account_type,
            source_currency);
        std::cout << "source_ok=" << source_ok << ' ';
        smoke::print_account(std::cout, source_runtime.platform());
        source_runtime.disconnect();
        if (!source_ok) {
            target_runtime.disconnect();
            restore_settings_direct(target_config, std::cout, std::cerr);
            return 1;
        }
    }

    const std::size_t update_count = target_runtime.account_update_count();
    const auto started_at = std::chrono::steady_clock::now();
    const bool recovered = target_runtime.wait_for_account_settings_after(
        target_account_type,
        target_currency,
        update_count,
        timeout_ms);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at).count();

    std::cout << "recovered=" << recovered
              << " elapsed_ms=" << elapsed_ms
              << " account_updates=" << target_runtime.account_update_count()
              << ' ';
    smoke::print_account(std::cout, target_runtime.platform());

    target_runtime.disconnect();
    if (!recovered) {
        restore_settings_direct(target_config, std::cout, std::cerr);
    }
    return recovered ? 0 : 1;
}

std::optional<std::uint32_t> trade_duration_from_cli(
        int64_t duration,
        std::ostream& err) {
    if (duration <= 0 ||
        duration > static_cast<int64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        err << "--duration must be a positive number of seconds within uint32 range.\n";
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(duration);
}

int run_open_trade(smoke::IntradeBarSmokeConfig config, const CliOptions& options) {
    if (!smoke::require_live_config(config, std::cerr)) return 2;
    const bool confirmed = options.confirm || config.allow_trade;
    if (!confirmed) {
        std::cerr << "open-trade requires --confirm or OPTIONX_INTRADE_BAR_ALLOW_TRADE=1.\n";
        return 2;
    }
    if (config.account_type != optionx::AccountType::DEMO &&
        !(options.allow_real && config.allow_real_trade)) {
        std::cerr << "Refusing real-account trade. Use DEMO or set both --allow-real and OPTIONX_INTRADE_BAR_ALLOW_REAL_TRADE=1.\n";
        return 2;
    }

    const std::string symbol = smoke::option_value_or(
        options.values,
        "symbol",
        config.trade_symbol);
    const double amount = smoke::option_double_or(
        options.values,
        "amount",
        config.trade_amount);
    const int64_t duration = smoke::option_i64_or(
        options.values,
        "duration",
        config.trade_duration_sec);
    const auto trade_duration = trade_duration_from_cli(duration, std::cerr);
    if (!trade_duration) return 2;
    const auto order_type = smoke::parse_enum_or<optionx::OrderType>(
        smoke::option_value_or(
            options.values,
            "order",
            optionx::to_str(config.trade_order_type)),
        config.trade_order_type);

    smoke::IntradeBarSmokeRuntime runtime(config);
    if (!smoke::connect_or_report(runtime, std::cout, std::cerr)) return 1;
    smoke::print_account(std::cout, runtime.platform());

    const auto open = runtime.open_trade_and_wait(
        symbol,
        amount,
        order_type,
        *trade_duration,
        config.trade_open_timeout_ms);
    std::cout << "trade accepted=" << open.accepted
              << " callback=" << open.callback_received
              << " state=" << optionx::to_str(open.state)
              << " option_id=" << open.option_id
              << " open_price=" << open.open_price
              << " elapsed_ms=" << open.elapsed_ms
              << " error=" << open.error_desc
              << '\n';

    runtime.disconnect();
    return open.accepted && open.callback_received &&
        open.state == optionx::TradeState::OPEN_SUCCESS ? 0 : 1;
}

bool is_btc_symbol(const std::string& symbol) {
    const std::string normalized = upper_ascii(symbol);
    return normalized == "BTCUSD" || normalized == "BTCUSDT";
}

int64_t default_open_trades_sync_duration_sec(const std::string& symbol) {
    return is_btc_symbol(symbol) ? 5 * time_shield::SEC_PER_MIN :
        3 * time_shield::SEC_PER_MIN;
}

int run_open_trades_sync_check(
        smoke::IntradeBarSmokeConfig config,
        const CliOptions& options) {
    if (!smoke::require_live_config(config, std::cerr)) return 2;
    const bool confirmed = options.confirm || config.allow_trade;
    if (!confirmed) {
        std::cerr << "open-trades-sync-check opens a broker trade; pass --confirm or OPTIONX_INTRADE_BAR_ALLOW_TRADE=1.\n";
        return 2;
    }
    if (config.account_type != optionx::AccountType::DEMO &&
        !(options.allow_real && config.allow_real_trade)) {
        std::cerr << "Refusing real-account trade. Use DEMO or set both --allow-real and OPTIONX_INTRADE_BAR_ALLOW_REAL_TRADE=1.\n";
        return 2;
    }

    config.active_trades_close_buffer_ms = smoke::option_i64_or(
        options.values,
        "close-buffer-ms",
        config.active_trades_close_buffer_ms);
    config.active_trades_sync_period_ms = smoke::option_i64_or(
        options.values,
        "sync-period-ms",
        config.active_trades_sync_period_ms);
    config.order_interval_ms = smoke::option_i64_or(
        options.values,
        "order-interval-ms",
        config.order_interval_ms);

    const std::string requested_symbol = smoke::option_value_or(
        options.values,
        "symbol",
        "BTCUSDT");
    const std::string symbol = smoke::normalize_quote_symbol(requested_symbol);
    const int64_t duration = smoke::option_i64_or(
        options.values,
        "duration",
        default_open_trades_sync_duration_sec(symbol));
    const auto trade_duration = trade_duration_from_cli(duration, std::cerr);
    if (!trade_duration) return 2;
    const double amount = smoke::option_double_or(
        options.values,
        "amount",
        config.trade_amount);
    const auto order_type = smoke::parse_enum_or<optionx::OrderType>(
        smoke::option_value_or(
            options.values,
            "order",
            optionx::to_str(config.trade_order_type)),
        config.trade_order_type);
    const int64_t snapshot_timeout_ms = smoke::option_i64_or(
        options.values,
        "snapshot-timeout-ms",
        std::max<int64_t>(30000, config.active_trades_sync_period_ms + 15000));
    const int64_t local_counter_timeout_ms = smoke::option_i64_or(
        options.values,
        "local-counter-timeout-ms",
        10000);
    const int64_t trade_count = std::max<int64_t>(
        1,
        smoke::option_i64_or(options.values, "count", 1));
    const int64_t open_interval_ms = std::max<int64_t>(
        0,
        smoke::option_i64_or(options.values, "interval-ms", 15000));
    const int64_t min_accepted = std::max<int64_t>(
        1,
        smoke::option_i64_or(options.values, "min-accepted", trade_count));

    std::cout << "open_trades_sync_check symbol=" << symbol;
    if (requested_symbol != symbol) {
        std::cout << " requested=" << requested_symbol;
    }
    std::cout << " amount=" << amount
              << " duration=" << duration
              << " order=" << optionx::to_str(order_type)
              << " count=" << trade_count
              << " interval_ms=" << open_interval_ms
              << " min_accepted=" << min_accepted
              << " order_interval_ms=" << config.order_interval_ms
              << " close_buffer_ms=" << config.active_trades_close_buffer_ms
              << " sync_period_ms=" << config.active_trades_sync_period_ms
              << " snapshot_timeout_ms=" << snapshot_timeout_ms
              << '\n';

    smoke::IntradeBarSmokeRuntime open_runtime(config);
    const std::size_t first_initial_updates = open_runtime.open_trades_update_count();
    if (!smoke::connect_or_report(open_runtime, std::cout, std::cerr)) return 1;
    const bool first_snapshot = open_runtime.wait_for_open_trades_update_after(
        first_initial_updates,
        snapshot_timeout_ms);
    const int64_t initial_open_trades = open_runtime.current_open_trades();
    std::cout << "initial_snapshot=" << first_snapshot
              << " initial_open_trades=" << initial_open_trades
              << " open_trades_updates=" << open_runtime.open_trades_update_count()
              << ' ';
    smoke::print_account(std::cout, open_runtime.platform());
    if (!first_snapshot) {
        std::cerr << "Initial broker open-trades snapshot was not received.\n";
        open_runtime.disconnect();
        return 1;
    }

    std::vector<smoke::TradeOpenAttempt> opened_trades;
    opened_trades.reserve(static_cast<std::size_t>(trade_count));
    bool local_counter_ok = true;
    for (int64_t index = 0; index < trade_count; ++index) {
        const int64_t open_attempt_started_ms = OPTIONX_TIMESTAMP_MS;
        const std::size_t before_open_updates = open_runtime.open_trades_update_count();
        const auto open = open_runtime.open_trade_and_wait(
            symbol,
            amount,
            order_type,
            *trade_duration,
            config.trade_open_timeout_ms);

        bool local_increment = false;
        if (open.accepted &&
            open.callback_received &&
            open.state == optionx::TradeState::OPEN_SUCCESS &&
            open.option_id > 0) {
            opened_trades.push_back(open);
            local_increment = open_runtime.wait_for_open_trades_at_least_after(
                initial_open_trades + static_cast<int64_t>(opened_trades.size()),
                before_open_updates,
                local_counter_timeout_ms);
            local_counter_ok = local_counter_ok && local_increment;
        }
        const int64_t local_open_trades = open_runtime.current_open_trades();

        std::cout << "open[" << (index + 1) << "/" << trade_count << "]"
                  << " accepted=" << open.accepted
                  << " callback=" << open.callback_received
                  << " state=" << optionx::to_str(open.state)
                  << " option_id=" << open.option_id
                  << " open_price=" << std::setprecision(12) << open.open_price
                  << " open_date=" << open.open_date
                  << " close_date=" << open.close_date
                  << " elapsed_ms=" << open.elapsed_ms
                  << " local_increment=" << local_increment
                  << " local_open_trades=" << local_open_trades
                  << " error=" << open.error_desc
                  << '\n';

        if (index + 1 < trade_count && open_interval_ms > 0) {
            const int64_t elapsed_ms = OPTIONX_TIMESTAMP_MS - open_attempt_started_ms;
            open_runtime.pump_for(std::max<int64_t>(0, open_interval_ms - elapsed_ms));
        }
    }

    std::cout << "open_summary requested=" << trade_count
              << " accepted=" << opened_trades.size()
              << " local_counter_ok=" << local_counter_ok
              << " local_open_trades=" << open_runtime.current_open_trades()
              << '\n';

    open_runtime.disconnect();

    if (static_cast<int64_t>(opened_trades.size()) < min_accepted ||
        !local_counter_ok) {
        return 1;
    }

    smoke::IntradeBarSmokeRuntime snapshot_runtime(config);
    const std::size_t reconnect_updates = snapshot_runtime.open_trades_update_count();
    const auto reconnect = snapshot_runtime.connect(config.auth_timeout_ms);
    std::cout << "reconnect callback=" << reconnect.callback_received
              << " success=" << reconnect.success
              << " elapsed_ms=" << reconnect.elapsed_ms << '\n';
    if (!reconnect.success) {
        std::cerr << "reconnect auth failed: " << reconnect.reason << '\n';
        snapshot_runtime.disconnect();
        return 1;
    }

    const bool broker_snapshot = snapshot_runtime.wait_for_open_trades_at_least_after(
        static_cast<int64_t>(opened_trades.size()),
        reconnect_updates,
        snapshot_timeout_ms);
    const int64_t broker_snapshot_open_trades = snapshot_runtime.current_open_trades();
    std::cout << "broker_snapshot=" << broker_snapshot
              << " broker_open_trades=" << broker_snapshot_open_trades
              << " open_trades_updates=" << snapshot_runtime.open_trades_update_count()
              << ' ';
    smoke::print_account(std::cout, snapshot_runtime.platform());
    if (!broker_snapshot || broker_snapshot_open_trades <= 0) {
        snapshot_runtime.disconnect();
        return 1;
    }

    int64_t latest_close_date = 0;
    for (const auto& trade : opened_trades) {
        latest_close_date = std::max(latest_close_date, trade.close_date);
    }
    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    const int64_t default_close_timeout_ms = std::max<int64_t>(
        30000,
        std::max<int64_t>(0, latest_close_date - now_ms) +
            config.active_trades_close_buffer_ms +
            2 * config.active_trades_sync_period_ms +
            60000);
    const int64_t close_timeout_ms = smoke::option_i64_or(
        options.values,
        "close-timeout-ms",
        default_close_timeout_ms);
    const int64_t target_open_trades = std::max<int64_t>(
        0,
        broker_snapshot_open_trades - static_cast<int64_t>(opened_trades.size()));
    std::size_t last_update_count = snapshot_runtime.open_trades_update_count();
    std::cout << "waiting_for_decrement close_timeout_ms=" << close_timeout_ms
              << " expected_close_date=" << latest_close_date
              << " target_open_trades=" << target_open_trades
              << '\n';

    const int64_t close_wait_started_ms = OPTIONX_TIMESTAMP_MS;
    const int64_t close_deadline_ms = close_wait_started_ms + close_timeout_ms;
    while (snapshot_runtime.current_open_trades() > target_open_trades &&
           OPTIONX_TIMESTAMP_MS < close_deadline_ms) {
        const int64_t remaining_ms = close_deadline_ms - OPTIONX_TIMESTAMP_MS;
        const bool update = snapshot_runtime.wait_for_open_trades_update_after(
            last_update_count,
            std::max<int64_t>(1, remaining_ms));
        const auto updates = snapshot_runtime.open_trades_updates();
        if (updates.size() > last_update_count) {
            for (std::size_t i = last_update_count; i < updates.size(); ++i) {
                std::cout << "broker_counter_update index=" << (i + 1)
                          << " open_trades=" << updates[i]
                          << " elapsed_ms=" << (OPTIONX_TIMESTAMP_MS - close_wait_started_ms)
                          << '\n';
            }
            last_update_count = updates.size();
        }
        if (!update) break;
    }

    const int64_t final_open_trades = snapshot_runtime.current_open_trades();
    const bool broker_decrement = final_open_trades <= target_open_trades;
    std::cout << "broker_decrement=" << broker_decrement
              << " final_open_trades=" << final_open_trades
              << " open_trades_updates=" << snapshot_runtime.open_trades_update_count()
              << ' ';
    smoke::print_account(std::cout, snapshot_runtime.platform());

    snapshot_runtime.disconnect();
    return broker_decrement ? 0 : 1;
}

void print_trade_result_line(
        std::ostream& out,
        const std::string& prefix,
        const optionx::TradeResult& result) {
    out << prefix
        << " state=" << optionx::to_str(result.trade_state)
        << " option_id=" << result.option_id
        << " amount=" << std::setprecision(12) << result.amount
        << " currency=" << optionx::to_str(result.currency)
        << " open_price=" << std::setprecision(12) << result.open_price
        << " close_price=" << std::setprecision(12) << result.close_price
        << " profit=" << std::setprecision(12) << result.profit
        << " payout=" << std::setprecision(12) << result.payout
        << " balance=" << std::setprecision(12) << result.balance
        << " open_date=" << result.open_date
        << " close_date=" << result.close_date
        << " error=" << result.error_desc
        << '\n';
}

void print_trade_record_line(
        std::ostream& out,
        const std::string& prefix,
        const optionx::TradeRecord& record) {
    out << prefix
        << " state=" << optionx::to_str(record.trade_state)
        << " option_id=" << record.option_id
        << " symbol=" << record.symbol
        << " option_type=" << optionx::to_str(record.option_type)
        << " order=" << optionx::to_str(record.order_type)
        << " amount=" << std::setprecision(12) << record.amount
        << " currency=" << optionx::to_str(record.currency)
        << " open_price=" << std::setprecision(12) << record.open_price
        << " close_price=" << std::setprecision(12) << record.close_price
        << " profit=" << std::setprecision(12) << record.profit
        << " payout=" << std::setprecision(12) << record.payout
        << " close_balance=" << std::setprecision(12) << record.close_balance
        << " open_date=" << record.open_date
        << " close_date=" << record.close_date
        << " comment=" << record.comment
        << " error=" << record.error_desc
        << '\n';
}

int run_history(smoke::IntradeBarSmokeConfig config, const CliOptions& options) {
    if (!smoke::require_live_config(config, std::cerr)) return 2;

    config.trade_history_source =
        optionx::platforms::intrade_bar::trade_history_source_from_string(
            smoke::option_value_or(
                options.values,
                "source",
                optionx::platforms::intrade_bar::trade_history_source_to_string(
                    config.trade_history_source)),
            config.trade_history_source);
    config.account_type = smoke::parse_enum_or<optionx::AccountType>(
        smoke::option_value_or(
            options.values,
            "account-type",
            optionx::to_str(config.account_type)),
        config.account_type);

    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    const int64_t days = smoke::option_i64_or(options.values, "days", 14);
    const bool fetch_all = options.values.find("all") != options.values.end();
    const int64_t from_ms = smoke::option_i64_or(
        options.values,
        "from-ms",
        now_ms - days * time_shield::MS_PER_DAY);
    const int64_t to_ms = smoke::option_i64_or(options.values, "to-ms", now_ms);
    const int64_t timeout_ms = smoke::option_i64_or(
        options.values,
        "timeout-ms",
        config.auth_timeout_ms);

    optionx::TradeHistoryRequest request = fetch_all ?
        optionx::TradeHistoryRequest::all() :
        optionx::TradeHistoryRequest{};
    request.time_field = parse_trade_history_time_field(
        smoke::option_value_or(
            options.values,
            "time-field",
            trade_history_time_field_to_string(request.time_field)),
        request.time_field);
    request.comment = smoke::option_value_or(
        options.values,
        "comment",
        request.comment);
    if (!fetch_all) {
        request.start_ms = from_ms;
        request.stop_ms = to_ms;
        request.range_mode = parse_time_range_mode(
            smoke::option_value_or(
                options.values,
                "range-mode",
                time_range_mode_to_string(request.range_mode)),
            request.range_mode);
    }

    std::cout << "history source="
              << optionx::platforms::intrade_bar::trade_history_source_to_string(
                     config.trade_history_source)
              << " account_type=" << optionx::to_str(config.account_type)
              << " range_mode=" << time_range_mode_to_string(request.range_mode)
              << " time_field=" << trade_history_time_field_to_string(request.time_field)
              << " from_ms=" << request.start_ms
              << " to_ms=" << request.stop_ms
              << " comment=" << request.comment
              << " timeout_ms=" << timeout_ms
              << '\n';

    smoke::IntradeBarSmokeRuntime runtime(config);
    if (!smoke::connect_or_report(runtime, std::cout, std::cerr)) return 1;
    smoke::print_account(std::cout, runtime.platform());

    const auto history = runtime.fetch_trade_history_and_wait(request, timeout_ms);
    std::cout << "history accepted=" << history.accepted
              << " callback=" << history.callback_received
              << " success=" << history.success
              << " status_code=" << history.status_code
              << " records=" << history.records.size()
              << " elapsed_ms=" << history.elapsed_ms
              << " error=" << history.error_desc
              << '\n';
    for (const auto& record : history.records) {
        print_trade_record_line(std::cout, "history_record", record);
    }

    runtime.disconnect();
    return history.accepted && history.callback_received && history.success ? 0 : 1;
}

int run_open_check_result(smoke::IntradeBarSmokeConfig config, const CliOptions& options) {
    if (!smoke::require_live_config(config, std::cerr)) return 2;
    const bool confirmed = options.confirm || config.allow_trade;
    if (!confirmed) {
        std::cerr << "open-check-result opens a broker trade; pass --confirm or OPTIONX_INTRADE_BAR_ALLOW_TRADE=1.\n";
        return 2;
    }
    if (config.account_type != optionx::AccountType::DEMO &&
        !(options.allow_real && config.allow_real_trade)) {
        std::cerr << "Refusing real-account trade. Use DEMO or set both --allow-real and OPTIONX_INTRADE_BAR_ALLOW_REAL_TRADE=1.\n";
        return 2;
    }

    const std::string symbol = smoke::option_value_or(
        options.values,
        "symbol",
        "BTCUSDT");
    const double amount = smoke::option_double_or(
        options.values,
        "amount",
        1.0);
    const int64_t duration = smoke::option_i64_or(
        options.values,
        "duration",
        5 * time_shield::SEC_PER_MIN);
    const auto trade_duration = trade_duration_from_cli(duration, std::cerr);
    if (!trade_duration) return 2;
    const int64_t result_timeout_ms = smoke::option_i64_or(
        options.values,
        "result-timeout-ms",
        config.trade_result_timeout_ms);
    const int retry_attempts = static_cast<int>(smoke::option_i64_or(
        options.values,
        "retry-attempts",
        15));
    const auto order_type = smoke::parse_enum_or<optionx::OrderType>(
        smoke::option_value_or(
            options.values,
            "order",
            optionx::to_str(config.trade_order_type)),
        config.trade_order_type);

    smoke::IntradeBarSmokeRuntime runtime(config);
    if (!smoke::connect_or_report(runtime, std::cout, std::cerr)) return 1;
    smoke::print_account(std::cout, runtime.platform());

    std::cout << "open_check_result symbol=" << symbol
              << " amount=" << amount
              << " duration=" << duration
              << " order=" << optionx::to_str(order_type)
              << " result_timeout_ms=" << result_timeout_ms
              << " retry_attempts=" << retry_attempts
              << '\n';

    const auto lifecycle = runtime.open_trade_and_wait_for_result(
        symbol,
        amount,
        order_type,
        *trade_duration,
        result_timeout_ms);

    std::cout << "lifecycle accepted=" << lifecycle.accepted
              << " callback=" << lifecycle.callback_received
              << " open_received=" << lifecycle.open_received
              << " terminal_received=" << lifecycle.terminal_received
              << " elapsed_ms=" << lifecycle.elapsed_ms
              << '\n';
    print_trade_result_line(std::cout, "lifecycle_result", lifecycle.result);

    if (!lifecycle.accepted ||
        !lifecycle.callback_received ||
        !lifecycle.open_received ||
        !lifecycle.terminal_received ||
        lifecycle.result.option_id <= 0) {
        runtime.disconnect();
        return 1;
    }

    optionx::TradeResultQuery query;
    query.trade_id = lifecycle.result.trade_id;
    query.option_id = lifecycle.result.option_id;
    query.retry_attempts = retry_attempts;

    auto restored_result = std::make_unique<optionx::TradeResult>();
    restored_result->trade_id = lifecycle.result.trade_id;
    restored_result->option_id = lifecycle.result.option_id;
    restored_result->amount = lifecycle.result.amount;
    restored_result->payout = lifecycle.result.payout;
    restored_result->account_type = lifecycle.result.account_type;
    restored_result->currency = lifecycle.result.currency;
    restored_result->platform_type = lifecycle.result.platform_type;
    restored_result->open_price = lifecycle.result.open_price;
    restored_result->open_date = lifecycle.result.open_date;
    restored_result->close_date = lifecycle.result.close_date;

    const auto fetch = runtime.fetch_trade_result_and_wait(
        std::move(query),
        std::move(restored_result),
        config.trade_open_timeout_ms);

    std::cout << "fetch accepted=" << fetch.accepted
              << " callback=" << fetch.callback_received
              << " elapsed_ms=" << fetch.elapsed_ms
              << '\n';
    print_trade_result_line(std::cout, "fetched_result", fetch.result);

    const bool state_match = fetch.result.trade_state == lifecycle.result.trade_state;
    const bool profit_match = std::abs(fetch.result.profit - lifecycle.result.profit) < 0.01;
    std::cout << "compare state_match=" << state_match
              << " profit_match=" << profit_match
              << '\n';

    runtime.disconnect();
    return fetch.accepted &&
        fetch.callback_received &&
        optionx::is_terminal_trade_state(fetch.result.trade_state) &&
        fetch.result.trade_state != optionx::TradeState::CHECK_ERROR &&
        state_match &&
        profit_match ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    LOGIT_ADD_MEMORY_LOGGER_DEFAULT_SINGLE_MODE();
    if (smoke::parse_bool(smoke::getenv_or_empty("OPTIONX_INTRADE_BAR_CLI_CONSOLE_LOG"), false)) {
        LOGIT_ADD_CONSOLE_DEFAULT();
    }
    LOGIT_ADD_FILE_LOGGER_DEFAULT();
    LOGIT_ADD_UNIQUE_FILE_LOGGER_DEFAULT_SINGLE_MODE();
    kurlyk::init();

    const CliOptions options = parse_args(argc, argv);
    auto config = smoke::load_config();
    int result = 0;

    if (options.command == "help" || options.command == "--help" || options.command == "-h") {
        print_usage(std::cout);
    } else if (options.command == "auth") {
        result = run_auth(std::move(config));
    } else if (options.command == "auth-cache") {
        result = run_auth_cache(std::move(config));
    } else if (options.command == "show-account") {
        result = run_auth(std::move(config));
    } else if (options.command == "domain-check") {
        result = run_domain_check(std::move(config), options);
    } else if (options.command == "quotes") {
        result = run_quotes(std::move(config), options);
    } else if (options.command == "market-stream") {
        result = run_market_stream(std::move(config), options);
    } else if (options.command == "history") {
        result = run_history(std::move(config), options);
    } else if (options.command == "switch-check") {
        result = run_switch_check(std::move(config), options);
    } else if (options.command == "open-trade") {
        result = run_open_trade(std::move(config), options);
    } else if (options.command == "open-trades-sync-check") {
        result = run_open_trades_sync_check(std::move(config), options);
    } else if (options.command == "open-check-result") {
        result = run_open_check_result(std::move(config), options);
    } else {
        std::cerr << "Unknown command: " << options.command << "\n\n";
        print_usage(std::cerr);
        result = 2;
    }

    kurlyk::deinit();
    LOGIT_WAIT();
    return result;
}
