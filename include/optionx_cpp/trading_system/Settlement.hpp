#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_SETTLEMENT_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_SETTLEMENT_HPP_INCLUDED

/// \file Settlement.hpp
/// \brief Defines broker-independent outcome evidence and transition results.

#include "Identity.hpp"
#include "PolicyTypes.hpp"

#include <optional>

namespace optionx::trading_system {

enum class OutcomeKind { WIN, LOSS, DRAW, REJECTED, CANCELLED, UNKNOWN };
enum class ExecutionEvidence { NOT_SUBMITTED, MAY_HAVE_EXECUTED, EXECUTED };
enum class SeriesStatus { ACTIVE, WAITING_OUTCOME, UNRESOLVED, COMPLETED, BLOCKED_ERROR };
enum class AggregationStatus { PENDING, READY, NUMERIC_OVERFLOW };

enum class ProgressionResult {
    APPLIED,
    DUPLICATE,
    SETTLEMENT_CONFLICT,
    REVISION_CONFLICT,
    REVISION_OVERFLOW,
    INVALID_STATE,
    INVALID_IDENTITY,
    INVALID_CONFIGURATION,
    UNSUPPORTED_CARDINALITY,
    INTENT_ALREADY_USED,
    WRONG_OPERATION,
    UNKNOWN_MEMBER,
    MEMBER_ALREADY_TERMINAL,
    INVALID_OUTCOME,
    CURRENCY_MISMATCH,
    PRECISION_MISMATCH,
    NUMERIC_OVERFLOW,
    STEP_LIMIT_REACHED,
    IDEMPOTENCY_KEY_ALREADY_USED
};

/// \class TradeSettlement
/// \brief An immutable-by-convention event for one predeclared physical member.
/// \details UNKNOWN is non-terminal. REJECTED/CANCELLED assert proven absence
/// of submission. Reconciliation supplies a new ID, never rewrites an event.
struct TradeSettlement {
    SettlementId settlement_id = 0;
    IntentId intent_id = 0;
    ExecutionGroupId execution_group_id = 0;
    std::uint64_t trade_id = 0;
    OutcomeKind kind = OutcomeKind::UNKNOWN;
    ExecutionEvidence evidence = ExecutionEvidence::MAY_HAVE_EXECUTED;
    std::optional<MoneyAmount> net_profit;

    bool is_trading_result() const noexcept {
        return kind == OutcomeKind::WIN || kind == OutcomeKind::LOSS ||
               kind == OutcomeKind::DRAW;
    }

    bool terminal() const noexcept { return kind != OutcomeKind::UNKNOWN; }

    bool valid() const noexcept {
        if (settlement_id == 0 || intent_id == 0 || trade_id == 0) return false;
        if (is_trading_result()) {
            if (evidence != ExecutionEvidence::EXECUTED || !net_profit ||
                !net_profit->valid_metadata()) return false;
            return (kind == OutcomeKind::WIN && net_profit->units > 0) ||
                   (kind == OutcomeKind::LOSS && net_profit->units < 0) ||
                   (kind == OutcomeKind::DRAW && net_profit->units == 0);
        }
        if (net_profit) return false;
        if (kind == OutcomeKind::UNKNOWN) {
            return evidence == ExecutionEvidence::MAY_HAVE_EXECUTED;
        }
        return (kind == OutcomeKind::REJECTED || kind == OutcomeKind::CANCELLED) &&
               evidence == ExecutionEvidence::NOT_SUBMITTED;
    }
};

inline bool operator==(const TradeSettlement& lhs, const TradeSettlement& rhs) noexcept {
    if (lhs.settlement_id != rhs.settlement_id || lhs.intent_id != rhs.intent_id ||
        lhs.execution_group_id != rhs.execution_group_id || lhs.trade_id != rhs.trade_id ||
        lhs.kind != rhs.kind || lhs.evidence != rhs.evidence ||
        lhs.net_profit.has_value() != rhs.net_profit.has_value()) return false;
    return !lhs.net_profit ||
           (lhs.net_profit->units == rhs.net_profit->units &&
            lhs.net_profit->precision == rhs.net_profit->precision &&
            lhs.net_profit->currency == rhs.net_profit->currency);
}

/// \brief Compares terminal evidence while ignoring its event identity.
///
/// Different broker observations may carry different settlement IDs while
/// proving the same terminal physical result. Such observations are
/// idempotent evidence, not a second terminal transition.
inline bool same_terminal_evidence(
    const TradeSettlement& lhs, const TradeSettlement& rhs) noexcept {
    if (!lhs.terminal() || !rhs.terminal() ||
        lhs.intent_id != rhs.intent_id ||
        lhs.execution_group_id != rhs.execution_group_id ||
        lhs.trade_id != rhs.trade_id || lhs.kind != rhs.kind ||
        lhs.evidence != rhs.evidence ||
        lhs.net_profit.has_value() != rhs.net_profit.has_value()) {
        return false;
    }
    return !lhs.net_profit ||
           (lhs.net_profit->units == rhs.net_profit->units &&
            lhs.net_profit->precision == rhs.net_profit->precision &&
            lhs.net_profit->currency == rhs.net_profit->currency);
}

/// \brief Orders settlement observations for deterministic replay.
///
/// Non-terminal reconciliation evidence is replayed before terminal evidence;
/// SettlementId is only a deterministic tie-breaker within one phase.
inline bool canonical_settlement_before(
    const TradeSettlement& lhs, const TradeSettlement& rhs) noexcept {
    const auto lhs_phase = lhs.terminal() ? 1 : 0;
    const auto rhs_phase = rhs.terminal() ? 1 : 0;
    if (lhs_phase != rhs_phase) return lhs_phase < rhs_phase;
    return lhs.settlement_id < rhs.settlement_id;
}

/// \class GroupOutcome
/// \brief A value snapshot; partial money never constitutes a final outcome.
struct GroupOutcome {
    bool final = false;
    bool unresolved = false;
    bool all_members_terminal = false;
    std::uint32_t completed_members = 0;
    AggregationStatus aggregation_status = AggregationStatus::PENDING;
    std::optional<OutcomeKind> kind;
    std::optional<MoneyAmount> net_profit;
};

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_SETTLEMENT_HPP_INCLUDED
