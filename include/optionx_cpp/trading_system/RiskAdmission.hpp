#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_RISK_ADMISSION_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_RISK_ADMISSION_HPP_INCLUDED

/// \file RiskAdmission.hpp
/// \brief Defines the broker-independent, single-scope risk reservation owner.

#include "TradeIntent.hpp"
#include "PolicyTypes.hpp"
#include "Settlement.hpp"
#include "ExecutionGroupAggregator.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace optionx::trading_system {

enum class RiskScopeKind { ACCOUNT, STRATEGY, GROUP };

struct RiskScope {
    RiskScopeKind kind = RiskScopeKind::ACCOUNT;
    std::uint64_t scope_id = 0;

    bool valid() const noexcept { return scope_id != 0; }
};

enum class RiskAvailability {
    SUPPORTED,
    UNAVAILABLE,
    STALE
};

/// \brief Provider-owned account facts used by the admission owner.
struct RiskAccountSnapshot {
    RiskAvailability availability = RiskAvailability::UNAVAILABLE;
    MoneyAmount available_balance;
    std::int64_t captured_at_ms = 0;
    std::uint64_t revision = 0;

    bool valid() const noexcept {
        return availability == RiskAvailability::SUPPORTED &&
               available_balance.valid_metadata() && available_balance.units >= 0;
    }
};

/// \brief Limits fixed for one RiskAdmission owner.
struct RiskAdmissionConfig {
    RiskScope scope;
    std::int64_t max_snapshot_age_ms = 30'000;
    std::optional<MoneyAmount> max_reserved_risk;
    bool admission_blocked = false;

    bool valid() const noexcept {
        return scope.valid() && max_snapshot_age_ms >= 0 &&
               (!max_reserved_risk ||
                (max_reserved_risk->valid_metadata() && max_reserved_risk->units > 0));
    }
};

/// \brief One logical reservation; potential_risk covers the complete group.
struct RiskRequest {
    IntentId intent_id = 0;
    ExecutionGroupIdentity execution_group;
    ExecutionCardinality cardinality = ExecutionCardinality::ZERO_OR_ONE;
    IntentStatus intent_status = IntentStatus::PROPOSED;
    std::string idempotency_key;
    MoneyAmount stake;
    MoneyAmount potential_risk;

    static RiskRequest from_intent(
        const TradeIntent& intent,
        const MoneyAmount& stake,
        const MoneyAmount& potential_risk) {
        RiskRequest result;
        result.intent_id = intent.intent_id;
        result.execution_group = intent.execution_group;
        result.cardinality = intent.cardinality;
        result.intent_status = intent.status;
        result.idempotency_key = intent.idempotency_key;
        result.stake = stake;
        result.potential_risk = potential_risk;
        return result;
    }

    bool valid_identity() const noexcept {
        return intent_id != 0 && !idempotency_key.empty() &&
               execution_group.valid_for(cardinality) &&
               (intent_status == IntentStatus::PROPOSED ||
                intent_status == IntentStatus::ADMITTED);
    }
};

enum class RiskReservationState {
    RESERVED,
    SUBMITTED,
    EXECUTION_UNKNOWN,
    RELEASED,
    SETTLED
};

enum class RiskAdmissionKind {
    ADMITTED,
    APPLIED,
    DUPLICATE,
    REJECTED,
    UNAVAILABLE,
    CONFLICT,
    INVALID_STATE
};

enum class RiskAdmissionReason {
    NONE,
    RESERVED,
    COMMITTED,
    RELEASED,
    EXECUTION_UNKNOWN,
    SETTLED,
    INVALID_CONFIGURATION,
    INVALID_REQUEST,
    INVALID_RISK,
    CURRENCY_MISMATCH,
    PRECISION_MISMATCH,
    SNAPSHOT_UNAVAILABLE,
    SNAPSHOT_STALE,
    BALANCE_LIMIT_EXCEEDED,
    RISK_LIMIT_EXCEEDED,
    NUMERIC_OVERFLOW,
    UNKNOWN_RESERVATION,
    INVALID_TRANSITION,
    PAYLOAD_CONFLICT,
    REVISION_CONFLICT,
    INVALID_SETTLEMENT_EVIDENCE,
    RISK_SCOPE_BLOCKED
};

