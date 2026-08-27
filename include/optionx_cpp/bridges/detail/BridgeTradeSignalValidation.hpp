#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_DETAIL_BRIDGE_TRADE_SIGNAL_VALIDATION_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_DETAIL_BRIDGE_TRADE_SIGNAL_VALIDATION_HPP_INCLUDED

/// \file BridgeTradeSignalValidation.hpp
/// \brief Defines shared validation helpers for executable bridge trade signals.

#include <optionx_cpp/data/trading.hpp>

#include <cmath>
#include <stdexcept>
#include <string>

namespace optionx::bridges::detail {

    /// \brief Returns a copy without leading/trailing ASCII whitespace.
    inline std::string trim_ascii_copy(const std::string& value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    /// \brief Validates a ratio-style trade signal field.
    /// \param value Field value.
    /// \param field_name Field name used in diagnostics.
    inline void validate_trade_signal_ratio(
            const double value,
            const char* field_name) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument(std::string(field_name) + " must be finite.");
        }
        if (value < 0.0 || value > 1.0) {
            throw std::invalid_argument(std::string(field_name) + " must be in the 0..1 range.");
        }
    }

    /// \brief Validates a trade signal that a bridge is about to dispatch as executable intent.
    /// \param signal Signal to validate.
    /// \param context Diagnostic prefix, for example `BotBinary signal`.
    /// \throws std::invalid_argument when the signal is not executable.
    inline void validate_executable_trade_signal(
            const TradeSignal& signal,
            const char* context = "Bridge signal",
            const bool require_positive_amount = true) {
        const std::string prefix = context ? context : "Bridge signal";
        if (trim_ascii_copy(signal.symbol).empty()) {
            throw std::invalid_argument(prefix + " symbol is required.");
        }
        switch (signal.order_type) {
        case OrderType::BUY:
        case OrderType::SELL:
            break;
        default:
            throw std::invalid_argument(prefix + " order_type must be BUY or SELL.");
        }
        switch (signal.option_type) {
        case OptionType::SPRINT:
        case OptionType::CLASSIC:
            break;
        default:
            throw std::invalid_argument(prefix + " option_type must be SPRINT or CLASSIC.");
        }
        if (!std::isfinite(signal.amount) || signal.amount < 0.0) {
            throw std::invalid_argument(prefix + " amount must be finite and non-negative.");
        }
        if (require_positive_amount && signal.amount == 0.0) {
            throw std::invalid_argument(prefix + " amount must be positive.");
        }
        validate_trade_signal_ratio(signal.refund, "refund");
        validate_trade_signal_ratio(signal.min_payout, "min_payout");
        if (signal.expiry_time < 0) {
            throw std::invalid_argument(prefix + " expiry_time must not be negative.");
        }
        const bool has_duration = signal.duration != 0;
        const bool has_expiry_time = signal.expiry_time > 0;
        if (has_duration == has_expiry_time) {
            throw std::invalid_argument(prefix + " requires exactly one expiry representation.");
        }
    }

} // namespace optionx::bridges::detail

#endif // OPTIONX_HEADER_BRIDGES_DETAIL_BRIDGE_TRADE_SIGNAL_VALIDATION_HPP_INCLUDED
