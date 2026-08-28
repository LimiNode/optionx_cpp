#pragma once
#ifndef OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_STATS_CALCULATOR_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_STATS_CALCULATOR_HPP_INCLUDED

/// \file TradeStatsCalculator.hpp
/// \brief Computes TradeStats from a vector of TradeRecord objects.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#include <time_shield.hpp>

#include <optionx_cpp/data/trading.hpp>
#include "TradeRecordFilterMatcher.hpp"

namespace optionx::storage {

    /// \class TradeStatsCalculator
    /// \brief Static calculator producing a full statistical summary over trade records.
    class TradeStatsCalculator {
    public:
        /// \brief Calculates statistics from a collection of trade records.
        /// \param records Input trade records.
        /// \param config Optional filtering and conversion configuration.
        /// \return Heap-allocated TradeStats (large struct — never returned by value).
        static std::unique_ptr<optionx::TradeStats> calc(
                const std::vector<optionx::TradeRecord>& records,
                const optionx::TradeStatsConfig& config = {}) {
            auto stats_ptr = std::make_unique<optionx::TradeStats>();
            auto& stats = *stats_ptr;

            std::map<std::int64_t, double> realized_profit_by_ts;
            std::map<std::int64_t, double> daily_profit_map;
            std::map<std::int64_t, double> hourly_profit_map;

            std::uint64_t volume_trade_count = 0;

            struct Event {
                std::int64_t ts;
                double delta;
            };

            std::vector<Event> events;
            events.reserve(records.size() * 2);
            std::vector<BalanceSnapshotEvent> balance_events;
            balance_events.reserve(records.size());
            std::vector<OutcomeEvent> outcome_events;
            outcome_events.reserve(records.size());

            for (const auto& rec : records) {
                // 1. Filter predicate (applies to both outcome and monetary)
                if (!TradeRecordFilterMatcher::match_filter(rec, config.filter, config.time_zone)) {
                    continue;
                }

                // 2. Determine if this record contributes to selected statistics.
                const bool include_selected = include_by_selection(rec, config);
                bool include_outcome = include_selected;

                // 3. Error / terminal inclusion for outcome stats
                if (include_outcome) {
                    if (!config.include_errors && optionx::is_error_trade_state(rec.trade_state)) {
                        include_outcome = false;
                    }
                    if (!config.include_non_terminal && !optionx::is_terminal_trade_state(rec.trade_state)) {
                        include_outcome = false;
                    }
                }

                // 4. Monetary aggregations (same selected result-state population)
                if (include_selected && optionx::is_result_state(rec.trade_state)) {
                    const auto curve_ts = rec.close_date > 0 ? rec.close_date : rec.open_date;
                    const auto amount_ts = rec.open_date > 0 ? rec.open_date : curve_ts;
                    const double amount = convert_money(rec.amount, rec.currency, amount_ts, config);
                    const double profit = convert_money(rec.profit, rec.currency, curve_ts, config);

                    ++volume_trade_count;
                    stats.total_volume += amount;
                    stats.total_profit += profit;
                    if (profit > 0.0) stats.gross_profit += profit;
                    if (profit < 0.0) stats.gross_loss += -profit;

                    stats.max_profit_trade = std::max(stats.max_profit_trade, profit);
                    stats.max_loss_trade = std::min(stats.max_loss_trade, profit);

                    // Realized-profit curves are built after aggregation by
                    // timestamp, so same-moment closes do not invent an order.
                    if (curve_ts > 0) {
                        realized_profit_by_ts[curve_ts] += profit;
                        if (rec.has_close_balance()) {
                            balance_events.push_back({
                                curve_ts,
                                rec.open_date > 0 ? rec.open_date : curve_ts,
                                AccountKey{
                                    rec.platform_type,
                                    rec.account_type,
                                    rec.account_id,
                                rec.currency},
                                rec.currency,
                                rec.open_balance,
                                rec.has_open_balance(),
                                close_balance_snapshot(rec),
                                rec.profit});
                        }

                        // Daily / hourly profit buckets
                        const auto day_utc =
                            config.time_zone.start_of_local_day_utc_ms(curve_ts);
                        daily_profit_map[day_utc] += profit;

                        const auto hour_utc =
                            config.time_zone.start_of_local_hour_utc_ms(curve_ts);
                        hourly_profit_map[hour_utc] += profit;
                    }

                    // Ping stats
                    if (rec.ping > 0) {
                        stats.ping.by_ping_ms[rec.ping].add(rec);
                    }

                    // Sweep-line events for free-funds curve
                    if (config.balance_mode == optionx::TradeStatsBalanceMode::SWEEP_LINE) {
                        if (rec.open_date > 0) {
                            events.push_back({rec.open_date, -amount});
                        }
                        if (curve_ts > 0) {
                            events.push_back({curve_ts, amount + profit});
                        }
                    }
                }

                // 6. Winrate / outcome aggregations (terminal trades only, subject to selection)
                if (include_outcome && optionx::is_terminal_trade_state(rec.trade_state)) {
                    stats.total.add(rec);
                    if (rec.order_type == optionx::OrderType::BUY) stats.buy.add(rec);
                    else if (rec.order_type == optionx::OrderType::SELL) stats.sell.add(rec);

                    if (!rec.symbol.empty()) stats.by_symbol[rec.symbol].add(rec);
                    if (!rec.signal_name.empty()) stats.by_signal[rec.signal_name].add(rec);
                    stats.by_platform[rec.platform_type].add(rec);
                    stats.by_currency[rec.currency].add(rec);
                    if (rec.duration > 0) stats.by_duration[rec.duration].add(rec);
                    stats.by_mm_step[rec.mm_step].add(rec);

                    // Time-of-day buckets
                    const auto ts = optionx::select_timestamp_ms(rec, optionx::TradeRecordTimeField::AUTO);
                    if (ts > 0) {
                        const auto local_ms = config.time_zone.to_local_ms(ts);
                        const auto sec = time_shield::ms_to_sec<time_shield::ts_t>(local_ms);
                        const auto dt = time_shield::to_date_time<time_shield::DateTimeStruct>(sec);
                        const auto sec_of_day = dt.hour * 3600 + dt.min * 60 + dt.sec;
                        const auto min_of_day = dt.hour * 60 + dt.min;

                        if (sec_of_day >= 0 && sec_of_day < 86400) {
                            stats.by_sec[static_cast<std::size_t>(sec_of_day)].add(rec);
                        }
                        if (min_of_day >= 0 && min_of_day < 1440) {
                            stats.by_min[static_cast<std::size_t>(min_of_day)].add(rec);
                        }
                        if (dt.hour >= 0 && dt.hour < 24) {
                            stats.by_hour[static_cast<std::size_t>(dt.hour)].add(rec);
                        }
                        const auto weekday = time_shield::weekday_of_ts(sec);
                        if (weekday >= 0 && weekday < 7) {
                            stats.by_weekday[static_cast<std::size_t>(weekday)].add(rec);
                        }
                        if (dt.day >= 1 && dt.day <= 31) {
                            stats.by_month_day[static_cast<std::size_t>(dt.day - 1)].add(rec);
                        }
                        if (dt.mon >= 1 && dt.mon <= 12) {
                            stats.by_month[static_cast<std::size_t>(dt.mon - 1)].add(rec);
                        }
                        if (dt.sec >= 0 && dt.sec < 60) {
                            stats.by_second[static_cast<std::size_t>(dt.sec)].add(rec);
                        }
                    }

                    outcome_events.push_back(make_outcome_event(rec));
                }
            }

            recount_series(outcome_events, stats.series);

            // Finalize winrate stats
            stats.total.calc();
            stats.buy.calc();
            stats.sell.calc();
            for (auto& kv : stats.by_symbol) kv.second.calc();
            for (auto& kv : stats.by_signal) kv.second.calc();
            for (auto& kv : stats.by_platform) kv.second.calc();
            for (auto& kv : stats.by_currency) kv.second.calc();
            for (auto& kv : stats.by_duration) kv.second.calc();
            for (auto& kv : stats.by_mm_step) kv.second.calc();
            for (auto& kv : stats.ping.by_ping_ms) kv.second.calc();
            for (auto& s : stats.by_sec) s.calc();
            for (auto& s : stats.by_min) s.calc();
            for (auto& s : stats.by_hour) s.calc();
            for (auto& s : stats.by_weekday) s.calc();
            for (auto& s : stats.by_month_day) s.calc();
            for (auto& s : stats.by_month) s.calc();
            for (auto& s : stats.by_second) s.calc();

            // Derived monetary stats
            stats.profit_factor = (stats.gross_loss > 0.0)
                ? (stats.gross_profit / stats.gross_loss)
                : ((stats.gross_profit > 0.0)
                    ? std::numeric_limits<double>::infinity()
                    : 0.0);

            if (volume_trade_count > 0) {
                stats.average_amount = stats.total_volume / static_cast<double>(volume_trade_count);
            }
            if (volume_trade_count > 0) {
                stats.average_profit_per_trade =
                    stats.total_profit / static_cast<double>(volume_trade_count);
                stats.average_profit =
                    stats.total_profit / static_cast<double>(volume_trade_count);
            }

            if (config.equity_mode == optionx::TradeStatsEquityMode::RECORD_BALANCE) {
                build_record_balance_curves(stats, config, balance_events);
            } else if (config.equity_mode == optionx::TradeStatsEquityMode::PORTFOLIO_BALANCE) {
                build_portfolio_balance_curves(stats, config, balance_events);
            } else {
                build_synthetic_curves(stats, config, realized_profit_by_ts);
            }

            // Fill chart data
            for (const auto& kv : daily_profit_map) {
                stats.daily_profit.x_time.push_back(kv.first);
                stats.daily_profit.y_value.push_back(kv.second);
            }
            for (const auto& kv : hourly_profit_map) {
                stats.hourly_profit.x_time.push_back(kv.first);
                stats.hourly_profit.y_value.push_back(kv.second);
            }

            // Sweep-line free-funds curve
            if (config.balance_mode == optionx::TradeStatsBalanceMode::SWEEP_LINE && !events.empty()) {
                std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
                    return a.ts < b.ts;
                });

