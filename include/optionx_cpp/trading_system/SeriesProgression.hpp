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

    /// \brief Validates one durable logical completion using the same state
    /// machine as the live owner.
    ///
    /// `group_before` and `receipts_before` describe the durable state before
    /// the `incoming` receipts are applied. No owner state is modified by this
    /// helper.
    static ProgressionResult validate_completion_transition(
        const SeriesSnapshot& before,
        const SeriesSnapshot& candidate,
        ExecutionGroupAggregator group_before,
        const std::vector<TradeSettlement>& receipts_before,
        const std::set<IntentId>& used_intents,
        const std::set<std::string>& used_intent_keys,
        const std::vector<TradeSettlement>& incoming) {
        SeriesProgression owner(before.series);
        const auto hydrated = owner.hydrate(
            before, std::move(group_before), receipts_before,
            used_intents, used_intent_keys);
        if (hydrated != ProgressionResult::APPLIED) return hydrated;
        for (const auto& event : incoming) {
            const auto applied = owner.apply(event, owner.snapshot().series.state_revision);
            if (applied != ProgressionResult::APPLIED) return applied;
        }
        const auto actual = owner.snapshot();
        return same_snapshot(actual, candidate)
            ? ProgressionResult::APPLIED : ProgressionResult::INVALID_STATE;
    }

    /// \brief Hydrates the owner from a durable checkpoint and a fresh group owner.
    ///
    /// The checkpoint carries receipt and idempotency history because
    /// `SeriesSnapshot` alone is only a read projection. All validation is
    /// performed on a candidate copy before this owner is changed.
    ProgressionResult hydrate(
        const SeriesSnapshot& snapshot,
        ExecutionGroupAggregator group,
        const std::vector<TradeSettlement>& receipts,
        const std::set<IntentId>& used_intents,
        const std::set<std::string>& used_intent_keys) {
        if (!snapshot.series.valid() || snapshot.series.state_revision ==
                (std::numeric_limits<std::uint64_t>::max)()) {
            return ProgressionResult::INVALID_IDENTITY;
        }
        SeriesProgression candidate(snapshot.series);
        candidate.m_state = snapshot;
        candidate.m_group = std::move(group);
        for (const auto intent_id : used_intents) {
            if (intent_id == 0) return ProgressionResult::INVALID_IDENTITY;
        }
        for (const auto& key : used_intent_keys) {
            if (key.empty()) return ProgressionResult::INVALID_IDENTITY;
        }
        candidate.m_used_intents = used_intents;
        candidate.m_used_intent_keys = used_intent_keys;
        candidate.m_has_group = snapshot.status == SeriesStatus::WAITING_OUTCOME ||
                                snapshot.status == SeriesStatus::UNRESOLVED ||
                                snapshot.status == SeriesStatus::BLOCKED_ERROR;
        if (candidate.m_has_group && !candidate.m_group.valid()) {
            return ProgressionResult::INVALID_CONFIGURATION;
        }
        for (const auto& receipt : receipts) {
            if (!receipt.valid() || !candidate.m_receipts.emplace(receipt.settlement_id, receipt).second) {
                return ProgressionResult::SETTLEMENT_CONFLICT;
            }
            // Receipts from completed groups remain idempotency history, but
            // only receipts belonging to the current roster may be replayed
            // into the current aggregator.
            if (candidate.m_has_group &&
                receipt.execution_group_id == snapshot.current_group_id &&
                candidate.m_group.apply(receipt) != ProgressionResult::APPLIED) {
                return ProgressionResult::INVALID_OUTCOME;
            }
        }
        if (candidate.m_has_group && !same_outcome(candidate.m_group.outcome(), snapshot.outcome)) {
            return ProgressionResult::INVALID_STATE;
        }
        if (candidate.m_has_group && candidate.m_group.outcome().final &&
            snapshot.status == SeriesStatus::WAITING_OUTCOME) {
            return ProgressionResult::INVALID_STATE;
        }
        swap(candidate);
        return ProgressionResult::APPLIED;
    }

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
    static bool same_snapshot(const SeriesSnapshot& lhs, const SeriesSnapshot& rhs) noexcept {
        return lhs.series.series_id == rhs.series.series_id &&
               lhs.series.origin_signal_id == rhs.series.origin_signal_id &&
               lhs.series.next_step == rhs.series.next_step &&
               lhs.status == rhs.status &&
               lhs.current_intent_id == rhs.current_intent_id &&
               lhs.current_group_id == rhs.current_group_id &&
               lhs.current_step == rhs.current_step &&
               same_outcome(lhs.outcome, rhs.outcome);
    }

    static bool same_outcome(const GroupOutcome& lhs, const GroupOutcome& rhs) noexcept {
        if (lhs.final != rhs.final || lhs.unresolved != rhs.unresolved ||
            lhs.all_members_terminal != rhs.all_members_terminal ||
            lhs.completed_members != rhs.completed_members ||
            lhs.aggregation_status != rhs.aggregation_status || lhs.kind != rhs.kind ||
            lhs.net_profit.has_value() != rhs.net_profit.has_value()) return false;
        if (!lhs.net_profit) return true;
        return lhs.net_profit->units == rhs.net_profit->units &&
               lhs.net_profit->precision == rhs.net_profit->precision &&
               lhs.net_profit->currency == rhs.net_profit->currency;
    }

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
