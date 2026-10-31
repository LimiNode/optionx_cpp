#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_FILTERS_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_FILTERS_HPP_INCLUDED

/// \file Filters.hpp
/// \brief Defines pure symbol, source and trading-window filters.

#include "DecisionContext.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace optionx::trading_system {

/// \brief Half-open local-time interval in seconds from midnight.
///
/// `start_second` is inclusive and `end_second` is exclusive. An interval may
/// cross midnight when start is greater than end. `0..86400` represents a full
/// day; equal endpoints are rejected to avoid an ambiguous empty/full meaning.
struct TradingTimeWindow {
    std::uint32_t start_second = 0;
    std::uint32_t end_second = 0;

    bool valid() const noexcept {
        return start_second < 86400 &&
               end_second > 0 &&
               end_second <= 86400 &&
               start_second != end_second;
    }

    bool contains(std::uint32_t second_of_day) const noexcept {
        if (!valid() || second_of_day >= 86400) return false;
        if (start_second < end_second) {
            return second_of_day >= start_second && second_of_day < end_second;
        }
        return second_of_day >= start_second || second_of_day < end_second;
    }
};

/// \brief Normalizes a generic symbol by removing separators and uppercasing it.
inline std::string normalize_symbol(std::string symbol) {
    symbol.erase(std::remove_if(
        symbol.begin(),
        symbol.end(),
        [](unsigned char ch) {
            return ch == '/' || std::isspace(ch) != 0;
        }), symbol.end());
    for (auto& ch : symbol) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return symbol;
}

using SymbolNormalizer = std::function<std::string(std::string)>;

/// \brief Allows or rejects normalized symbols without owning broker knowledge.
///
/// A caller may provide a broker-specific normalizer, such as the existing
/// Intrade Bar alias helper, and an explicit known-symbol set. The default
/// policy only performs generic separator/case normalization.
class SymbolFilter {
public:
    SymbolFilter(
        std::vector<std::string> allowed_symbols = {},
        std::vector<std::string> denied_symbols = {},
        std::vector<std::string> known_symbols = {},
        SymbolNormalizer normalizer = {})
        : m_normalizer(std::move(normalizer)) {
        if (!m_normalizer) {
            m_normalizer = normalize_symbol;
        }
        for (auto& symbol : allowed_symbols) {
            m_allowed_symbols.push_back(m_normalizer(std::move(symbol)));
        }
        for (auto& symbol : denied_symbols) {
            m_denied_symbols.push_back(m_normalizer(std::move(symbol)));
        }
        for (auto& symbol : known_symbols) {
            m_known_symbols.push_back(m_normalizer(std::move(symbol)));
        }
    }

    PolicyDecision evaluate(const TradingDecisionContext& context) const {
        if (context.symbol.empty()) {
            return PolicyDecision::unavailable(PolicyReason::MISSING_SYMBOL);
        }

        const auto normalized = m_normalizer(context.symbol);
        if (normalized.empty()) {
            return PolicyDecision::unavailable(PolicyReason::MISSING_SYMBOL);
        }
        if (!m_known_symbols.empty() && !contains(m_known_symbols, normalized)) {
            return PolicyDecision::unavailable(PolicyReason::UNKNOWN_SYMBOL);
        }
        if (!m_allowed_symbols.empty() && !contains(m_allowed_symbols, normalized)) {
            return PolicyDecision::reject(PolicyReason::SYMBOL_NOT_ALLOWED);
        }
        if (contains(m_denied_symbols, normalized)) {
            return PolicyDecision::reject(PolicyReason::SYMBOL_DENIED);
        }
        return PolicyDecision::allow();
    }

private:
    static bool contains(const std::vector<std::string>& values, const std::string& value) {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    std::vector<std::string> m_allowed_symbols;
    std::vector<std::string> m_denied_symbols;
    std::vector<std::string> m_known_symbols;
    SymbolNormalizer m_normalizer;
};

/// \brief Filters signals by bridge identity and/or exact signal name.
class SourceFilter {
public:
    SourceFilter(
        std::vector<BridgeId> allowed_bridge_ids = {},
        std::vector<BridgeId> denied_bridge_ids = {},
        std::vector<std::string> allowed_signal_names = {},
        std::vector<std::string> denied_signal_names = {})
        : m_allowed_bridge_ids(std::move(allowed_bridge_ids)),
          m_denied_bridge_ids(std::move(denied_bridge_ids)),
          m_allowed_signal_names(std::move(allowed_signal_names)),
          m_denied_signal_names(std::move(denied_signal_names)) {}

