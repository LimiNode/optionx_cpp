#pragma once
#ifndef OPTIONX_TEST_SUPPORT_TRADING_SYSTEM_LEGACY_SETTLEMENT_V1_HPP_INCLUDED
#define OPTIONX_TEST_SUPPORT_TRADING_SYSTEM_LEGACY_SETTLEMENT_V1_HPP_INCLUDED

/// \file LegacySettlementV1.hpp
/// \brief Frozen v1 settlement codec/validator from reviewed dad7bcf8, not a storage engine.

namespace optionx::test_support::legacy_settlement_v1 {
namespace ts = optionx::trading_system;

enum class ExecutionEvidence { NOT_SUBMITTED, MAY_HAVE_EXECUTED, EXECUTED };

// Keep this validator independent of the current TradeSettlement::valid().
struct TradeSettlement {
    ts::SettlementId settlement_id = 0;
    ts::IntentId intent_id = 0;
    ts::ExecutionGroupId execution_group_id = 0;
    std::uint64_t trade_id = 0;
    ts::OutcomeKind kind = ts::OutcomeKind::UNKNOWN;
    ExecutionEvidence evidence = ExecutionEvidence::MAY_HAVE_EXECUTED;
    std::optional<ts::MoneyAmount> net_profit;

    bool valid() const noexcept {
        if (settlement_id == 0 || intent_id == 0 || trade_id == 0) return false;
        if (kind == ts::OutcomeKind::WIN || kind == ts::OutcomeKind::LOSS ||
            kind == ts::OutcomeKind::DRAW) {
            if (evidence != ExecutionEvidence::EXECUTED || !net_profit ||
                !net_profit->valid_metadata()) return false;
            return (kind == ts::OutcomeKind::WIN && net_profit->units > 0) ||
                   (kind == ts::OutcomeKind::LOSS && net_profit->units < 0) ||
                   (kind == ts::OutcomeKind::DRAW && net_profit->units == 0);
        }
        if (net_profit) return false;
        if (kind == ts::OutcomeKind::UNKNOWN) {
            return evidence == ExecutionEvidence::MAY_HAVE_EXECUTED;
        }
        return (kind == ts::OutcomeKind::REJECTED || kind == ts::OutcomeKind::CANCELLED) &&
               evidence == ExecutionEvidence::NOT_SUBMITTED;
    }
};

inline TradeSettlement decode(const nlohmann::json& value) {
    TradeSettlement result;
    result.settlement_id = value.value("settlement_id", ts::SettlementId{0});
    result.intent_id = value.value("intent_id", ts::IntentId{0});
    result.execution_group_id = value.value("execution_group_id", ts::ExecutionGroupId{0});
    result.trade_id = value.value("trade_id", std::uint64_t{0});
    result.kind = static_cast<ts::OutcomeKind>(value.value("kind", 5));
    result.evidence = static_cast<ExecutionEvidence>(value.value("evidence", 1));
    if (value.contains("net_profit")) result.net_profit = ts::detail::money_from_json(value.at("net_profit"));
    return result;
}

} // namespace optionx::test_support::legacy_settlement_v1

#endif // OPTIONX_TEST_SUPPORT_TRADING_SYSTEM_LEGACY_SETTLEMENT_V1_HPP_INCLUDED