struct RiskReservation {
    std::uint64_t reservation_id = 0;
    RiskScope scope;
    IntentId intent_id = 0;
    ExecutionGroupIdentity execution_group;
    ExecutionCardinality cardinality = ExecutionCardinality::ZERO_OR_ONE;
    std::string idempotency_key;
    MoneyAmount stake;
    MoneyAmount potential_risk;
    RiskReservationState state = RiskReservationState::RELEASED;
    std::uint64_t account_revision = 0;

    bool valid() const noexcept {
        const bool valid_scope_kind = scope.kind == RiskScopeKind::ACCOUNT ||
                                      scope.kind == RiskScopeKind::STRATEGY ||
                                      scope.kind == RiskScopeKind::GROUP;
        return reservation_id != 0 && valid_scope_kind && scope.valid() && intent_id != 0 &&
               !idempotency_key.empty() && execution_group.valid_for(cardinality) &&
               stake.valid_metadata() && stake.units > 0 &&
               potential_risk.valid_metadata() && potential_risk.units > 0 &&
               stake.currency == potential_risk.currency &&
               stake.precision == potential_risk.precision &&
               (state == RiskReservationState::RESERVED ||
                state == RiskReservationState::SUBMITTED ||
                state == RiskReservationState::EXECUTION_UNKNOWN ||
                state == RiskReservationState::RELEASED ||
                state == RiskReservationState::SETTLED);
    }
};

/// \brief Typed proof that one logical execution group reached terminal state.
/// \details This is a structural trust boundary. The host must construct it
/// from the authoritative Batch 3 progression and settlement owner.
struct RiskSettlementEvidence {
    std::uint64_t reservation_id = 0;
    IntentId intent_id = 0;
    ExecutionGroupIdentity execution_group;
    GroupOutcome outcome;
    std::vector<TradeSettlement> settlements;

    bool valid_for(const RiskReservation& reservation) const {
        if (reservation_id != reservation.reservation_id ||
            intent_id != reservation.intent_id ||
            execution_group.group_id != reservation.execution_group.group_id ||
            execution_group.expected_members != reservation.execution_group.expected_members ||
            !outcome.final || !outcome.all_members_terminal || outcome.unresolved ||
            outcome.aggregation_status != AggregationStatus::READY ||
            !outcome.kind ||
            outcome.completed_members != execution_group.expected_members ||
            settlements.size() != execution_group.expected_members) {
            return false;
        }
        std::set<SettlementId> settlement_ids;
        std::set<std::uint64_t> trade_ids;
        for (const auto& settlement : settlements) {
            if (!settlement.valid() || !settlement.terminal() ||
                settlement.intent_id != intent_id ||
                settlement.execution_group_id != execution_group.group_id ||
                !settlement_ids.insert(settlement.settlement_id).second ||
                !trade_ids.insert(settlement.trade_id).second) {
                return false;
            }
        }
        std::vector<std::uint64_t> roster(trade_ids.begin(), trade_ids.end());
        ExecutionGroupAggregator aggregator(
            reservation.intent_id,
            reservation.execution_group,
            roster,
            reservation.potential_risk.currency,
            reservation.potential_risk.precision);
        if (!aggregator.valid()) {
            return false;
        }
        for (const auto& settlement : settlements) {
            if (aggregator.apply(settlement) != ProgressionResult::APPLIED) {
                return false;
            }
        }
        const auto actual = aggregator.outcome();
        if (actual.aggregation_status != AggregationStatus::READY) {
            return false;
        }
        return same_outcome(actual, outcome);
    }

private:
    static bool same_outcome(const GroupOutcome& lhs, const GroupOutcome& rhs) noexcept {
        if (lhs.final != rhs.final || lhs.unresolved != rhs.unresolved ||
            lhs.all_members_terminal != rhs.all_members_terminal ||
            lhs.completed_members != rhs.completed_members ||
            lhs.aggregation_status != rhs.aggregation_status || lhs.kind != rhs.kind ||
            lhs.net_profit.has_value() != rhs.net_profit.has_value()) {
            return false;
        }
        if (!lhs.net_profit) return true;
        return lhs.net_profit->units == rhs.net_profit->units &&
               lhs.net_profit->precision == rhs.net_profit->precision &&
               lhs.net_profit->currency == rhs.net_profit->currency;
    }
};

