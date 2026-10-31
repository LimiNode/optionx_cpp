#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_POLICY_TYPES_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_POLICY_TYPES_HPP_INCLUDED

/// \file PolicyTypes.hpp
/// \brief Defines deterministic policy decisions and fixed-point money values.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace optionx::trading_system {

enum class DataAvailability {
    SUPPORTED,
    UNAVAILABLE,
    STALE,
    UNKNOWN
};

enum class PolicyDecisionKind {
    ALLOW,
    REJECT,
    UNAVAILABLE
};

enum class PolicyReason {
    NONE,
    MATCHED,
    INVALID_CONTEXT,
    INVALID_CONFIGURATION,
    MISSING_SYMBOL,
    UNKNOWN_SYMBOL,
    SYMBOL_NOT_ALLOWED,
    SYMBOL_DENIED,
    MISSING_SOURCE_ID,
    SOURCE_NOT_ALLOWED,
    SOURCE_DENIED,
    MISSING_SIGNAL_NAME,
    SIGNAL_NAME_NOT_ALLOWED,
    SIGNAL_NAME_DENIED,
    MISSING_EVALUATION_TIME,
    WEEKDAY_NOT_ALLOWED,
    TIME_NOT_ALLOWED,
    TIME_DENIED
};

inline const char* to_string(DataAvailability value) noexcept {
    switch (value) {
        case DataAvailability::SUPPORTED: return "SUPPORTED";
        case DataAvailability::UNAVAILABLE: return "UNAVAILABLE";
        case DataAvailability::STALE: return "STALE";
        case DataAvailability::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

inline const char* to_string(PolicyDecisionKind value) noexcept {
    switch (value) {
        case PolicyDecisionKind::ALLOW: return "ALLOW";
        case PolicyDecisionKind::REJECT: return "REJECT";
        case PolicyDecisionKind::UNAVAILABLE: return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}

inline const char* to_string(PolicyReason value) noexcept {
    switch (value) {
        case PolicyReason::NONE: return "NONE";
        case PolicyReason::MATCHED: return "MATCHED";
        case PolicyReason::INVALID_CONTEXT: return "INVALID_CONTEXT";
        case PolicyReason::INVALID_CONFIGURATION: return "INVALID_CONFIGURATION";
        case PolicyReason::MISSING_SYMBOL: return "MISSING_SYMBOL";
        case PolicyReason::UNKNOWN_SYMBOL: return "UNKNOWN_SYMBOL";
        case PolicyReason::SYMBOL_NOT_ALLOWED: return "SYMBOL_NOT_ALLOWED";
        case PolicyReason::SYMBOL_DENIED: return "SYMBOL_DENIED";
        case PolicyReason::MISSING_SOURCE_ID: return "MISSING_SOURCE_ID";
        case PolicyReason::SOURCE_NOT_ALLOWED: return "SOURCE_NOT_ALLOWED";
        case PolicyReason::SOURCE_DENIED: return "SOURCE_DENIED";
        case PolicyReason::MISSING_SIGNAL_NAME: return "MISSING_SIGNAL_NAME";
        case PolicyReason::SIGNAL_NAME_NOT_ALLOWED: return "SIGNAL_NAME_NOT_ALLOWED";
        case PolicyReason::SIGNAL_NAME_DENIED: return "SIGNAL_NAME_DENIED";
        case PolicyReason::MISSING_EVALUATION_TIME: return "MISSING_EVALUATION_TIME";
        case PolicyReason::WEEKDAY_NOT_ALLOWED: return "WEEKDAY_NOT_ALLOWED";
        case PolicyReason::TIME_NOT_ALLOWED: return "TIME_NOT_ALLOWED";
        case PolicyReason::TIME_DENIED: return "TIME_DENIED";
    }
    return "INVALID_CONTEXT";
}

struct PolicyDecision {
    PolicyDecisionKind kind = PolicyDecisionKind::UNAVAILABLE;
    PolicyReason reason = PolicyReason::INVALID_CONTEXT;

    static PolicyDecision allow(PolicyReason reason = PolicyReason::MATCHED) noexcept {
        return {PolicyDecisionKind::ALLOW, reason};
    }

    static PolicyDecision reject(PolicyReason reason) noexcept {
        return {PolicyDecisionKind::REJECT, reason};
    }

    static PolicyDecision unavailable(PolicyReason reason) noexcept {
        return {PolicyDecisionKind::UNAVAILABLE, reason};
    }

    bool is_allowed() const noexcept {
        return kind == PolicyDecisionKind::ALLOW;
    }
};

enum class SizingDecisionKind {
    SIZED,
    REJECTED,
    UNAVAILABLE
};

enum class SizingReason {
    NONE,
    SIZED,
    INVALID_CONFIGURATION,
    INVALID_AMOUNT,
    INVALID_PERCENT,
    UNKNOWN_CURRENCY,
    CURRENCY_MISMATCH,
    BALANCE_UNAVAILABLE,
    BALANCE_STALE,
    BALANCE_UNKNOWN,
    MISSING_BALANCE,
    ZERO_BALANCE,
    NEGATIVE_BALANCE,
    ZERO_RESULT,
    PRECISION_MISMATCH,
    NUMERIC_OVERFLOW
};

inline const char* to_string(SizingDecisionKind value) noexcept {
    switch (value) {
        case SizingDecisionKind::SIZED: return "SIZED";
        case SizingDecisionKind::REJECTED: return "REJECTED";
        case SizingDecisionKind::UNAVAILABLE: return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}

inline const char* to_string(SizingReason value) noexcept {
    switch (value) {
        case SizingReason::NONE: return "NONE";
        case SizingReason::SIZED: return "SIZED";
        case SizingReason::INVALID_CONFIGURATION: return "INVALID_CONFIGURATION";
        case SizingReason::INVALID_AMOUNT: return "INVALID_AMOUNT";
        case SizingReason::INVALID_PERCENT: return "INVALID_PERCENT";
        case SizingReason::UNKNOWN_CURRENCY: return "UNKNOWN_CURRENCY";
        case SizingReason::CURRENCY_MISMATCH: return "CURRENCY_MISMATCH";
        case SizingReason::BALANCE_UNAVAILABLE: return "BALANCE_UNAVAILABLE";
        case SizingReason::BALANCE_STALE: return "BALANCE_STALE";
        case SizingReason::BALANCE_UNKNOWN: return "BALANCE_UNKNOWN";
        case SizingReason::MISSING_BALANCE: return "MISSING_BALANCE";
        case SizingReason::ZERO_BALANCE: return "ZERO_BALANCE";
        case SizingReason::NEGATIVE_BALANCE: return "NEGATIVE_BALANCE";
        case SizingReason::ZERO_RESULT: return "ZERO_RESULT";
        case SizingReason::PRECISION_MISMATCH: return "PRECISION_MISMATCH";
        case SizingReason::NUMERIC_OVERFLOW: return "NUMERIC_OVERFLOW";
    }
    return "INVALID_CONFIGURATION";
}

namespace detail {

inline constexpr std::uint8_t max_money_precision = 18;

inline constexpr std::uint64_t power_of_ten(std::uint8_t precision) noexcept {
    std::uint64_t result = 1;
    for (std::uint8_t i = 0; i < precision; ++i) result *= 10;
    return result;
}

} // namespace detail

/// \brief A non-negative or signed fixed-point monetary value.
///
/// `units` stores the amount multiplied by 10^`precision`. Policies operate on
/// this representation and never round a broker amount implicitly from double.
struct MoneyAmount {
    std::int64_t units = 0;
    std::uint8_t precision = 2;
    CurrencyType currency = CurrencyType::UNKNOWN;

    bool valid_metadata() const noexcept {
        return precision <= detail::max_money_precision &&
               currency != CurrencyType::UNKNOWN;
    }

    /// \brief Parses a decimal amount without discarding significant digits.
    ///
    /// Extra fractional digits are accepted only when they are zero. This
    /// makes conversion to fixed precision explicit instead of silently
    /// applying a rounding rule.
    static std::optional<MoneyAmount> from_decimal(
        const std::string& text,
        CurrencyType currency,
        std::uint8_t precision = 2) {
        if (text.empty() || precision > detail::max_money_precision ||
            currency == CurrencyType::UNKNOWN) {
            return std::nullopt;
        }

        std::size_t position = 0;
        bool negative = false;
        if (text[position] == '+' || text[position] == '-') {
            negative = text[position] == '-';
            ++position;
            if (position == text.size()) return std::nullopt;
        }

        const auto dot = text.find('.', position);
        if (dot != std::string::npos && text.find('.', dot + 1) != std::string::npos) {
            return std::nullopt;
        }

        const auto integer_end = dot == std::string::npos ? text.size() : dot;
        const auto fraction_begin = dot == std::string::npos ? text.size() : dot + 1;
        const auto fraction_size = text.size() - fraction_begin;
        if (integer_end == position && fraction_size == 0) return std::nullopt;

        std::uint64_t whole = 0;
        for (std::size_t i = position; i < integer_end; ++i) {
            const auto ch = static_cast<unsigned char>(text[i]);
            if (ch < '0' || ch > '9') return std::nullopt;
            const auto digit = static_cast<std::uint64_t>(ch - '0');
            if (whole > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                return std::nullopt;
            }
            whole = whole * 10 + digit;
        }

        const auto stored_fraction_size = std::min<std::size_t>(fraction_size, precision);
        std::uint64_t fraction = 0;
        for (std::size_t i = 0; i < fraction_size; ++i) {
            const auto ch = static_cast<unsigned char>(text[fraction_begin + i]);
            if (ch < '0' || ch > '9') return std::nullopt;
            if (i < stored_fraction_size) {
                fraction = fraction * 10 + static_cast<std::uint64_t>(ch - '0');
            } else if (ch != '0') {
                return std::nullopt;
            }
        }
        for (std::size_t i = stored_fraction_size; i < precision; ++i) {
            fraction *= 10;
        }

        const auto factor = detail::power_of_ten(precision);
        const auto max_positive = static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)());
        const auto max_magnitude = negative ? max_positive + 1 : max_positive;
        if (whole > (max_magnitude - fraction) / factor) return std::nullopt;
        const auto magnitude = whole * factor + fraction;

        MoneyAmount result;
        result.precision = precision;
        result.currency = currency;
        if (!negative) {
            result.units = static_cast<std::int64_t>(magnitude);
        } else if (magnitude == max_positive + 1) {
            result.units = (std::numeric_limits<std::int64_t>::min)();
        } else {
            result.units = -static_cast<std::int64_t>(magnitude);
        }
        return result;
    }

    /// \brief Formats the fixed-point value without scientific notation.
    std::string to_decimal() const {
        if (!valid_metadata()) return {};

        const bool negative = units < 0;
        const auto magnitude = negative
            ? static_cast<std::uint64_t>(-(units + 1)) + 1
            : static_cast<std::uint64_t>(units);
        const auto factor = detail::power_of_ten(precision);
        const auto whole = magnitude / factor;
        const auto fraction = magnitude % factor;
        std::string result = negative ? "-" : "";
        result += std::to_string(whole);
        if (precision == 0) return result;
        result += ".";
        auto fraction_text = std::to_string(fraction);
        result.append(precision - fraction_text.size(), '0');
        result += fraction_text;
        return result;
    }
};

struct SizingDecision {
    SizingDecisionKind kind = SizingDecisionKind::UNAVAILABLE;
    SizingReason reason = SizingReason::INVALID_CONFIGURATION;
    std::optional<MoneyAmount> amount;

    static SizingDecision sized(const MoneyAmount& amount) {
        return {SizingDecisionKind::SIZED, SizingReason::SIZED, amount};
    }

    static SizingDecision rejected(SizingReason reason) noexcept {
        return {SizingDecisionKind::REJECTED, reason, std::nullopt};
    }

    static SizingDecision unavailable(SizingReason reason) noexcept {
        return {SizingDecisionKind::UNAVAILABLE, reason, std::nullopt};
    }

    bool is_sized() const noexcept {
        return kind == SizingDecisionKind::SIZED && amount.has_value();
    }
};

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_POLICY_TYPES_HPP_INCLUDED
