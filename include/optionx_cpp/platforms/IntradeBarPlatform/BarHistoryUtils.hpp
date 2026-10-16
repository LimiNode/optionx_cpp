#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_BAR_HISTORY_UTILS_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_BAR_HISTORY_UTILS_HPP_INCLUDED

/// \file BarHistoryUtils.hpp
/// \brief Helpers for Intrade Bar historical bar request slicing.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "symbol_utils.hpp"

namespace optionx::platforms::intrade_bar {

    /// \brief Maximum bars requested from the Intrade FX history endpoint at once.
    inline constexpr std::int64_t FX_HISTORY_MAX_BARS_PER_REQUEST = 1000;

    /// \brief Maximum klines requested from Binance at once.
    /// \details Binance documents 1000 as the maximum kline limit.
    inline constexpr std::int64_t BINANCE_KLINES_MAX_BARS_PER_REQUEST = 1000;

    /// \brief Known lower bound for historical bars of one symbol.
    struct BarHistoryStartLimit {
        const char* symbol = "";          ///< Normalized symbol.
        std::int64_t min_from_ts = 0;     ///< Earliest known timestamp in seconds.
    };

    /// \brief Empirical FX history lower bounds from the old Intrade history dataset.
    inline constexpr BarHistoryStartLimit FX_HISTORY_START_LIMITS[] = {
        {"EURUSD", 1007337600}, // 03.12.2001
        {"USDJPY", 1007337600}, // 03.12.2001
        {"GBPUSD", 1007337600}, // 03.12.2001
        {"USDCHF", 1007337600}, // 03.12.2001
        {"USDCAD", 1007424000}, // 04.12.2001
        {"EURJPY", 1006992000}, // 29.11.2001
        {"AUDUSD", 1039824000}, // 14.12.2002
        {"NZDUSD", 1007424000}, // 04.12.2001
        {"EURGBP", 1012521600}, // 01.02.2002
        {"EURCHF", 1007596800}, // 06.12.2001
        {"AUDJPY", 1006905600}, // 28.11.2001
        {"GBPJPY", 1006905600}, // 28.11.2001
        {"CHFJPY", 1007337600}, // 03.12.2001
        {"EURCAD", 1007078400}, // 30.11.2001
        {"AUDCAD", 1059523200}, // 30.07.2003
        {"CADJPY", 1006992000}, // 29.11.2001
        {"NZDJPY", 1006992000}, // 29.11.2001
        {"AUDNZD", 1006992000}, // 29.11.2001
        {"GBPAUD", 1006992000}, // 29.11.2001
        {"EURAUD", 1006992000}, // 29.11.2001
        {"GBPCHF", 1007078400}, // 30.11.2001
        {"EURNZD", 1206921600}, // 31.03.2008
        {"AUDCHF", 1006992000}, // 29.11.2001
        {"GBPNZD", 1007078400}, // 30.11.2001
        {"GBPCAD", 1006992000}, // 29.11.2001
        {"XAUUSD", 1254096000}  // 28.09.2009
    };

    /// \brief Inclusive time range for one backend request.
    struct BarHistoryChunk {
        std::int64_t from_ts = 0; ///< Chunk start timestamp in seconds.
        std::int64_t to_ts = 0;   ///< Chunk end timestamp in seconds.
    };

    /// \brief Checks whether historical bars for a symbol must be requested from Binance.
    /// \param symbol Public or normalized symbol.
    /// \return True when the symbol maps to BTCUSDT.
    inline bool uses_binance_bar_history(const std::string& symbol) {
        return is_btc_symbol(symbol);
    }

    /// \brief Converts a normalized FX symbol to the slash form expected by /fxhis.
    /// \param symbol Public or normalized symbol.
    /// \return Broker query symbol such as "NZD/USD".
    inline std::string fx_history_query_symbol(const std::string& symbol) {
        const auto normalized = normalize_symbol_name(symbol);
        if (normalized.size() == 6) {
            return normalized.substr(0, 3) + "/" + normalized.substr(3, 3);
        }
        return normalized;
    }

    /// \brief Returns an empirically known earliest history timestamp.
    /// \param symbol Public or normalized symbol.
    /// \return Minimum supported timestamp in seconds, or 0 when no local limit is known.
    inline std::int64_t minimum_bar_history_from_ts(const std::string& symbol) {
        const auto normalized = normalize_symbol_name(symbol);

        // Binance BTCUSDT history begins in August 2017.
        if (normalized == "BTCUSDT") return 1502942400;

        for (const auto& item : FX_HISTORY_START_LIMITS) {
            if (normalized == item.symbol) return item.min_from_ts;
        }

        return 0;
    }

