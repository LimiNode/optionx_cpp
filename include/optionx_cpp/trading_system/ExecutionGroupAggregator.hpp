#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_EXECUTION_GROUP_AGGREGATOR_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_EXECUTION_GROUP_AGGREGATOR_HPP_INCLUDED

/// \file ExecutionGroupAggregator.hpp
/// \brief Aggregates a fixed roster of physical results with idempotent delivery.

#include "Settlement.hpp"

#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace optionx::trading_system {

/// \class ExecutionGroupAggregator
/// \brief Single-owner, in-memory aggregation; no submission or retry behavior.
class ExecutionGroupAggregator {
public:
    /// \brief Creates an invalid, empty aggregator for an idle series owner.
    ExecutionGroupAggregator() = default;

    /// \brief Fixes the roster before submission, including never-submitted slots.
    ExecutionGroupAggregator(
        IntentId intent_id,
        ExecutionGroupIdentity group,
        const std::vector<std::uint64_t>& trade_ids,
        CurrencyType currency,
        std::uint8_t precision = 2)
        : m_intent_id(intent_id), m_group(group), m_zero{0, precision, currency} {
        if (intent_id == 0 || !m_zero.valid_metadata() || trade_ids.empty() ||
            trade_ids.size() != group.expected_members ||
            (trade_ids.size() > 1 && group.group_id == 0)) return;
        for (auto trade_id : trade_ids) {
            if (trade_id == 0 || !m_members.emplace(trade_id, std::nullopt).second) return;
        }
        m_valid = true;
    }

    bool valid() const noexcept { return m_valid; }
    GroupOutcome outcome() const { return m_outcome; }

    /// \brief Applies one event atomically relative to this sequential owner.
    /// \details Exact replays are no-ops, including old UNKNOWN events after
    /// reconciliation. A different event cannot overwrite a terminal member.
    ProgressionResult apply(const TradeSettlement& event) {
        const auto receipt = m_receipts.find(event.settlement_id);
        if (receipt != m_receipts.end()) {
            return receipt->second == event ? ProgressionResult::DUPLICATE
                                            : ProgressionResult::SETTLEMENT_CONFLICT;
        }
        if (!m_valid) return ProgressionResult::INVALID_CONFIGURATION;
        if (!event.valid()) return ProgressionResult::INVALID_OUTCOME;
        if (event.intent_id != m_intent_id || event.execution_group_id != m_group.group_id) {
            return ProgressionResult::WRONG_OPERATION;
        }
        const auto member = m_members.find(event.trade_id);
        if (member == m_members.end()) return ProgressionResult::UNKNOWN_MEMBER;
        if (member->second && member->second->terminal()) {
            return ProgressionResult::MEMBER_ALREADY_TERMINAL;
        }
        if (event.net_profit) {
            if (event.net_profit->currency != m_zero.currency) {
                return ProgressionResult::CURRENCY_MISMATCH;
            }
            if (event.net_profit->precision != m_zero.precision) {
                return ProgressionResult::PRECISION_MISMATCH;
            }
        }

        auto candidate = *this;
        candidate.m_members.at(event.trade_id) = event;
        const auto result = candidate.aggregate();
        if (result != ProgressionResult::APPLIED) return result;
        candidate.m_receipts.emplace(event.settlement_id, event);
        swap(candidate);
        return ProgressionResult::APPLIED;
    }

    void swap(ExecutionGroupAggregator& other) noexcept {
        using std::swap;
        swap(m_intent_id, other.m_intent_id);
        swap(m_group, other.m_group);
        swap(m_zero, other.m_zero);
        swap(m_valid, other.m_valid);
        swap(m_outcome, other.m_outcome);
        m_members.swap(other.m_members);
        m_receipts.swap(other.m_receipts);
    }

private:
    ProgressionResult aggregate() noexcept {
        GroupOutcome result;
        auto sum = m_zero;
        bool has_execution = false;
        bool has_rejection = false;
        for (const auto& member : m_members) {
            if (!member.second) continue;
            const auto& event = *member.second;
            if (!event.terminal()) {
                result.unresolved = true;
                continue;
            }
            ++result.completed_members;
            has_rejection = has_rejection || event.kind == OutcomeKind::REJECTED;
            if (!event.net_profit) continue;
            has_execution = true;
        }

        // Cancel opposite signs first so a representable total cannot fail
        // merely because the roster orders two large wins before a loss.
        auto positive = m_members.cbegin();
        auto negative = m_members.cbegin();
        const auto take = [&](auto& iterator, bool want_positive) -> std::optional<std::int64_t> {
            while (iterator != m_members.cend()) {
                const auto& event = (iterator++)->second;
                if (!event || !event->net_profit) continue;
                const auto units = event->net_profit->units;
                if (want_positive ? units > 0 : units < 0) return units;
            }
            return std::nullopt;
        };
        auto plus = take(positive, true);
        auto minus = take(negative, false);
        while (plus || minus) {
            const bool use_minus = minus && (sum.units >= 0 || !plus);
            const auto units = use_minus ? *minus : *plus;
            if ((units > 0 && sum.units > (std::numeric_limits<std::int64_t>::max)() - units) ||
                (units < 0 && sum.units < (std::numeric_limits<std::int64_t>::min)() - units)) {
                return ProgressionResult::NUMERIC_OVERFLOW;
            }
            sum.units += units;
            if (use_minus) minus = take(negative, false);
            else plus = take(positive, true);
        }
        if (has_execution) result.net_profit = sum;
        result.final = result.completed_members == m_group.expected_members;
        if (result.final) {
            if (has_execution) {
                result.kind = sum.units > 0 ? OutcomeKind::WIN :
                              sum.units < 0 ? OutcomeKind::LOSS : OutcomeKind::DRAW;
            } else {
                result.kind = has_rejection ? OutcomeKind::REJECTED : OutcomeKind::CANCELLED;
            }
        }
        m_outcome = result;
        return ProgressionResult::APPLIED;
    }

    IntentId m_intent_id = 0;
    ExecutionGroupIdentity m_group;
    MoneyAmount m_zero;
    bool m_valid = false;
    GroupOutcome m_outcome;
    std::map<std::uint64_t, std::optional<TradeSettlement>> m_members;
    std::map<SettlementId, TradeSettlement> m_receipts;
};

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_EXECUTION_GROUP_AGGREGATOR_HPP_INCLUDED
