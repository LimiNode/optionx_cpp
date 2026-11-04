#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_SERIES_PROGRESSION_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_SERIES_PROGRESSION_HPP_INCLUDED

/// \file SeriesProgression.hpp
/// \brief Defines single-owner, revision-checked series transitions.

#include "TradeSeries.hpp"
#include "TradeIntent.hpp"
#include "ExecutionGroupAggregator.hpp"

#include <set>
#include <string>

namespace optionx::trading_system {

/// \brief Shared counter ceiling; begin reserves room for one possible advance.
inline constexpr std::uint32_t max_series_step =
    static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());

/// \class SeriesSnapshot
/// \brief An owned read snapshot, not a persistence or runtime checkpoint.
struct SeriesSnapshot {
    TradeSeries series;
    SeriesStatus status = SeriesStatus::ACTIVE;
    IntentId current_intent_id = 0;
    ExecutionGroupId current_group_id = 0;
    std::uint32_t current_step = 0;
    GroupOutcome outcome;
};

/// \class SeriesProgression
/// \brief Owns one series and at most one unresolved logical operation.
/// \details Callers serialize every call. This is neither thread-safe CAS nor
/// persistent exactly-once processing. Copies are independent simulations.
class SeriesProgression {
public:
    explicit SeriesProgression(TradeSeries series) : m_state{std::move(series)} {}

    SeriesSnapshot snapshot() const { return m_state; }

    /// \brief Starts one operation with an immutable physical-member roster.
    ProgressionResult begin(
        const TradeIntent& intent,
        const std::vector<std::uint64_t>& trade_ids,
        CurrencyType currency,
        std::uint8_t precision,
        std::uint64_t expected_revision) {
        const auto check = check_revision(expected_revision);
        if (check != ProgressionResult::APPLIED) return check;
        if (m_state.status != SeriesStatus::ACTIVE) return ProgressionResult::INVALID_STATE;
        if (m_state.series.next_step == max_series_step) return ProgressionResult::STEP_LIMIT_REACHED;
        if (!intent.valid() || intent.series_id != m_state.series.series_id ||
            intent.origin_signal_id != m_state.series.origin_signal_id ||
            intent.series_step < 0 ||
            static_cast<std::uint32_t>(intent.series_step) != m_state.series.next_step) {
            return ProgressionResult::INVALID_IDENTITY;
        }
        if (intent.status != IntentStatus::PROPOSED && intent.status != IntentStatus::ADMITTED) {
            return ProgressionResult::INVALID_STATE;
        }
        if (intent.cardinality == ExecutionCardinality::MULTIPLE) {
            return ProgressionResult::UNSUPPORTED_CARDINALITY;
        }
        if (m_used_intents.count(intent.intent_id)) return ProgressionResult::INTENT_ALREADY_USED;
        if (m_used_intent_keys.count(intent.idempotency_key)) {
            return ProgressionResult::IDEMPOTENCY_KEY_ALREADY_USED;
        }
        auto group = intent.execution_group;
        // ZERO_OR_ONE may omit a physical count in Batch 1. A reserved slot
        // still needs a terminal no-execution event when it is never submitted.
        if (intent.cardinality == ExecutionCardinality::ZERO_OR_ONE) group.expected_members = 1;
        ExecutionGroupAggregator aggregator(intent.intent_id, group, trade_ids, currency, precision);
        if (!aggregator.valid()) return ProgressionResult::INVALID_CONFIGURATION;

        auto candidate = *this;
        candidate.m_group = std::move(aggregator);
        candidate.m_has_group = true;
        candidate.m_used_intents.insert(intent.intent_id);
        candidate.m_used_intent_keys.insert(intent.idempotency_key);
        candidate.m_state.status = SeriesStatus::WAITING_OUTCOME;
        candidate.m_state.current_intent_id = intent.intent_id;
        candidate.m_state.current_group_id = group.group_id;
        candidate.m_state.current_step = m_state.series.next_step;
        candidate.m_state.outcome = {};
        ++candidate.m_state.series.state_revision;
        swap(candidate);
        return ProgressionResult::APPLIED;
    }

