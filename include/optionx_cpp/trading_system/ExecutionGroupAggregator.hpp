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
        candidate.aggregate();
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
    // At most UINT32_MAX members contribute signed 64-bit values, so each
    // sign's magnitude needs at most 95 bits. Two portable limbs suffice.
    struct Magnitude {
        std::uint64_t low = 0;
        std::uint64_t high = 0;

        void add(std::uint64_t units) noexcept {
            const auto previous = low;
            low += units;
            if (low < previous) ++high;
        }

        bool less_than(const Magnitude& other) const noexcept {
            return high < other.high || (high == other.high && low < other.low);
        }

        Magnitude subtract(const Magnitude& other) const noexcept {
            return {low - other.low, high - other.high - (low < other.low ? 1U : 0U)};
        }
    };

    class NetAccumulator {
    public:
        void add(std::int64_t units) noexcept {
            if (units < 0) {
                m_negative.add(static_cast<std::uint64_t>(-(units + 1)) + 1);
            } else {
                m_positive.add(static_cast<std::uint64_t>(units));
            }
        }

        bool to_units(std::int64_t& units) const noexcept {
            const bool negative = m_positive.less_than(m_negative);
            const auto magnitude = negative ? m_negative.subtract(m_positive)
                                            : m_positive.subtract(m_negative);
            const auto maximum = static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)());
            if (magnitude.high != 0 || magnitude.low > maximum + (negative ? 1U : 0U)) {
                return false;
            }
            if (negative && magnitude.low == maximum + 1) {
                units = (std::numeric_limits<std::int64_t>::min)();
            } else {
                const auto value = static_cast<std::int64_t>(magnitude.low);
                units = negative ? -value : value;
            }
            return true;
        }

    private:
        Magnitude m_positive;
        Magnitude m_negative;
    };

    void aggregate() noexcept {
        GroupOutcome result;
        auto sum = m_zero;
        NetAccumulator accumulator;
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
            accumulator.add(event.net_profit->units);
        }
        result.all_members_terminal = result.completed_members == m_group.expected_members;
        if (has_execution && !accumulator.to_units(sum.units)) {
            result.aggregation_status = AggregationStatus::NUMERIC_OVERFLOW;
            m_outcome = result;
            return;
        }
        if (has_execution) result.net_profit = sum;
        result.final = result.all_members_terminal;
        if (result.final) {
            result.aggregation_status = AggregationStatus::READY;
            if (has_execution) {
                result.kind = sum.units > 0 ? OutcomeKind::WIN :
                              sum.units < 0 ? OutcomeKind::LOSS : OutcomeKind::DRAW;
            } else {
                result.kind = has_rejection ? OutcomeKind::REJECTED : OutcomeKind::CANCELLED;
            }
        }
        m_outcome = result;
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
