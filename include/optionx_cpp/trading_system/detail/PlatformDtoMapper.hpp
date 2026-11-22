#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_PLATFORM_DTO_MAPPER_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_PLATFORM_DTO_MAPPER_HPP_INCLUDED

/// \file PlatformDtoMapper.hpp
/// \brief Internal pure IntradeBar DTO translation; no execution or durable receipts.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace optionx::trading_system::detail::platform_mapping {

static_assert(std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::radix == 2 &&
              std::numeric_limits<double>::digits == 53,
              "The DTO money profile requires IEEE 754 binary64");

enum class MappingError {
    NONE,
    UNSUPPORTED_CURRENCY,
    INVALID_PRECISION,
    CURRENCY_MISMATCH,
    NON_FINITE_MONEY,
    NUMERIC_OVERFLOW,
    LOSSY_STAKE,
    INVALID_BINDING,
    ID_OUT_OF_RANGE,
    ACCOUNT_MISMATCH,
    ACCOUNT_INACTIVE,
    TRADE_MISMATCH,
    UNSUPPORTED_PLATFORM,
    INVALID_REQUEST,
    INVALID_BROKER_ID,
    BROKER_ID_MISMATCH,
    MISSING_BROKER_ID,
    STAKE_MISMATCH,
    INVALID_OUTCOME,
    UNSUPPORTED_REFUND,
    UNSUPPORTED_STATE
};

/// \brief Explicit native quantization profile, not a public money-policy API.
struct MoneyProfile {
    CurrencyType currency = CurrencyType::UNKNOWN;
    int precision = 0;
};

inline constexpr const char* money_normalization_rule =
    "BINARY64_FIXED_NEAREST_EVEN_V1";

template <class T>
struct MappingResult {
    MappingError error = MappingError::NONE;
    std::optional<T> value;

    bool succeeded() const noexcept {
        return error == MappingError::NONE && value.has_value();
    }
};

inline bool supported_currency(CurrencyType value) noexcept {
    switch (value) {
        case CurrencyType::USD: case CurrencyType::EUR: case CurrencyType::GBP:
        case CurrencyType::BTC: case CurrencyType::ETH: case CurrencyType::USDT:
        case CurrencyType::USDC: case CurrencyType::RUB: case CurrencyType::UAH:
        case CurrencyType::KZT: return true;
        default: return false;
    }
}

inline MappingError profile_error(const MoneyProfile& profile) noexcept {
    if (!supported_currency(profile.currency)) return MappingError::UNSUPPORTED_CURRENCY;
    if (profile.precision < 0 || profile.precision > max_money_precision) {
        return MappingError::INVALID_PRECISION;
    }
    return MappingError::NONE;
}

/// \brief Quantizes the exact input binary64 value, nearest with ties to even.
inline MappingResult<MoneyAmount> normalize_money(
    double value, CurrencyType currency, const MoneyProfile& profile) {
    const auto error = profile_error(profile);
    if (error != MappingError::NONE) return {error, std::nullopt};
    if (!supported_currency(currency)) {
        return {MappingError::UNSUPPORTED_CURRENCY, std::nullopt};
    }
    if (currency != profile.currency) return {MappingError::CURRENCY_MISMATCH, std::nullopt};
    if (!std::isfinite(value)) return {MappingError::NON_FINITE_MONEY, std::nullopt};

    // charconv avoids scaled floating-point arithmetic and locale/fenv rounding.
    // 400 covers the largest binary64 fixed representation plus 18 decimals.
    char buffer[400];
    const auto formatted = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::fixed,
        profile.precision);
    if (formatted.ec != std::errc{}) return {MappingError::NUMERIC_OVERFLOW, std::nullopt};
    const auto amount = MoneyAmount::from_decimal(
        std::string(buffer, formatted.ptr), currency,
        static_cast<std::uint8_t>(profile.precision));
    if (!amount) return {MappingError::NUMERIC_OVERFLOW, std::nullopt};
    return {MappingError::NONE, amount};
}

inline MappingResult<double> stake_to_double(
    const MoneyAmount& stake, const MoneyProfile& profile) {
    const auto error = profile_error(profile);
    if (error != MappingError::NONE) return {error, std::nullopt};
    if (!stake.valid_metadata() || stake.units <= 0 ||
        stake.precision != profile.precision) {
        return {MappingError::INVALID_REQUEST, std::nullopt};
    }
    if (stake.currency != profile.currency) return {MappingError::CURRENCY_MISMATCH, std::nullopt};
    const auto text = stake.to_decimal();
    double value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value, std::chars_format::fixed);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return {MappingError::NUMERIC_OVERFLOW, std::nullopt};
    }
    const auto round_trip = normalize_money(value, stake.currency, profile);
    if (!round_trip.succeeded() || round_trip.value->units != stake.units) {
        return {MappingError::LOSSY_STAKE, std::nullopt};
    }
    return {MappingError::NONE, value};
}

