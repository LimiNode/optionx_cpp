#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_DECISION_CONTEXT_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_DECISION_CONTEXT_HPP_INCLUDED

/// \file DecisionContext.hpp
/// \brief Defines the small value snapshot consumed by Batch 2 policies.

#include "Identity.hpp"
#include "PolicyTypes.hpp"
#include "TradeIntent.hpp"

#include <optional>
#include <string>
#include <utility>

namespace optionx::trading_system {

/// \brief Account balance snapshot supplied to pure sizing policies.
struct BalanceSnapshot {
    DataAvailability availability = DataAvailability::UNKNOWN;
    CurrencyType currency = CurrencyType::UNKNOWN;
    std::optional<MoneyAmount> balance;
    std::int64_t captured_at_ms = 0;
    std::uint64_t revision = 0;
};

/// \brief Immutable-by-convention input snapshot for stateless policies.
///
/// The snapshot owns all strings and scalar values needed by Batch 2. It does
/// not retain broker/platform pointers, and policy evaluation never reads the
/// system clock or calls a live provider. Construct it once and pass it as a
/// const value to policy objects.
struct TradingDecisionContext {
    IntentId intent_id = 0;
    SignalId signal_id = 0;
    std::string symbol;
    BridgeId bridge_id = 0;
    std::string signal_name;
    std::optional<std::int64_t> evaluation_time_ms;
    TradeTimeZone time_zone = TradeTimeZone::utc();
    BalanceSnapshot account;

    static TradingDecisionContext from_signal(
        const TradeSignal& signal,
        std::int64_t evaluation_time_ms,
        TradeTimeZone time_zone,
        BalanceSnapshot account = {}) {
        TradingDecisionContext result;
        result.signal_id = signal.signal_id;
        result.symbol = signal.symbol;
        result.bridge_id = signal.bridge_id;
        result.signal_name = signal.signal_name;
        result.evaluation_time_ms = evaluation_time_ms;
        result.time_zone = time_zone;
        result.account = std::move(account);
        return result;
    }

    static TradingDecisionContext from_intent(
        const TradeIntent& intent,
        std::int64_t evaluation_time_ms,
        TradeTimeZone time_zone,
        BalanceSnapshot account = {}) {
        TradingDecisionContext result;
        result.intent_id = intent.intent_id;
        result.signal_id = intent.origin_signal_id;
        result.symbol = intent.request.symbol;
        result.bridge_id = intent.request.bridge_id;
        result.signal_name = intent.request.signal_name;
        result.evaluation_time_ms = evaluation_time_ms;
        result.time_zone = time_zone;
        result.account = std::move(account);
        return result;
    }
};

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_DECISION_CONTEXT_HPP_INCLUDED