    PolicyDecision evaluate(const TradingDecisionContext& context) const {
        if (!m_allowed_bridge_ids.empty() && context.bridge_id == 0) {
            return PolicyDecision::unavailable(PolicyReason::MISSING_SOURCE_ID);
        }
        if (!m_allowed_signal_names.empty() && context.signal_name.empty()) {
            return PolicyDecision::unavailable(PolicyReason::MISSING_SIGNAL_NAME);
        }
        if (contains(m_denied_bridge_ids, context.bridge_id)) {
            return PolicyDecision::reject(PolicyReason::SOURCE_DENIED);
        }
        if (!m_allowed_bridge_ids.empty() &&
            !contains(m_allowed_bridge_ids, context.bridge_id)) {
            return PolicyDecision::reject(PolicyReason::SOURCE_NOT_ALLOWED);
        }
        if (contains(m_denied_signal_names, context.signal_name)) {
            return PolicyDecision::reject(PolicyReason::SIGNAL_NAME_DENIED);
        }
        if (!m_allowed_signal_names.empty() &&
            !contains(m_allowed_signal_names, context.signal_name)) {
            return PolicyDecision::reject(PolicyReason::SIGNAL_NAME_NOT_ALLOWED);
        }
        return PolicyDecision::allow();
    }

private:
    static bool contains(const std::vector<BridgeId>& values, BridgeId value) {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    static bool contains(const std::vector<std::string>& values, const std::string& value) {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    std::vector<BridgeId> m_allowed_bridge_ids;
    std::vector<BridgeId> m_denied_bridge_ids;
    std::vector<std::string> m_allowed_signal_names;
    std::vector<std::string> m_denied_signal_names;
};

/// \brief Filters local time and weekday using the timezone in the context.
class TradingWindowFilter {
public:
    TradingWindowFilter(
        std::vector<TradingTimeWindow> allowed_windows = {},
        std::vector<TradingTimeWindow> denied_windows = {},
        std::vector<time_shield::Weekday> allowed_weekdays = {})
        : m_allowed_windows(std::move(allowed_windows)),
          m_denied_windows(std::move(denied_windows)),
          m_allowed_weekdays(std::move(allowed_weekdays)) {
        for (const auto& window : m_allowed_windows) m_valid &= window.valid();
        for (const auto& window : m_denied_windows) m_valid &= window.valid();
        for (const auto weekday : m_allowed_weekdays) {
            if (weekday < time_shield::SUN || weekday > time_shield::SAT) {
                m_valid = false;
            }
        }
    }

    PolicyDecision evaluate(const TradingDecisionContext& context) const {
        if (!m_valid) {
            return PolicyDecision::unavailable(PolicyReason::INVALID_CONFIGURATION);
        }
        if (!context.evaluation_time_ms.has_value()) {
            return PolicyDecision::unavailable(PolicyReason::MISSING_EVALUATION_TIME);
        }

        const auto local_ms = context.time_zone.to_local_ms(*context.evaluation_time_ms);
        const auto local_sec = time_shield::ms_to_sec<time_shield::ts_t>(local_ms);
        const auto second_of_day = static_cast<std::uint32_t>(
            time_shield::sec_of_day(local_sec));
        const auto weekday = time_shield::weekday_of_ts(local_sec);

        if (!m_allowed_weekdays.empty() &&
            !contains(m_allowed_weekdays, weekday)) {
            return PolicyDecision::reject(PolicyReason::WEEKDAY_NOT_ALLOWED);
        }
        if (!m_allowed_windows.empty() &&
            !contains(m_allowed_windows, second_of_day)) {
            return PolicyDecision::reject(PolicyReason::TIME_NOT_ALLOWED);
        }
        if (contains(m_denied_windows, second_of_day)) {
            return PolicyDecision::reject(PolicyReason::TIME_DENIED);
        }
        return PolicyDecision::allow();
    }

private:
    static bool contains(
        const std::vector<TradingTimeWindow>& windows,
        std::uint32_t second_of_day) noexcept {
        return std::any_of(
            windows.begin(), windows.end(),
            [second_of_day](const auto& window) {
                return window.contains(second_of_day);
            });
    }

    static bool contains(
        const std::vector<time_shield::Weekday>& weekdays,
        time_shield::Weekday weekday) noexcept {
        return std::find(weekdays.begin(), weekdays.end(), weekday) != weekdays.end();
    }

    std::vector<TradingTimeWindow> m_allowed_windows;
    std::vector<TradingTimeWindow> m_denied_windows;
    std::vector<time_shield::Weekday> m_allowed_weekdays;
    bool m_valid = true;
};

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_FILTERS_HPP_INCLUDED