    /// \brief Converts a timeframe in seconds to the Binance kline interval.
    /// \param timeframe_sec Requested timeframe in seconds.
    /// \return Binance interval string, or an empty string when unsupported.
    inline std::string binance_interval_from_timeframe(std::int64_t timeframe_sec) {
        switch (timeframe_sec) {
        case 60: return "1m";
        case 180: return "3m";
        case 300: return "5m";
        case 900: return "15m";
        case 1800: return "30m";
        case 3600: return "1h";
        case 7200: return "2h";
        case 14400: return "4h";
        case 21600: return "6h";
        case 28800: return "8h";
        case 43200: return "12h";
        case 86400: return "1d";
        case 259200: return "3d";
        case 604800: return "1w";
        default:
            return {};
        }
    }

    /// \brief Splits an inclusive history range into backend-sized chunks.
    /// \param from_ts Inclusive start timestamp in seconds.
    /// \param to_ts Inclusive end timestamp in seconds.
    /// \param timeframe_sec Bar timeframe in seconds.
    /// \param max_bars_per_request Backend limit for one request.
    /// \return Ordered chunks without duplicated bar timestamps.
    inline std::vector<BarHistoryChunk> build_bar_history_chunks(
            std::int64_t from_ts,
            std::int64_t to_ts,
            std::int64_t timeframe_sec,
            std::int64_t max_bars_per_request) {
        std::vector<BarHistoryChunk> chunks;
        if (from_ts <= 0 ||
            to_ts < from_ts ||
            timeframe_sec <= 0 ||
            max_bars_per_request <= 0) {
            return chunks;
        }

        const auto chunk_span =
            timeframe_sec * std::max<std::int64_t>(0, max_bars_per_request - 1);
        for (auto current = from_ts; current <= to_ts;) {
            const auto remaining = to_ts - current;
            const auto current_to = current + std::min(remaining, chunk_span);
            chunks.push_back({current, current_to});

            if (current_to > (std::numeric_limits<std::int64_t>::max)() - timeframe_sec) {
                break;
            }
            current = current_to + timeframe_sec;
        }

        return chunks;
    }

    /// \brief Checks a response against a dense bar-history request.
    /// \details This proves only a dense timeframe grid. It deliberately does
    ///          not infer that missing timestamps belong to a closed session.
    /// \param sequence Parsed bars returned for the request.
    /// \param request Original public history request.
    /// \return True when every requested dense slot is present exactly once.
    inline bool bar_sequence_covers_dense_range(
            const BarSequence& sequence,
            const BarHistoryRequest& request) noexcept {
        if (request.symbol.empty() ||
            request.timeframe <= 0 ||
            request.from_ts <= 0 ||
            request.to_ts < request.from_ts ||
            sequence.timeframe != request.timeframe ||
            sequence.symbol.empty() ||
            normalize_symbol_name(sequence.symbol) != normalize_symbol_name(request.symbol)) {
            return false;
        }

        const auto from_ts = static_cast<std::uint64_t>(request.from_ts);
        const auto to_ts = static_cast<std::uint64_t>(request.to_ts);
        const auto timeframe = static_cast<std::uint64_t>(request.timeframe);
        if (timeframe == 0 ||
            from_ts > (std::numeric_limits<std::uint64_t>::max)() / 1000U ||
            to_ts > (std::numeric_limits<std::uint64_t>::max)() / 1000U) {
            return false;
        }

        auto expected_time_ms = from_ts * 1000U;
        const auto step_ms = timeframe >
                (std::numeric_limits<std::uint64_t>::max)() / 1000U
            ? 0U
            : timeframe * 1000U;
        if (step_ms == 0 || sequence.bars.empty()) return false;

        const auto expected_count =
            (to_ts - from_ts) / timeframe + 1U;
        if (sequence.bars.size() != expected_count) return false;

        for (std::size_t index = 0; index < sequence.bars.size(); ++index) {
            const auto& bar = sequence.bars[index];
            if (bar.time_ms != expected_time_ms) return false;
            if (index + 1 < sequence.bars.size() &&
                expected_time_ms >
                    (std::numeric_limits<std::uint64_t>::max)() - step_ms) {
                return false;
            }
            if (index + 1 < sequence.bars.size()) {
                expected_time_ms += step_ms;
            }
        }
        return true;
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_BAR_HISTORY_UTILS_HPP_INCLUDED