struct RiskAdmissionResult {
    RiskAdmissionKind kind = RiskAdmissionKind::INVALID_STATE;
    RiskAdmissionReason reason = RiskAdmissionReason::INVALID_CONFIGURATION;
    std::optional<RiskReservation> reservation;

    static RiskAdmissionResult applied(
        RiskAdmissionReason reason, const RiskReservation& reservation) {
        return {RiskAdmissionKind::APPLIED, reason, reservation};
    }

    static RiskAdmissionResult admitted(const RiskReservation& reservation) {
        return {RiskAdmissionKind::ADMITTED, RiskAdmissionReason::RESERVED, reservation};
    }

    static RiskAdmissionResult duplicate(const RiskReservation& reservation) {
        return {RiskAdmissionKind::DUPLICATE, RiskAdmissionReason::NONE, reservation};
    }

    static RiskAdmissionResult rejected(RiskAdmissionReason reason) noexcept {
        return {RiskAdmissionKind::REJECTED, reason, std::nullopt};
    }

    static RiskAdmissionResult unavailable(RiskAdmissionReason reason) noexcept {
        return {RiskAdmissionKind::UNAVAILABLE, reason, std::nullopt};
    }

    static RiskAdmissionResult conflict(RiskAdmissionReason reason) noexcept {
        return {RiskAdmissionKind::CONFLICT, reason, std::nullopt};
    }

    static RiskAdmissionResult invalid_state(RiskAdmissionReason reason) noexcept {
        return {RiskAdmissionKind::INVALID_STATE, reason, std::nullopt};
    }

    bool succeeded() const noexcept {
        return kind == RiskAdmissionKind::ADMITTED || kind == RiskAdmissionKind::APPLIED ||
               (kind == RiskAdmissionKind::DUPLICATE && reservation &&
                reservation->state == RiskReservationState::RESERVED);
    }

    bool retains_risk() const noexcept {
        return reservation &&
               (reservation->state == RiskReservationState::RESERVED ||
                reservation->state == RiskReservationState::SUBMITTED ||
                reservation->state == RiskReservationState::EXECUTION_UNKNOWN);
    }

    bool is_submission_admission() const noexcept {
        return kind == RiskAdmissionKind::ADMITTED ||
               (kind == RiskAdmissionKind::DUPLICATE && reservation &&
                reservation->state == RiskReservationState::RESERVED);
    }
};

/// \class RiskAdmission
/// \brief Atomically owns reservations for one account, strategy, or group scope.
/// \details Calls are safe from concurrent clients of this owner. Separate
/// owners do not coordinate; the host must create one owner per risk scope.
class RiskAdmission {
public:
    /// \brief Holds this owner's mutex across durable preparation.
    ///
    /// A preparation lease is the authority boundary between in-memory risk
    /// verification and the storage commit. Risk transitions block until the
    /// lease is released. The owner must outlive the lease. Acquire, use and
    /// release it on the same thread; do not call locking owner methods while
    /// holding it. Multiple owners are leased in (scope kind, scope ID) order,
    /// before acquiring a TradingSystemStorage writer guard.
    class PreparationLease {
    public:
        PreparationLease() = default;
        PreparationLease(PreparationLease&&) noexcept = default;
        PreparationLease& operator=(PreparationLease&&) noexcept = default;
        PreparationLease(const PreparationLease&) = delete;
        PreparationLease& operator=(const PreparationLease&) = delete;

        RiskAdmissionResult verify_group_admission(
            const RiskRequest& request) const {
            if (!m_owner || !m_lock.owns_lock()) {
                return RiskAdmissionResult::invalid_state(
                    RiskAdmissionReason::INVALID_CONFIGURATION);
            }
            return m_owner->verify_group_admission_locked(request);
        }

