#pragma once
#ifndef OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_META_STATS_CALCULATOR_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_META_STATS_CALCULATOR_HPP_INCLUDED

/// \file TradeMetaStatsCalculator.hpp
/// \brief Computes meta-statistics (available values + per-value breakdowns) over trade records.

#include <memory>
#include <set>
#include <vector>

#include <optionx_cpp/data/trading.hpp>
#include "TradeStatsCalculator.hpp"

namespace optionx::storage {

    /// \class TradeMetaStatsCalculator
    /// \brief Static calculator producing per-dimension statistics (meta-analysis).
    class TradeMetaStatsCalculator {
    public:
        /// \brief Calculates meta-statistics from a collection of trade records.
        /// \param records Input trade records.
        /// \param config Optional filtering and conversion configuration.
        /// \return Populated TradeMetaStats structure.
        static optionx::TradeMetaStats calc(
                const std::vector<optionx::TradeRecord>& records,
                const optionx::TradeStatsConfig& config = {}) {
            optionx::TradeMetaStats meta{};

            // Collect unique values across all records (respecting the base filter).
            std::set<optionx::PlatformType> platforms_set;
            std::set<optionx::AccountType> accounts_set;
            std::set<optionx::CurrencyType> currencies_set;
            std::set<std::string> symbols_set;
            std::set<std::string> signals_set;
            std::set<std::uint32_t> durations_set;

            for (const auto& rec : records) {
                if (!TradeRecordFilterMatcher::match_filter(rec, config.filter, config.time_zone)) continue;
                if (!config.include_errors && optionx::is_error_trade_state(rec.trade_state)) continue;
                if (!config.include_non_terminal && !optionx::is_terminal_trade_state(rec.trade_state)) continue;

                platforms_set.insert(rec.platform_type);
                accounts_set.insert(rec.account_type);
                currencies_set.insert(rec.currency);
                if (!rec.symbol.empty()) symbols_set.insert(rec.symbol);
                if (!rec.signal_name.empty()) signals_set.insert(rec.signal_name);
                if (rec.duration > 0) durations_set.insert(rec.duration);

                if (rec.account_type == optionx::AccountType::DEMO) meta.has_demo = true;
                if (rec.account_type == optionx::AccountType::REAL) meta.has_real = true;
            }

            meta.platforms.assign(platforms_set.begin(), platforms_set.end());
            meta.accounts.assign(accounts_set.begin(), accounts_set.end());
            meta.currencies.assign(currencies_set.begin(), currencies_set.end());
            meta.symbols.assign(symbols_set.begin(), symbols_set.end());
            meta.signals.assign(signals_set.begin(), signals_set.end());
            meta.durations.assign(durations_set.begin(), durations_set.end());

            // Compute per-platform stats
            for (const auto& pt : meta.platforms) {
                auto cfg = config;
                cfg.filter.platforms.include.clear();
                cfg.filter.platforms.add_include(pt);
                meta.platform_stats.push_back(std::move(*TradeStatsCalculator::calc(records, cfg)));
            }

            // Per-account stats
            for (const auto& at : meta.accounts) {
                auto cfg = config;
                cfg.filter.accounts.include.clear();
                cfg.filter.accounts.add_include(at);
                meta.account_stats.push_back(std::move(*TradeStatsCalculator::calc(records, cfg)));
            }

            // Per-currency stats
            for (const auto& ct : meta.currencies) {
                auto cfg = config;
                cfg.filter.currencies.include.clear();
                cfg.filter.currencies.add_include(ct);
                meta.currency_stats.push_back(std::move(*TradeStatsCalculator::calc(records, cfg)));
            }

            // Per-symbol stats
            for (const auto& sym : meta.symbols) {
                auto cfg = config;
                cfg.filter.symbols.include.clear();
                cfg.filter.symbols.add_include(sym);
                meta.symbol_stats.push_back(std::move(*TradeStatsCalculator::calc(records, cfg)));
            }

            // Per-signal stats
            for (const auto& sig : meta.signals) {
                auto cfg = config;
                cfg.filter.signals.include.clear();
                cfg.filter.signals.add_include(sig);
                meta.signal_stats.push_back(std::move(*TradeStatsCalculator::calc(records, cfg)));
            }

            // Per-duration stats
            for (const auto& dur : meta.durations) {
                auto cfg = config;
                cfg.filter.durations.include.clear();
                cfg.filter.durations.add_include(dur);
                meta.duration_stats.push_back(std::move(*TradeStatsCalculator::calc(records, cfg)));
            }

            // Hourly stats (0..23)
            for (std::uint32_t h = 0; h < 24; ++h) {
                auto cfg = config;
                cfg.filter.hours.include.clear();
                cfg.filter.hours.add_include(h);
                meta.hour_stats.push_back(std::move(*TradeStatsCalculator::calc(records, cfg)));
            }

            // Weekday stats (0=Sunday .. 6=Saturday)
            for (std::uint32_t wd = 0; wd < 7; ++wd) {
                auto cfg = config;
                cfg.filter.weekdays.include.clear();
                cfg.filter.weekdays.add_include(wd);
                meta.weekday_stats.push_back(std::move(*TradeStatsCalculator::calc(records, cfg)));
            }

            return meta;
        }
    };

} // namespace optionx::storage

#endif // OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_META_STATS_CALCULATOR_HPP_INCLUDED
