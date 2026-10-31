#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_SIZING_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_SIZING_HPP_INCLUDED

/// \file Sizing.hpp
/// \brief Defines pure fixed-stake and percentage sizing policies.

#include "DecisionContext.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace optionx::trading_system {

enum class MoneyRounding {
    DOWN,
    HALF_UP,
    UP
};

namespace detail {

inline std::optional<std::int64_t> percentage_units(
    std::int64_t balance_units,
    std::uint8_t balance_precision,
    std::uint8_t output_precision,
    std::int64_t basis_points,
    MoneyRounding rounding) noexcept {
    if (balance_units < 0 || balance_precision > max_money_precision ||
        output_precision < balance_precision ||
        output_precision > max_money_precision ||
        basis_points <= 0 || basis_points > 10000) {
        return std::nullopt;
    }

    const auto precision_delta = static_cast<std::uint8_t>(
        output_precision - balance_precision);
    const auto max_value = static_cast<std::uint64_t>(
        (std::numeric_limits<std::int64_t>::max)());

    // Four decimal percentage places are represented by basis points. When
    // the output has at least four extra decimal places, the denominator is
    // cancelled before multiplication and the result is exact.
    if (precision_delta >= 4) {
        const auto factor = power_of_ten(
            static_cast<std::uint8_t>(precision_delta - 4));
        const auto multiplier = static_cast<std::uint64_t>(basis_points) * factor;
        if (static_cast<std::uint64_t>(balance_units) > max_value / multiplier) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(
            static_cast<std::uint64_t>(balance_units) * multiplier);
    }

    const auto denominator = power_of_ten(
        static_cast<std::uint8_t>(4 - precision_delta));
    const auto value = static_cast<std::uint64_t>(balance_units);
    const auto whole = value / denominator;
    const auto remainder = value % denominator;
    if (whole > max_value / static_cast<std::uint64_t>(basis_points)) {
        return std::nullopt;
    }

    auto result = whole * static_cast<std::uint64_t>(basis_points);
    const auto remainder_product = remainder * static_cast<std::uint64_t>(basis_points);
    const auto integral_remainder = remainder_product / denominator;
    if (result > max_value - integral_remainder) return std::nullopt;
    result += integral_remainder;
    const auto fractional = remainder_product % denominator;
    if (rounding == MoneyRounding::UP && fractional != 0) {
        if (result == max_value) return std::nullopt;
        ++result;
    } else if (rounding == MoneyRounding::HALF_UP &&
               fractional >= denominator / 2) {
        if (result == max_value) return std::nullopt;
        ++result;
    }
    return static_cast<std::int64_t>(result);
}

} // namespace detail

/// \brief Returns a configured monetary amount without reading account state.
class FixedStake {
public:
    explicit FixedStake(MoneyAmount amount) : m_amount(std::move(amount)) {}

    SizingDecision evaluate(const TradingDecisionContext&) const {
        if (!m_amount.valid_metadata() || m_amount.units <= 0) {
            return SizingDecision::rejected(SizingReason::INVALID_AMOUNT);
        }
        return SizingDecision::sized(m_amount);
    }

private:
    MoneyAmount m_amount;
};

/// \brief Calculates a fixed percentage of a supported balance snapshot.
class PercentOfBalance {
public:
    explicit PercentOfBalance(
        double percent,
        std::uint8_t output_precision = 2,
        MoneyRounding rounding = MoneyRounding::DOWN)
        : m_output_precision(output_precision), m_rounding(rounding) {
        if (!std::isfinite(percent) || percent <= 0.0 || percent > 100.0) return;
        const auto basis_points = std::round(percent * 100.0);
        if (!std::isfinite(basis_points) || basis_points < 1.0 || basis_points > 10000.0) {
            return;
        }
        m_basis_points = static_cast<std::int64_t>(basis_points);
        m_valid = true;
    }

    static PercentOfBalance from_basis_points(
        std::int64_t basis_points,
        std::uint8_t output_precision = 2,
        MoneyRounding rounding = MoneyRounding::DOWN) noexcept {
        PercentOfBalance result;
        result.m_basis_points = basis_points;
        result.m_output_precision = output_precision;
        result.m_rounding = rounding;
        result.m_valid = basis_points > 0 && basis_points <= 10000;
        return result;
    }

    bool valid() const noexcept {
        return m_valid && m_output_precision <= detail::max_money_precision;
    }

    SizingDecision evaluate(const TradingDecisionContext& context) const {
        if (!valid()) {
            return SizingDecision::rejected(SizingReason::INVALID_PERCENT);
        }
        switch (context.account.availability) {
            case DataAvailability::SUPPORTED: break;
            case DataAvailability::UNAVAILABLE:
                return SizingDecision::unavailable(SizingReason::BALANCE_UNAVAILABLE);
            case DataAvailability::STALE:
                return SizingDecision::unavailable(SizingReason::BALANCE_STALE);
            case DataAvailability::UNKNOWN:
                return SizingDecision::unavailable(SizingReason::BALANCE_UNKNOWN);
        }
        if (!context.account.balance.has_value()) {
            return SizingDecision::unavailable(SizingReason::MISSING_BALANCE);
        }

        const auto& balance = *context.account.balance;
        if (balance.precision > detail::max_money_precision) {
            return SizingDecision::rejected(SizingReason::INVALID_AMOUNT);
        }
        if (balance.currency == CurrencyType::UNKNOWN) {
            return SizingDecision::rejected(SizingReason::UNKNOWN_CURRENCY);
        }
        if (!balance.valid_metadata()) {
            return SizingDecision::rejected(SizingReason::INVALID_AMOUNT);
        }
        if (context.account.currency == CurrencyType::UNKNOWN) {
            return SizingDecision::unavailable(SizingReason::UNKNOWN_CURRENCY);
        }
        if (context.account.currency != balance.currency) {
            return SizingDecision::rejected(SizingReason::CURRENCY_MISMATCH);
        }
        if (balance.units < 0) {
            return SizingDecision::rejected(SizingReason::NEGATIVE_BALANCE);
        }
        if (balance.units == 0) {
            return SizingDecision::rejected(SizingReason::ZERO_BALANCE);
        }

        if (m_output_precision < balance.precision) {
            return SizingDecision::rejected(SizingReason::PRECISION_MISMATCH);
        }
        const auto calculated = detail::percentage_units(
            balance.units,
            balance.precision,
            m_output_precision,
            m_basis_points,
            m_rounding);
        if (!calculated.has_value()) {
            return SizingDecision::rejected(SizingReason::NUMERIC_OVERFLOW);
        }
        if (*calculated <= 0) {
            return SizingDecision::rejected(SizingReason::ZERO_RESULT);
        }
        return SizingDecision::sized(
            MoneyAmount{*calculated, m_output_precision, balance.currency});
    }

private:
    PercentOfBalance() = default;

    std::int64_t m_basis_points = 0;
    std::uint8_t m_output_precision = 2;
    MoneyRounding m_rounding = MoneyRounding::DOWN;
    bool m_valid = false;
};

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_SIZING_HPP_INCLUDED