    private:
        friend class RiskAdmission;
        explicit PreparationLease(const RiskAdmission& owner)
            : m_owner(&owner), m_lock(owner.m_mutex) {}

        const RiskAdmission* m_owner = nullptr;
        std::unique_lock<std::mutex> m_lock;
    };

    explicit RiskAdmission(
        RiskAdmissionConfig config,
        std::uint64_t first_reservation_id = 1)
        : m_config(std::move(config)),
          m_next_reservation_id(first_reservation_id),
          m_id_sequence_valid(first_reservation_id != 0) {}

    bool valid() const noexcept { return m_config.valid() && m_id_sequence_valid; }

    /// \brief Acquires the authority lease used by durable preparation.
    PreparationLease acquire_preparation_lease() const {
        return PreparationLease(*this);
    }

    /// \brief Returns the immutable scope owned by this admission authority.
    RiskScope scope() const noexcept {
        return m_config.scope;
    }

    /// \brief Verifies that a reservation was admitted by this owner.
    ///
    /// A durable ReservationRecord is only a storage fact.  Callers that cross
    /// into execution preparation must present the corresponding owner token;
    /// this check is the narrow authority boundary and does not mutate state.
    RiskAdmissionResult verify_admission(const RiskReservation& token) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!valid()) {
            return RiskAdmissionResult::invalid_state(
                RiskAdmissionReason::INVALID_CONFIGURATION);
        }
        const auto found = m_by_key.find(token.idempotency_key);
        if (found == m_by_key.end()) {
            return RiskAdmissionResult::invalid_state(
                RiskAdmissionReason::UNKNOWN_RESERVATION);
        }
        if (!matches(found->second, token)) {
            return RiskAdmissionResult::conflict(RiskAdmissionReason::PAYLOAD_CONFLICT);
        }
        if (found->second.state != RiskReservationState::RESERVED) {
            return RiskAdmissionResult::invalid_state(
                RiskAdmissionReason::INVALID_TRANSITION);
        }
        return RiskAdmissionResult::admitted(found->second);
    }

    /// \brief Verifies a group-level admission owned by this scope.
    ///
    /// Durable execution may split one owner reservation across several
    /// physical roster members.  In that case the owner token is the aggregate
    /// reservation, while the storage records retain member-level identities.
    RiskAdmissionResult verify_group_admission(const RiskRequest& request) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return verify_group_admission_locked(request);
    }

private:
    RiskAdmissionResult verify_group_admission_locked(
        const RiskRequest& request) const {
        if (!valid() || !request.valid_identity()) {
            return RiskAdmissionResult::invalid_state(
                RiskAdmissionReason::INVALID_CONFIGURATION);
        }
        if (m_config.admission_blocked) {
            return RiskAdmissionResult::unavailable(
                RiskAdmissionReason::RISK_SCOPE_BLOCKED);
        }
        const auto found = m_by_intent.find(request.intent_id);
        if (found == m_by_intent.end()) {
            return RiskAdmissionResult::invalid_state(
                RiskAdmissionReason::UNKNOWN_RESERVATION);
        }
        const auto stored = m_by_key.find(found->second);
        if (stored == m_by_key.end()) {
            return RiskAdmissionResult::invalid_state(
                RiskAdmissionReason::UNKNOWN_RESERVATION);
        }
        if (stored->second.state != RiskReservationState::RESERVED) {
            return RiskAdmissionResult::invalid_state(
                RiskAdmissionReason::INVALID_TRANSITION);
        }
        if (!same_request(stored->second, request) ||
            stored->second.idempotency_key != request.idempotency_key) {
            return RiskAdmissionResult::conflict(RiskAdmissionReason::PAYLOAD_CONFLICT);
        }
        return RiskAdmissionResult::admitted(stored->second);
    }