inline AccountType account_realm(const AccountIdentityRecord& account) noexcept {
    if (account.realm == "DEMO") return AccountType::DEMO;
    if (account.realm == "REAL") return AccountType::REAL;
    return AccountType::UNKNOWN;
}

inline MappingError binding_error(
    const ExecutionOperationRecord& operation, const AccountIdentityRecord& account) {
    if (!operation.valid() || !account.valid()) return MappingError::INVALID_BINDING;
    if (operation.trade_id > (std::numeric_limits<std::uint32_t>::max)() ||
        operation.account_id > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())) return MappingError::ID_OUT_OF_RANGE;
    if (operation.account_id != account.account_id ||
        operation.risk_scope_id != account.risk_scope_id ||
        account_realm(account) == AccountType::UNKNOWN) return MappingError::ACCOUNT_MISMATCH;
    // Both spellings occur in existing persistent/configured account metadata.
    if (account.platform_type != "IntradeBar" && account.platform_type != "INTRADE_BAR") {
        return MappingError::UNSUPPORTED_PLATFORM;
    }
    return MappingError::NONE;
}

/// \brief Copies a fixed roster member into a callback-free legacy request.
/// \details This validates translation, not risk admission or permission to submit.
inline MappingResult<TradeRequest> map_request(
    const TradeIntent& intent, const ExecutionRosterRecord& roster,
    const ExecutionOperationRecord& operation, const AccountIdentityRecord& account,
    const MoneyProfile& profile) {
    auto error = binding_error(operation, account);
    if (error != MappingError::NONE) return {error, std::nullopt};
    if (!account.active) return {MappingError::ACCOUNT_INACTIVE, std::nullopt};
    if (!intent.valid() || !roster.valid() ||
        roster.schema_version != trading_storage_schema_version ||
        roster.intent_id != intent.intent_id || operation.intent_id != intent.intent_id ||
        roster.group_id != intent.execution_group.group_id ||
        operation.group_id != roster.group_id ||
        roster.expected_members != intent.execution_group.expected_members) {
        return {MappingError::INVALID_BINDING, std::nullopt};
    }
    const auto member = std::find_if(roster.members.begin(), roster.members.end(),
        [&operation](const RosterMemberRecord& value) { return value.trade_id == operation.trade_id; });
    if (member == roster.members.end()) return {MappingError::TRADE_MISMATCH, std::nullopt};
    if (member->account_id != operation.account_id ||
        member->risk_scope_id != operation.risk_scope_id) {
        return {MappingError::ACCOUNT_MISMATCH, std::nullopt};
    }
    const auto account_id = static_cast<std::int64_t>(operation.account_id);
    const auto trade_id = static_cast<std::uint32_t>(operation.trade_id);
    const auto& source = intent.request;
    if ((source.trade_id != 0 && source.trade_id != trade_id) ||
        (source.signal_id != 0 && source.signal_id != intent.origin_signal_id)) {
        return {MappingError::TRADE_MISMATCH, std::nullopt};
    }
    if ((source.account_id != 0 && source.account_id != account_id) ||
        (source.account_type != AccountType::UNKNOWN &&
         source.account_type != account_realm(account))) return {MappingError::ACCOUNT_MISMATCH, std::nullopt};
    if (source.currency != CurrencyType::UNKNOWN && source.currency != member->stake.currency) {
        return {MappingError::CURRENCY_MISMATCH, std::nullopt};
    }
    if (source.symbol.empty() ||
        (source.order_type != OrderType::BUY && source.order_type != OrderType::SELL) ||
        (source.option_type != OptionType::SPRINT && source.option_type != OptionType::CLASSIC) ||
        source.expiry_time < 0 || (source.duration == 0 && source.expiry_time == 0) ||
        !std::isfinite(source.refund) || source.refund < 0 || source.refund > 1 ||
        !std::isfinite(source.min_payout) || source.min_payout < 0) {
        return {MappingError::INVALID_REQUEST, std::nullopt};
    }
    const auto amount = stake_to_double(member->stake, profile);
    if (!amount.succeeded()) return {amount.error, std::nullopt};
    TradeRequest request(source);
    request.trade_id = trade_id;
    request.account_id = account_id;
    request.account_type = account_realm(account);
    request.signal_id = intent.origin_signal_id;
    request.currency = member->stake.currency;
    request.amount = *amount.value;
    return {MappingError::NONE, std::move(request)};
}

enum class ObservationKind {
    PROGRESS,
    FAILED_OPEN,
    OPEN_CONFIRMED,
    RESULT_CHECK_FAILED,
    FINANCIAL_RESULT,
    CANCELLATION_REPORTED,
    UNSUPPORTED
};

/// \brief Owned observation data, deliberately not a TradeSettlement or receipt.
struct PlatformObservation {
    TradeResult raw;
    ExecutionOperationRecord operation;
    AccountIdentityRecord account;
    MoneyProfile profile;
    MappingError error = MappingError::NONE;
    ObservationKind kind = ObservationKind::PROGRESS;
    bool proves_opening = false;
    std::string broker_trade_id;
    std::optional<OutcomeKind> outcome;
    std::optional<MoneyAmount> net_profit;
};

