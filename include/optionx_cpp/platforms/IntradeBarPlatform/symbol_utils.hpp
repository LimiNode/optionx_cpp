#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_SYMBOL_UTILS_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_SYMBOL_UTILS_HPP_INCLUDED

/// \file symbol_utils.hpp
/// \brief Broker-specific symbol normalization helpers for Intrade Bar.

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace optionx::platforms::intrade_bar {

    /// \brief Returns the symbols supported by the Intrade Bar trading model.
    /// \return Stable list of normalized broker symbol names.
    inline const std::array<const char*, 22>& supported_symbols() noexcept {
        static constexpr std::array<const char*, 22> symbols = {{
            "AUDCAD", "AUDCHF", "AUDJPY", "AUDNZD", "AUDUSD",
            "CADJPY",
            "EURAUD", "EURCAD", "EURCHF", "EURGBP", "EURJPY", "EURUSD",
            "GBPAUD", "GBPCHF", "GBPJPY", "GBPNZD",
            "NZDJPY", "NZDUSD",
            "USDCAD", "USDCHF", "USDJPY",
            "BTCUSDT"
        }};
        return symbols;
    }

    /// \brief Converts public symbol aliases to broker-side Intrade Bar names.
    /// \param symbol Public or broker symbol name.
    /// \return Normalized broker symbol name.
    inline std::string normalize_symbol_name(std::string symbol) {
        symbol.erase(std::remove_if(
            symbol.begin(),
            symbol.end(),
            [](unsigned char ch) {
                return ch == '/' || std::isspace(ch) != 0;
            }), symbol.end());
        for (auto& ch : symbol) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if (symbol == "BTCUSD") return "BTCUSDT";
        return symbol;
    }

    /// \brief Checks whether a public or broker symbol refers to the BTCUSDT instrument.
    /// \param symbol Public or broker symbol name.
    /// \return True for BTCUSD/BTCUSDT aliases.
    inline bool is_btc_symbol(const std::string& symbol) {
        return normalize_symbol_name(symbol) == "BTCUSDT";
    }

    /// \brief Checks whether a symbol is supported by the Intrade Bar trading model.
    /// \param symbol Public or broker symbol name.
    /// \return True for a known FX or BTC symbol.
    inline bool is_supported_symbol(const std::string& symbol) {
        const auto normalized = normalize_symbol_name(symbol);
        for (const auto* item : supported_symbols()) {
            if (normalized == item) return true;
        }
        return false;
    }

    /// \brief Checks whether `/fxconnect` is expected to support this FX symbol.
    /// \param symbol Public or broker symbol name.
    /// \return True for known Intrade Bar FX websocket symbols.
    inline bool is_fxconnect_supported_symbol(const std::string& symbol) {
        const auto normalized = normalize_symbol_name(symbol);
        return normalized != "BTCUSDT" && is_supported_symbol(normalized);
    }

    /// \brief Converts a normalized FX symbol to the `/fxconnect` stream format.
    /// \param symbol Public or broker symbol name.
    /// \return Slash-separated stream symbol, such as `EUR/USD`; empty for non-FX symbols.
    inline std::string make_fxconnect_symbol(const std::string& symbol) {
        const auto normalized = normalize_symbol_name(symbol);
        if (!is_fxconnect_supported_symbol(normalized)) {
            return {};
        }
        return normalized.substr(0, 3) + "/" + normalized.substr(3, 3);
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_SYMBOL_UTILS_HPP_INCLUDED