public:
    /// \brief Restores durable fail-closed readiness before new admission.
    ///
    /// The storage layer supplies the typed gate state; this owner method only
    /// applies that decision and never clears existing reservations.
    RiskAdmissionResult restore_admission_gate(bool blocked) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_config.valid() || !m_id_sequence_valid) {
            return RiskAdmissionResult::invalid_state(RiskAdmissionReason::INVALID_CONFIGURATION);
        }
        m_config.admission_blocked = blocked;
        return {RiskAdmissionKind::APPLIED, RiskAdmissionReason::NONE, std::nullopt};
    }

    RiskAdmissionResult update_snapshot(const RiskAccountSnapshot& snapshot) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!valid() || !snapshot.valid()) {
            return RiskAdmissionResult::unavailable(
                snapshot.availability == RiskAvailability::STALE
                    ? RiskAdmissionReason::SNAPSHOT_STALE
                    : RiskAdmissionReason::SNAPSHOT_UNAVAILABLE);
        }
        if (m_snapshot && snapshot.revision < m_snapshot->revision) {
            return RiskAdmissionResult::conflict(RiskAdmissionReason::REVISION_CONFLICT);
        }
        if (m_snapshot && snapshot.revision == m_snapshot->revision) {
            return same_snapshot(*m_snapshot, snapshot)
                ? duplicate_snapshot()
                : RiskAdmissionResult::conflict(RiskAdmissionReason::REVISION_CONFLICT);
        }
        if (m_snapshot && !same_currency(m_snapshot->available_balance, snapshot.available_balance)) {
            return RiskAdmissionResult::conflict(
                m_snapshot->available_balance.currency != snapshot.available_balance.currency
                    ? RiskAdmissionReason::CURRENCY_MISMATCH
                    : RiskAdmissionReason::PRECISION_MISMATCH);
        }
        m_snapshot = snapshot;
        return {RiskAdmissionKind::APPLIED, RiskAdmissionReason::NONE, std::nullopt};
    }

    RiskAdmissionResult reserve(const RiskRequest& request, std::int64_t now_ms) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!valid()) return RiskAdmissionResult::rejected(RiskAdmissionReason::INVALID_CONFIGURATION);
        if (m_config.admission_blocked) {
            return RiskAdmissionResult::unavailable(RiskAdmissionReason::RISK_SCOPE_BLOCKED);
        }
        if (!request.valid_identity()) return RiskAdmissionResult::rejected(RiskAdmissionReason::INVALID_REQUEST);

        const auto existing = m_by_key.find(request.idempotency_key);
        if (existing != m_by_key.end()) {
            if (same_request(existing->second, request)) return RiskAdmissionResult::duplicate(existing->second);
            return RiskAdmissionResult::conflict(RiskAdmissionReason::PAYLOAD_CONFLICT);
        }
        const auto by_intent = m_by_intent.find(request.intent_id);
        if (by_intent != m_by_intent.end()) {
            return RiskAdmissionResult::conflict(RiskAdmissionReason::PAYLOAD_CONFLICT);
        }
        if (m_id_sequence_exhausted) {
            return RiskAdmissionResult::rejected(RiskAdmissionReason::NUMERIC_OVERFLOW);
        }
        if (!valid_money(request.stake) || !valid_money(request.potential_risk)) {
            return RiskAdmissionResult::rejected(RiskAdmissionReason::INVALID_RISK);
        }
        if (!same_currency(request.stake, request.potential_risk)) {
            return RiskAdmissionResult::rejected(
                request.stake.currency != request.potential_risk.currency
                    ? RiskAdmissionReason::CURRENCY_MISMATCH
                    : RiskAdmissionReason::PRECISION_MISMATCH);
        }
        if (request.stake.units <= 0 || request.potential_risk.units <= 0) {
            return RiskAdmissionResult::rejected(RiskAdmissionReason::INVALID_RISK);
        }
        if (!m_snapshot || m_snapshot->availability != RiskAvailability::SUPPORTED) {
            return RiskAdmissionResult::unavailable(RiskAdmissionReason::SNAPSHOT_UNAVAILABLE);
        }
        if (!same_currency(m_snapshot->available_balance, request.potential_risk)) {
            return RiskAdmissionResult::rejected(
                m_snapshot->available_balance.currency != request.potential_risk.currency
                    ? RiskAdmissionReason::CURRENCY_MISMATCH
                    : RiskAdmissionReason::PRECISION_MISMATCH);
        }
        if (now_ms < m_snapshot->captured_at_ms ||
            static_cast<std::uint64_t>(now_ms) -
                    static_cast<std::uint64_t>(m_snapshot->captured_at_ms) >
                static_cast<std::uint64_t>(m_config.max_snapshot_age_ms)) {
            return RiskAdmissionResult::unavailable(RiskAdmissionReason::SNAPSHOT_STALE);
        }
        const auto reserved = active_risk_units();
        if (!can_add(reserved, request.potential_risk.units)) {
            return RiskAdmissionResult::rejected(RiskAdmissionReason::NUMERIC_OVERFLOW);
        }
        const auto next_reserved = reserved + request.potential_risk.units;
        if (next_reserved > m_snapshot->available_balance.units) {
            return RiskAdmissionResult::rejected(RiskAdmissionReason::BALANCE_LIMIT_EXCEEDED);
        }
        if (m_config.max_reserved_risk) {
            if (m_config.max_reserved_risk->currency != request.potential_risk.currency) {
                return RiskAdmissionResult::rejected(RiskAdmissionReason::CURRENCY_MISMATCH);
            }
            if (m_config.max_reserved_risk->precision != request.potential_risk.precision) {
                return RiskAdmissionResult::rejected(RiskAdmissionReason::PRECISION_MISMATCH);
            }
            if (next_reserved > m_config.max_reserved_risk->units) {
                return RiskAdmissionResult::rejected(RiskAdmissionReason::RISK_LIMIT_EXCEEDED);
            }
        }

        RiskReservation reservation;
        reservation.reservation_id = m_next_reservation_id;
        reservation.scope = m_config.scope;
        reservation.intent_id = request.intent_id;
        reservation.execution_group = normalized_group(request);
        reservation.cardinality = request.cardinality;
        reservation.idempotency_key = request.idempotency_key;
        reservation.stake = request.stake;
        reservation.potential_risk = request.potential_risk;
        reservation.state = RiskReservationState::RESERVED;
        reservation.account_revision = m_snapshot->revision;
        auto candidate_by_key = m_by_key;
        auto candidate_by_intent = m_by_intent;
        if (!candidate_by_key.emplace(reservation.idempotency_key, reservation).second ||
            !candidate_by_intent.emplace(reservation.intent_id, reservation.idempotency_key).second) {
            return RiskAdmissionResult::conflict(RiskAdmissionReason::PAYLOAD_CONFLICT);
        }
        const bool exhaust_ids =
            reservation.reservation_id == (std::numeric_limits<std::uint64_t>::max)();
        const auto next_id = exhaust_ids ? 0 : reservation.reservation_id + 1;
        m_by_key.swap(candidate_by_key);
        m_by_intent.swap(candidate_by_intent);
        m_next_reservation_id = next_id;
        m_id_sequence_exhausted = exhaust_ids;
        return RiskAdmissionResult::admitted(reservation);
    }

    /// \brief Marks submission-in-progress before any broker-side effect.
    RiskAdmissionResult commit(const RiskReservation& token) {
        return transition(token, RiskReservationState::SUBMITTED,
                          RiskAdmissionReason::COMMITTED,
                          [](RiskReservationState state) { return state == RiskReservationState::RESERVED; });
    }

    RiskAdmissionResult release(const RiskReservation& token) {
        return transition(token, RiskReservationState::RELEASED,
                          RiskAdmissionReason::RELEASED,
                          [](RiskReservationState state) { return state == RiskReservationState::RESERVED; });
    }

    RiskAdmissionResult mark_unknown(const RiskReservation& token) {
        return transition(token, RiskReservationState::EXECUTION_UNKNOWN,
                          RiskAdmissionReason::EXECUTION_UNKNOWN,
                          [](RiskReservationState state) {
                              return state == RiskReservationState::RESERVED ||
                                     state == RiskReservationState::SUBMITTED;
                          });
    }

    RiskAdmissionResult settle(
        const RiskReservation& token,
        const RiskSettlementEvidence& evidence) {
        if (!evidence.valid_for(token)) {
            return RiskAdmissionResult::rejected(
                RiskAdmissionReason::INVALID_SETTLEMENT_EVIDENCE);
        }
        return transition(token, RiskReservationState::SETTLED,
                          RiskAdmissionReason::SETTLED,
                          [](RiskReservationState state) {
                              return state == RiskReservationState::SUBMITTED ||
                                     state == RiskReservationState::EXECUTION_UNKNOWN;
                          });
    }

    std::int64_t reserved_risk_units() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return active_risk_units();
    }

    std::optional<RiskAccountSnapshot> account_snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_snapshot;
    }

    /// \brief Replaces in-memory state from validated durable facts.
    ///
    /// Validation completes before any owner state is changed. This method is
    /// the narrow hydration boundary for Batch 5B; it does not query a broker
    /// and does not infer missing reservations.
    RiskAdmissionResult hydrate(
        const RiskAccountSnapshot& snapshot,
        const std::vector<RiskReservation>& reservations,
        std::uint64_t next_reservation_id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_config.valid() || !snapshot.valid()) {
            return RiskAdmissionResult::invalid_state(RiskAdmissionReason::INVALID_CONFIGURATION);
        }
        std::map<std::string, RiskReservation> by_key;
        std::map<IntentId, std::string> by_intent;
        std::uint64_t maximum_id = 0;
        std::set<std::uint64_t> reservation_ids;
        std::int64_t restored_active_risk = 0;
        for (const auto& reservation : reservations) {
            if (!reservation.valid() || reservation.scope.kind != m_config.scope.kind ||
                reservation.scope.scope_id != m_config.scope.scope_id ||
                reservation.potential_risk.currency != snapshot.available_balance.currency ||
                reservation.potential_risk.precision != snapshot.available_balance.precision ||
                !by_key.emplace(reservation.idempotency_key, reservation).second ||
                !by_intent.emplace(reservation.intent_id, reservation.idempotency_key).second ||
                !reservation_ids.insert(reservation.reservation_id).second) {
                return RiskAdmissionResult::conflict(RiskAdmissionReason::PAYLOAD_CONFLICT);
            }
            if (reservation.reservation_id > maximum_id) maximum_id = reservation.reservation_id;
            if (reservation.state != RiskReservationState::RELEASED &&
                reservation.state != RiskReservationState::SETTLED) {
                if (reservation.potential_risk.units >
                    (std::numeric_limits<std::int64_t>::max)() - restored_active_risk) {
                    return RiskAdmissionResult::rejected(RiskAdmissionReason::NUMERIC_OVERFLOW);
                }
                restored_active_risk += reservation.potential_risk.units;
            }
        }
        if (restored_active_risk > snapshot.available_balance.units ||
            (m_config.max_reserved_risk &&
             restored_active_risk > m_config.max_reserved_risk->units)) {
            return RiskAdmissionResult::rejected(RiskAdmissionReason::BALANCE_LIMIT_EXCEEDED);
        }
        const bool exhausted = next_reservation_id == 0;
        if (exhausted && maximum_id != (std::numeric_limits<std::uint64_t>::max)()) {
            return RiskAdmissionResult::rejected(RiskAdmissionReason::NUMERIC_OVERFLOW);
        }
        if (!exhausted && maximum_id == (std::numeric_limits<std::uint64_t>::max)()) {
            return RiskAdmissionResult::rejected(RiskAdmissionReason::NUMERIC_OVERFLOW);
        }
        if (!exhausted && next_reservation_id <= maximum_id) {
            return RiskAdmissionResult::rejected(RiskAdmissionReason::NUMERIC_OVERFLOW);
        }
        m_snapshot = snapshot;
        m_by_key = std::move(by_key);
        m_by_intent = std::move(by_intent);
        m_next_reservation_id = next_reservation_id;
        m_id_sequence_valid = true;
        m_id_sequence_exhausted = exhausted;
        return {RiskAdmissionKind::APPLIED, RiskAdmissionReason::NONE, std::nullopt};
    }