                double free_funds = config.start_balance;
                double peak_free = config.start_balance;

                for (std::size_t i = 0; i < events.size();) {
                    const auto ts = events[i].ts;
                    double delta = 0.0;

                    do {
                        delta += events[i].delta;
                        ++i;
                    } while (i < events.size() && events[i].ts == ts);

                    free_funds += delta;
                    peak_free = std::max(peak_free, free_funds);

                    const auto dd = peak_free - free_funds;
                    if (dd > stats.max_absolute_drawdown_free) {
                        stats.max_absolute_drawdown_free = dd;
                        stats.max_drawdown_date_free = ts;
                    }
                    if (peak_free > 0.0) {
                        stats.max_relative_drawdown_free = std::max(
                            stats.max_relative_drawdown_free, dd / peak_free);
                    }

                    stats.free_funds_curve.x_time.push_back(ts);
                    stats.free_funds_curve.y_value.push_back(free_funds);
                }
            }

            return stats_ptr;
        }

    private:
        struct AccountKey {
            optionx::PlatformType platform_type = optionx::PlatformType::UNKNOWN;
            optionx::AccountType account_type = optionx::AccountType::UNKNOWN;
            std::int64_t account_id = 0;
            optionx::CurrencyType currency = optionx::CurrencyType::UNKNOWN;

            bool operator<(const AccountKey& other) const noexcept {
                if (platform_type != other.platform_type) return platform_type < other.platform_type;
                if (account_type != other.account_type) return account_type < other.account_type;
                if (account_id != other.account_id) return account_id < other.account_id;
                return currency < other.currency;
            }
        };

        struct BalanceSnapshotEvent {
            std::int64_t ts = 0;
            std::int64_t open_ts = 0;
            AccountKey account;
            optionx::CurrencyType currency = optionx::CurrencyType::UNKNOWN;
            double open_balance = 0.0;
            bool has_open_balance = false;
            double close_balance = 0.0;
            double profit = 0.0;
        };

        struct OutcomeEvent {
            std::int64_t result_ts = 0;
            std::int64_t decision_ts = 0;
            std::uint32_t trade_id = 0;
            std::int64_t unique_id = 0;
            optionx::TradeState trade_state = optionx::TradeState::UNKNOWN;
        };

        static std::int64_t outcome_result_timestamp(
                const optionx::TradeRecord& rec) noexcept {
            if (rec.close_date > 0) return rec.close_date;
            if (rec.open_date > 0) return rec.open_date;
            return optionx::select_timestamp_ms(rec, optionx::TradeRecordTimeField::AUTO);
        }

        static std::int64_t outcome_decision_timestamp(
                const optionx::TradeRecord& rec,
                std::int64_t result_ts) noexcept {
            if (rec.place_date > 0) return rec.place_date;
            if (rec.send_date > 0) return rec.send_date;
            if (rec.open_date > 0) return rec.open_date;
            return result_ts;
        }

        static OutcomeEvent make_outcome_event(
                const optionx::TradeRecord& rec) noexcept {
            const auto result_ts = outcome_result_timestamp(rec);
            return {
                result_ts,
                outcome_decision_timestamp(rec, result_ts),
                rec.trade_id,
                rec.unique_id,
                rec.trade_state
            };
        }

        static double close_balance_snapshot(
                const optionx::TradeRecord& rec) noexcept {
            return rec.close_balance;
        }

        static double convert_money(
                double value,
                optionx::CurrencyType from,
                std::int64_t timestamp_ms,
                const optionx::TradeStatsConfig& config) {
            const auto to = config.currency_matrix.base_currency;
            if (config.convert_currency) {
                return config.convert_currency(value, from, to, timestamp_ms);
            }
            if (to != optionx::CurrencyType::UNKNOWN) {
                return config.currency_matrix.convert_to_base(value, from);
            }
            if (config.convert) {
                return config.convert(value, from);
            }
            return value;
        }

        static double initial_balance_for_event(
                const BalanceSnapshotEvent& event,
                const optionx::TradeStatsConfig& config) {
            if (event.has_open_balance) {
                return convert_money(
                    event.open_balance,
                    event.currency,
                    event.open_ts > 0 ? event.open_ts : event.ts,
                    config);
            }

            const double close_balance = convert_money(
                event.close_balance,
                event.currency,
                event.ts,
                config);
            const double profit = convert_money(
                event.profit,
                event.currency,
                event.ts,
                config);
            return close_balance - profit;
        }

        static void append_equity_sample(
                optionx::TradeStats& stats,
                std::int64_t ts,
                double equity,
                double baseline) {
            stats.equity_curve.x_time.push_back(ts);
            stats.equity_curve.y_value.push_back(equity);
            stats.profit_curve.x_time.push_back(ts);
            stats.profit_curve.y_value.push_back(equity - baseline);
            if (baseline > 0.0) {
                stats.profit_percent_curve.x_time.push_back(ts);
                stats.profit_percent_curve.y_value.push_back(
                    ((equity - baseline) / baseline) * 100.0);
            }
        }

        static void update_equity_drawdown(
                optionx::TradeStats& stats,
                std::int64_t ts,
                double equity,
                double& peak) {
            peak = std::max(peak, equity);
            const auto dd = peak - equity;
            if (dd > stats.max_absolute_drawdown) {
                stats.max_absolute_drawdown = dd;
                stats.max_drawdown_date = ts;
            }
            if (peak > 0.0) {
                stats.max_relative_drawdown = std::max(
                    stats.max_relative_drawdown,
                    dd / peak);
            }
        }

        static void build_synthetic_curves(
                optionx::TradeStats& stats,
                const optionx::TradeStatsConfig& config,
                const std::map<std::int64_t, double>& realized_profit_by_ts) {
            double equity = config.start_balance;
            double peak = config.start_balance;
            for (const auto& kv : realized_profit_by_ts) {
                const auto ts = kv.first;
                equity += kv.second;
                append_equity_sample(stats, ts, equity, config.start_balance);
                update_equity_drawdown(stats, ts, equity, peak);
            }
        }

        static void build_record_balance_curves(
                optionx::TradeStats& stats,
                const optionx::TradeStatsConfig& config,
                std::vector<BalanceSnapshotEvent> events) {
            if (events.empty()) return;

            std::stable_sort(
                events.begin(),
                events.end(),
                [](const BalanceSnapshotEvent& lhs, const BalanceSnapshotEvent& rhs) {
                    if (lhs.ts != rhs.ts) return lhs.ts < rhs.ts;
                    return lhs.account < rhs.account;
                });

            bool has_baseline = false;
            double baseline = 0.0;
            double peak = 0.0;

            for (std::size_t i = 0; i < events.size();) {
                const auto ts = events[i].ts;
                double equity = 0.0;
                do {
                    const auto& event = events[i];
                    if (!has_baseline) {
                        baseline = initial_balance_for_event(event, config);
                        peak = baseline;
                        has_baseline = true;
                    }
                    equity = convert_money(
                        event.close_balance,
                        event.currency,
                        event.ts,
                        config);
                    ++i;
                } while (i < events.size() && events[i].ts == ts);

                append_equity_sample(stats, ts, equity, baseline);
                update_equity_drawdown(stats, ts, equity, peak);
            }
        }

        static void build_portfolio_balance_curves(
                optionx::TradeStats& stats,
                const optionx::TradeStatsConfig& config,
                std::vector<BalanceSnapshotEvent> events) {
            if (events.empty()) return;

            std::stable_sort(
                events.begin(),
                events.end(),
                [](const BalanceSnapshotEvent& lhs, const BalanceSnapshotEvent& rhs) {
                    if (lhs.ts != rhs.ts) return lhs.ts < rhs.ts;
                    return lhs.account < rhs.account;
                });

            std::map<AccountKey, double> account_balances;
            double baseline = 0.0;
            double equity = 0.0;
            double peak = 0.0;
            bool has_sample = false;

            for (std::size_t i = 0; i < events.size();) {
                const auto ts = events[i].ts;
                do {
                    const auto& event = events[i];
                    const double close_balance = convert_money(
                        event.close_balance,
                        event.currency,
                        event.ts,
                        config);

                    auto account_it = account_balances.find(event.account);
                    if (account_it == account_balances.end()) {
                        const double initial_balance = initial_balance_for_event(event, config);
                        baseline += initial_balance;
                        equity += initial_balance;
                        account_it = account_balances.emplace(event.account, initial_balance).first;
                        if (!has_sample) {
                            peak = baseline;
                            has_sample = true;
                        }
                    }

                    equity += close_balance - account_it->second;
                    account_it->second = close_balance;
                    ++i;
                } while (i < events.size() && events[i].ts == ts);

                append_equity_sample(stats, ts, equity, baseline);
                update_equity_drawdown(stats, ts, equity, peak);
            }
        }

        static bool include_by_selection(
                const optionx::TradeRecord& rec,
                const optionx::TradeStatsConfig& config) noexcept {
            if (config.selection == optionx::TradeStatsSelection::FIRST_MM_STEP && rec.mm_step != 0) {
                return false;
            }
            if (config.selection == optionx::TradeStatsSelection::LAST_IN_GROUP) {
                return rec.last_in_group();
            }
            return true;
        }

        static void recount_series(
                std::vector<OutcomeEvent>& events,
                optionx::TradeSeriesStats& series) {
            // Series need a sequence. Result time is primary; when several
            // trades resolve at the same moment, use the decision timeline
            // instead of inventing a monetary intra-timestamp order.
            std::stable_sort(
                events.begin(),
                events.end(),
                [](const OutcomeEvent& lhs, const OutcomeEvent& rhs) {
                    if (lhs.result_ts != rhs.result_ts) return lhs.result_ts < rhs.result_ts;
                    if (lhs.decision_ts != rhs.decision_ts) return lhs.decision_ts < rhs.decision_ts;
                    if (lhs.trade_id != rhs.trade_id) return lhs.trade_id < rhs.trade_id;
                    return lhs.unique_id < rhs.unique_id;
                });

            std::uint64_t cur_win = 0, cur_loss = 0;
            std::uint64_t max_w = 0, max_l = 0;
            std::uint64_t count_w = 0, count_l = 0;
            std::uint64_t total_w_len = 0, total_l_len = 0;

            for (const auto& event : events) {
                if (event.trade_state == optionx::TradeState::WIN) {
                    if (cur_loss > 0) {
                        ++count_l;
                        max_l = std::max(max_l, cur_loss);
                        total_l_len += cur_loss;
                        cur_loss = 0;
                    }
                    ++cur_win;
                } else if (event.trade_state == optionx::TradeState::LOSS) {
                    if (cur_win > 0) {
                        ++count_w;
                        max_w = std::max(max_w, cur_win);
                        total_w_len += cur_win;
                        cur_win = 0;
                    }
                    ++cur_loss;
                } else {
                    // Standoff / refund / error: close both series
                    if (cur_win > 0) {
                        ++count_w;
                        max_w = std::max(max_w, cur_win);
                        total_w_len += cur_win;
                        cur_win = 0;
                    }
                    if (cur_loss > 0) {
                        ++count_l;
                        max_l = std::max(max_l, cur_loss);
                        total_l_len += cur_loss;
                        cur_loss = 0;
                    }
                }
            }

            // Close trailing series
            if (cur_win > 0) {
                ++count_w;
                max_w = std::max(max_w, cur_win);
                total_w_len += cur_win;
            }
            if (cur_loss > 0) {
                ++count_l;
                max_l = std::max(max_l, cur_loss);
                total_l_len += cur_loss;
            }

            series.max_win_series = max_w;
            series.max_loss_series = max_l;
            series.total_win_series = count_w;
            series.total_loss_series = count_l;
            series.avg_win_series = (count_w > 0) ? (total_w_len / count_w) : 0;
            series.avg_loss_series = (count_l > 0) ? (total_l_len / count_l) : 0;
            series.current_series = (cur_win > 0) ? cur_win : cur_loss;
            series.current_is_win = (cur_win > 0);
        }
    };

} // namespace optionx::storage

#endif // OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_STATS_CALCULATOR_HPP_INCLUDED