    /// \brief Applies an event once; identical replays precede revision checks.
    ProgressionResult apply(const TradeSettlement& event, std::uint64_t expected_revision) {
        const auto receipt = m_receipts.find(event.settlement_id);
        if (receipt != m_receipts.end()) {
            return receipt->second == event ? ProgressionResult::DUPLICATE
                                            : ProgressionResult::SETTLEMENT_CONFLICT;
        }
        const auto check = check_revision(expected_revision);
        if (check != ProgressionResult::APPLIED) return check;
        if (!m_has_group || (m_state.status != SeriesStatus::WAITING_OUTCOME &&
                            m_state.status != SeriesStatus::UNRESOLVED &&
                            m_state.status != SeriesStatus::BLOCKED_ERROR)) {
            return ProgressionResult::INVALID_STATE;
        }

        auto candidate = *this;
        const auto result = candidate.m_group.apply(event);
        if (result != ProgressionResult::APPLIED) return result;
        const auto outcome = candidate.m_group.outcome();
        candidate.m_state.outcome = outcome;
        if (outcome.all_members_terminal &&
            outcome.aggregation_status == AggregationStatus::NUMERIC_OVERFLOW) {
            candidate.m_state.status = SeriesStatus::BLOCKED_ERROR;
        } else if (outcome.final) {
            if (outcome.net_profit) {
                ++candidate.m_state.series.next_step;
            }
            candidate.m_state.status = SeriesStatus::ACTIVE;
            candidate.m_state.current_intent_id = 0;
            candidate.m_state.current_group_id = 0;
            candidate.m_has_group = false;
        } else {
            candidate.m_state.status = outcome.unresolved ? SeriesStatus::UNRESOLVED
                                                        : SeriesStatus::WAITING_OUTCOME;
        }
        candidate.m_receipts.emplace(event.settlement_id, event);
        ++candidate.m_state.series.state_revision;
        swap(candidate);
        return ProgressionResult::APPLIED;
    }

    /// \brief Closes an idle series, never cancels an outstanding operation.
    ProgressionResult complete(std::uint64_t expected_revision) {
        const auto check = check_revision(expected_revision);
        if (check != ProgressionResult::APPLIED) return check;
        if (m_state.status != SeriesStatus::ACTIVE) return ProgressionResult::INVALID_STATE;
        m_state.status = SeriesStatus::COMPLETED;
        ++m_state.series.state_revision;
        return ProgressionResult::APPLIED;
    }

private:
    ProgressionResult check_revision(std::uint64_t expected_revision) const noexcept {
        if (!m_state.series.valid()) return ProgressionResult::INVALID_IDENTITY;
        if (m_state.series.state_revision != expected_revision) {
            return ProgressionResult::REVISION_CONFLICT;
        }
        if (expected_revision == (std::numeric_limits<std::uint64_t>::max)()) {
            return ProgressionResult::REVISION_OVERFLOW;
        }
        if (m_state.series.next_step > max_series_step) return ProgressionResult::STEP_LIMIT_REACHED;
        return ProgressionResult::APPLIED;
    }

    void swap(SeriesProgression& other) noexcept {
        using std::swap;
        swap(m_state, other.m_state);
        m_group.swap(other.m_group);
        swap(m_has_group, other.m_has_group);
        m_receipts.swap(other.m_receipts);
        m_used_intents.swap(other.m_used_intents);
        m_used_intent_keys.swap(other.m_used_intent_keys);
    }

    SeriesSnapshot m_state;
    ExecutionGroupAggregator m_group;
    bool m_has_group = false;
    std::map<SettlementId, TradeSettlement> m_receipts;
    std::set<IntentId> m_used_intents;
    std::set<std::string> m_used_intent_keys;
};

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_SERIES_PROGRESSION_HPP_INCLUDED