private:
    static RiskAdmissionResult duplicate_snapshot() noexcept {
        return {RiskAdmissionKind::DUPLICATE, RiskAdmissionReason::NONE, std::nullopt};
    }

    static bool same_currency(const MoneyAmount& lhs, const MoneyAmount& rhs) noexcept {
        return lhs.currency == rhs.currency && lhs.precision == rhs.precision;
    }

    static bool valid_money(const MoneyAmount& value) noexcept {
        return value.valid_metadata();
    }

    static bool same_snapshot(const RiskAccountSnapshot& lhs, const RiskAccountSnapshot& rhs) noexcept {
        return lhs.availability == rhs.availability && lhs.available_balance.units == rhs.available_balance.units &&
               same_currency(lhs.available_balance, rhs.available_balance) &&
               lhs.captured_at_ms == rhs.captured_at_ms;
    }

    static bool same_request(const RiskReservation& stored, const RiskRequest& request) noexcept {
        const auto group = normalized_group(request);
        return stored.intent_id == request.intent_id &&
               stored.execution_group.group_id == group.group_id &&
               stored.execution_group.expected_members == group.expected_members &&
               stored.cardinality == request.cardinality &&
               stored.stake.units == request.stake.units &&
               same_currency(stored.stake, request.stake) &&
               stored.potential_risk.units == request.potential_risk.units &&
               same_currency(stored.potential_risk, request.potential_risk);
    }

    static ExecutionGroupIdentity normalized_group(const RiskRequest& request) noexcept {
        auto group = request.execution_group;
        if (request.cardinality == ExecutionCardinality::ZERO_OR_ONE) {
            group.expected_members = 1;
        }
        return group;
    }

    static bool can_add(std::int64_t lhs, std::int64_t rhs) noexcept {
        return rhs >= 0 && lhs <= (std::numeric_limits<std::int64_t>::max)() - rhs;
    }

    std::int64_t active_risk_units() const noexcept {
        std::int64_t total = 0;
        for (const auto& entry : m_by_key) {
            const auto& reservation = entry.second;
            if (reservation.state == RiskReservationState::RELEASED ||
                reservation.state == RiskReservationState::SETTLED) continue;
            if (can_add(total, reservation.potential_risk.units)) {
                total += reservation.potential_risk.units;
            } else {
                return (std::numeric_limits<std::int64_t>::max)();
            }
        }
        return total;
    }

    static bool matches(const RiskReservation& stored, const RiskReservation& token) noexcept {
        return stored.reservation_id == token.reservation_id && stored.scope.kind == token.scope.kind &&
               stored.scope.scope_id == token.scope.scope_id && stored.intent_id == token.intent_id &&
               stored.idempotency_key == token.idempotency_key &&
               stored.execution_group.group_id == token.execution_group.group_id &&
               stored.execution_group.expected_members == token.execution_group.expected_members &&
               stored.cardinality == token.cardinality &&
               stored.stake.units == token.stake.units &&
               same_currency(stored.stake, token.stake) &&
               stored.potential_risk.units == token.potential_risk.units &&
               same_currency(stored.potential_risk, token.potential_risk);
    }

    template <class Allowed>
    RiskAdmissionResult transition(
        const RiskReservation& token,
        RiskReservationState target,
        RiskAdmissionReason reason,
        Allowed allowed) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_by_key.find(token.idempotency_key);
        if (found == m_by_key.end()) return RiskAdmissionResult::invalid_state(RiskAdmissionReason::UNKNOWN_RESERVATION);
        auto& stored = found->second;
        if (!matches(stored, token)) return RiskAdmissionResult::conflict(RiskAdmissionReason::PAYLOAD_CONFLICT);
        if (stored.state == target) return RiskAdmissionResult::duplicate(stored);
        if (!allowed(stored.state)) return RiskAdmissionResult::invalid_state(RiskAdmissionReason::INVALID_TRANSITION);
        stored.state = target;
        return RiskAdmissionResult::applied(reason, stored);
    }

    RiskAdmissionConfig m_config;
    mutable std::mutex m_mutex;
    std::optional<RiskAccountSnapshot> m_snapshot;
    std::uint64_t m_next_reservation_id = 1;
    bool m_id_sequence_valid = true;
    bool m_id_sequence_exhausted = false;
    std::map<std::string, RiskReservation> m_by_key;
    std::map<IntentId, std::string> m_by_intent;
};

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_RISK_ADMISSION_HPP_INCLUDED