/// \brief Maps one DTO with its previously captured request/operation/account binding.
/// \details The caller owns binding provenance; TradeResult has no account_id.
inline PlatformObservation map_observation(
    const TradeResult& result, const TradeRequest& request,
    const ExecutionOperationRecord& operation, const AccountIdentityRecord& account,
    const MoneyProfile& profile) {
    PlatformObservation observation;
    observation.raw = result;
    observation.operation = operation;
    observation.account = account;
    observation.profile = profile;
    auto& error = observation.error;
    error = binding_error(operation, account);
    if (error != MappingError::NONE) return observation;
    if (request.trade_id != operation.trade_id || result.trade_id != operation.trade_id) {
        error = MappingError::TRADE_MISMATCH;
        return observation;
    }
    if (request.account_id != static_cast<std::int64_t>(operation.account_id) ||
        request.account_type != account_realm(account) ||
        result.account_type != account_realm(account)) {
        error = MappingError::ACCOUNT_MISMATCH;
        return observation;
    }
    if (result.platform_type != PlatformType::INTRADE_BAR) {
        error = MappingError::UNSUPPORTED_PLATFORM;
        return observation;
    }
    error = profile_error(profile);
    if (error != MappingError::NONE) return observation;
    if (request.currency != profile.currency || result.currency != profile.currency) {
        error = MappingError::CURRENCY_MISMATCH;
        return observation;
    }
    std::int64_t saved_broker_id = 0;
    if (!operation.broker_trade_id.empty()) {
        const auto& text = operation.broker_trade_id;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), saved_broker_id);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            saved_broker_id <= 0 || std::to_string(saved_broker_id) != text) {
            error = MappingError::INVALID_BROKER_ID;
            return observation;
        }
    }
    if (result.option_id < 0) { error = MappingError::INVALID_BROKER_ID; return observation; }
    if (saved_broker_id != 0 && result.option_id != 0 && saved_broker_id != result.option_id) {
        error = MappingError::BROKER_ID_MISMATCH;
        return observation;
    }
    observation.broker_trade_id = result.option_id > 0
        ? std::to_string(result.option_id) : operation.broker_trade_id;

    switch (result.trade_state) {
        case TradeState::UNKNOWN:
        case TradeState::WAITING_OPEN: return observation;
        case TradeState::OPEN_ERROR:
            observation.kind = ObservationKind::FAILED_OPEN;
            return observation;
        case TradeState::OPEN_SUCCESS:
        case TradeState::IN_PROGRESS:
        case TradeState::WAITING_CLOSE:
            observation.kind = ObservationKind::OPEN_CONFIRMED;
            observation.proves_opening = true;
            return observation;
        case TradeState::CHECK_ERROR:
            observation.kind = ObservationKind::RESULT_CHECK_FAILED;
            observation.proves_opening = true;
            return observation;
        case TradeState::CANCELED_TRADE:
            observation.kind = ObservationKind::CANCELLATION_REPORTED;
            return observation;
        case TradeState::WIN: case TradeState::LOSS:
        case TradeState::STANDOFF: case TradeState::REFUND: break;
        default:
            observation.kind = ObservationKind::UNSUPPORTED;
            error = MappingError::UNSUPPORTED_STATE;
            return observation;
    }

    observation.kind = ObservationKind::FINANCIAL_RESULT;
    observation.proves_opening = true;
    if (observation.broker_trade_id.empty()) {
        error = MappingError::MISSING_BROKER_ID;
        return observation;
    }
    const auto requested = normalize_money(request.amount, request.currency, profile);
    const auto actual = normalize_money(result.amount, result.currency, profile);
    if (!requested.succeeded() || !actual.succeeded()) {
        error = !requested.succeeded() ? requested.error : actual.error;
        return observation;
    }
    if (requested.value->units <= 0 || requested.value->units != actual.value->units) {
        error = MappingError::STAKE_MISMATCH;
        return observation;
    }
    const auto net = normalize_money(result.profit, result.currency, profile);
    if (!net.succeeded()) { error = net.error; return observation; }
    if (result.trade_state == TradeState::REFUND && result.profit != 0) {
        error = MappingError::UNSUPPORTED_REFUND;
        return observation;
    }
    const bool win = result.trade_state == TradeState::WIN;
    const bool loss = result.trade_state == TradeState::LOSS;
    if ((win && !(result.profit > 0 && net.value->units > 0)) ||
        (loss && !(result.profit < 0 && net.value->units < 0)) ||
        (!win && !loss && (result.profit != 0 || net.value->units != 0))) {
        error = MappingError::INVALID_OUTCOME;
        return observation;
    }
    observation.outcome = win ? OutcomeKind::WIN : loss ? OutcomeKind::LOSS : OutcomeKind::DRAW;
    observation.net_profit = net.value;
    return observation;
}

} // namespace optionx::trading_system::detail::platform_mapping

#endif // OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_PLATFORM_DTO_MAPPER_HPP_INCLUDED
