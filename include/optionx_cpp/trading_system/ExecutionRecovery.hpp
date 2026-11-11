#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_EXECUTION_RECOVERY_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_EXECUTION_RECOVERY_HPP_INCLUDED

/// \file ExecutionRecovery.hpp
/// \brief Offline execution journal, simulated broker and fail-closed recovery.

#include "TradingSystemStorage.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace optionx::trading_system {

enum class ExecutionOperationState {
    PREPARED,
    SUBMISSION_STARTED,
    BROKER_ACCEPTED,
    BROKER_ID_PERSISTED,
    EXECUTED,
    SETTLED,
    EXECUTION_UNKNOWN,
    RECONCILIATION_REQUIRED,
    REJECTED,
    CANCELLED
};

enum class ReconciliationKind {
    MATCH_CONFIRMED,
    NOT_EXECUTED_PROVEN,
    AMBIGUOUS,
    HISTORY_UNAVAILABLE,
    EXECUTION_UNKNOWN
};

inline const char* to_string(ExecutionOperationState value) noexcept {
    switch (value) {
        case ExecutionOperationState::PREPARED: return "PREPARED";
        case ExecutionOperationState::SUBMISSION_STARTED: return "SUBMISSION_STARTED";
        case ExecutionOperationState::BROKER_ACCEPTED: return "BROKER_ACCEPTED";
        case ExecutionOperationState::BROKER_ID_PERSISTED: return "BROKER_ID_PERSISTED";
        case ExecutionOperationState::EXECUTED: return "EXECUTED";
        case ExecutionOperationState::SETTLED: return "SETTLED";
        case ExecutionOperationState::EXECUTION_UNKNOWN: return "EXECUTION_UNKNOWN";
        case ExecutionOperationState::RECONCILIATION_REQUIRED: return "RECONCILIATION_REQUIRED";
        case ExecutionOperationState::REJECTED: return "REJECTED";
        case ExecutionOperationState::CANCELLED: return "CANCELLED";
    }
    return "UNKNOWN";
}

inline ExecutionOperationState execution_operation_state_from_string(
    const std::string& value) {
    const std::pair<const char*, ExecutionOperationState> values[] = {
        {"PREPARED", ExecutionOperationState::PREPARED},
        {"SUBMISSION_STARTED", ExecutionOperationState::SUBMISSION_STARTED},
        {"BROKER_ACCEPTED", ExecutionOperationState::BROKER_ACCEPTED},
        {"BROKER_ID_PERSISTED", ExecutionOperationState::BROKER_ID_PERSISTED},
        {"EXECUTED", ExecutionOperationState::EXECUTED},
        {"SETTLED", ExecutionOperationState::SETTLED},
        {"EXECUTION_UNKNOWN", ExecutionOperationState::EXECUTION_UNKNOWN},
        {"RECONCILIATION_REQUIRED", ExecutionOperationState::RECONCILIATION_REQUIRED},
        {"REJECTED", ExecutionOperationState::REJECTED},
        {"CANCELLED", ExecutionOperationState::CANCELLED}
    };
    for (const auto& item : values) {
        if (value == item.first) return item.second;
    }
    throw std::invalid_argument("unknown execution operation state: " + value);
}

/// \brief One immutable state observation in the durable execution journal.
struct ExecutionOperationRecord {
    std::string operation_id;
    std::string client_operation_id;
    IntentId intent_id = 0;
    ExecutionGroupId group_id = 0;
    std::uint64_t trade_id = 0;
    std::uint64_t account_id = 0;
    std::uint64_t risk_scope_id = 0;
    ExecutionOperationState state = ExecutionOperationState::PREPARED;
    std::uint64_t state_revision = 0;
    std::string broker_trade_id;
    std::int64_t occurred_at_ms = 0;

    bool valid() const noexcept {
        return !operation_id.empty() && !client_operation_id.empty() && intent_id != 0 &&
               group_id != 0 && trade_id != 0 && account_id != 0 && risk_scope_id != 0;
    }
};

inline void to_json(nlohmann::json& json, const ExecutionOperationRecord& value) {
    json = nlohmann::json{
        {"operation_id", value.operation_id},
        {"client_operation_id", value.client_operation_id},
        {"intent_id", value.intent_id},
        {"group_id", value.group_id},
        {"trade_id", value.trade_id},
        {"account_id", value.account_id},
        {"risk_scope_id", value.risk_scope_id},
        {"state", to_string(value.state)},
        {"state_revision", value.state_revision},
        {"broker_trade_id", value.broker_trade_id},
        {"occurred_at_ms", value.occurred_at_ms}
    };
}

inline void from_json(const nlohmann::json& json, ExecutionOperationRecord& value) {
    value.operation_id = json.value("operation_id", std::string{});
    value.client_operation_id = json.value("client_operation_id", std::string{});
    value.intent_id = json.value("intent_id", IntentId{0});
    value.group_id = json.value("group_id", ExecutionGroupId{0});
    value.trade_id = json.value("trade_id", std::uint64_t{0});
    value.account_id = json.value("account_id", std::uint64_t{0});
    value.risk_scope_id = json.value("risk_scope_id", std::uint64_t{0});
    value.state = execution_operation_state_from_string(
        json.value("state", std::string{"PREPARED"}));
    value.state_revision = json.value("state_revision", std::uint64_t{0});
    value.broker_trade_id = json.value("broker_trade_id", std::string{});
    value.occurred_at_ms = json.value("occurred_at_ms", std::int64_t{0});
}

/// \brief Deterministic clock used by offline recovery tests and simulations.
class FakeExecutionClock {
public:
    explicit FakeExecutionClock(std::int64_t now_ms = 0) : m_now_ms(now_ms) {}
    std::int64_t now_ms() const noexcept { return m_now_ms; }
    void advance(std::int64_t delta_ms) noexcept { m_now_ms += delta_ms; }

private:
    std::int64_t m_now_ms;
};

/// \brief Typed result of a broker-history lookup.
struct ReconciliationResult {
    ReconciliationKind kind = ReconciliationKind::EXECUTION_UNKNOWN;
    std::string broker_trade_id;
    std::optional<TradeSettlement> settlement;
    std::string diagnostic;
};

enum class RecoveryResultKind {
    READY,
    STORAGE_ERROR,
    CORRUPTED,
    UNSUPPORTED_SCHEMA
};

/// \brief Typed result of rebuilding the immutable execution-operation view.
struct RecoveryResult {
    RecoveryResultKind kind = RecoveryResultKind::STORAGE_ERROR;
    std::map<std::string, ExecutionOperationRecord> operations;
    std::set<std::uint64_t> affected_risk_scopes;
    std::string diagnostic;

    bool succeeded() const noexcept { return kind == RecoveryResultKind::READY; }
};

/// \brief Admission decision reconstructed from all durable recovery facts.
struct RecoveryReadiness {
    RecoveryResultKind recovery_kind = RecoveryResultKind::STORAGE_ERROR;
    bool journal_valid = false;
    bool global_block = true;
    std::set<std::uint64_t> blocked_risk_scopes;
    std::string diagnostic;

    bool scope_admission_allowed(std::uint64_t risk_scope_id) const noexcept {
        return journal_valid && !global_block &&
               blocked_risk_scopes.count(risk_scope_id) == 0;
    }
};

struct SimulatedExecutionCommand {
    std::string operation_id;
    std::string client_operation_id;
    std::uint64_t account_id = 0;
    std::uint64_t trade_id = 0;
};

enum class SimulatedSubmissionPlan {
    SUCCESS,
    PRE_SUBMIT_REJECTION,
    ACCEPTED_RESPONSE_LOST,
    ACCEPTED_WITHOUT_BROKER_ID,
    TIMEOUT
};

struct SimulatedSubmissionResult {
    bool side_effect_occurred = false;
    bool accepted = false;
    bool response_lost = false;
    bool broker_id_available = false;
    std::string broker_trade_id;
};

/// \brief Offline broker model with explicit side effects and idempotency.
class SimulatedExecutionAdapter {
public:
    struct HistoryEntry {
        std::string operation_id;
        std::string client_operation_id;
        std::string broker_trade_id;
        std::uint64_t account_id = 0;
        std::uint64_t trade_id = 0;
        std::optional<TradeSettlement> settlement;
    };

    /// \brief State that belongs to the simulated external broker, not OptionX.
    struct State {
        std::uint64_t next_broker_id = 0;
        std::map<std::string, std::size_t> submission_counts;
        std::vector<HistoryEntry> history;
    };

    explicit SimulatedExecutionAdapter(bool broker_idempotency = true)
        : SimulatedExecutionAdapter(std::make_shared<State>(), broker_idempotency) {}

    explicit SimulatedExecutionAdapter(std::shared_ptr<State> state,
                                       bool broker_idempotency = true)
        : m_state(std::move(state)), m_broker_idempotency(broker_idempotency) {
        if (!m_state) m_state = std::make_shared<State>();
    }

    const std::shared_ptr<State>& state() const noexcept { return m_state; }

    void set_plan(const std::string& operation_id, SimulatedSubmissionPlan plan) {
        m_plans[operation_id] = plan;
    }

    void set_history_available(bool available) noexcept { m_history_available = available; }
    void set_proven_not_executed(const std::string& operation_id, bool value) {
        m_not_executed[operation_id] = value;
    }

    void add_history_entry(HistoryEntry entry) { m_state->history.push_back(std::move(entry)); }

    bool attach_settlement(const std::string& operation_id,
                           TradeSettlement settlement) {
        HistoryEntry* match = nullptr;
        for (auto& entry : m_state->history) {
            if (entry.operation_id != operation_id) continue;
            if (match) return false;
            match = &entry;
        }
        if (!match) return false;
        match->settlement = std::move(settlement);
        return true;
    }

    SimulatedSubmissionResult submit(const SimulatedExecutionCommand& command) {
        ++m_state->submission_counts[command.operation_id];
        const auto existing = find_unique(command.operation_id);
        if (existing && m_broker_idempotency) {
            return {true, true, false, !existing->broker_trade_id.empty(), existing->broker_trade_id};
        }
        const auto plan = m_plans.count(command.operation_id)
            ? m_plans.at(command.operation_id) : SimulatedSubmissionPlan::SUCCESS;
        if (plan == SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION) {
            return {};
        }
        if (plan == SimulatedSubmissionPlan::TIMEOUT) {
            return {true, false, true, false, {}};
        }

        HistoryEntry entry;
        entry.operation_id = command.operation_id;
        entry.client_operation_id = command.client_operation_id;
        entry.account_id = command.account_id;
        entry.trade_id = command.trade_id;
        entry.broker_trade_id = "broker-" + std::to_string(++m_state->next_broker_id);
        m_state->history.push_back(entry);
        const bool id_available = plan != SimulatedSubmissionPlan::ACCEPTED_WITHOUT_BROKER_ID;
        return {true, true, plan == SimulatedSubmissionPlan::ACCEPTED_RESPONSE_LOST,
                id_available, id_available ? entry.broker_trade_id : std::string{}};
    }

    std::size_t submission_count(const std::string& operation_id) const noexcept {
        const auto found = m_state->submission_counts.find(operation_id);
        return found == m_state->submission_counts.end() ? 0U : found->second;
    }

    ReconciliationResult reconcile(const ExecutionOperationRecord& operation) const {
        if (!m_history_available) return {ReconciliationKind::HISTORY_UNAVAILABLE, {}, std::nullopt,
                                           "simulated history is unavailable"};
        const auto matches = matching_entries(operation);
        if (matches.size() == 1) {
            const auto& entry = *matches.front();
            return {ReconciliationKind::MATCH_CONFIRMED, entry.broker_trade_id,
                    entry.settlement, "operation identity matched"};
        }
        if (matches.size() > 1) return {ReconciliationKind::AMBIGUOUS, {}, std::nullopt,
                                        "multiple physical history candidates"};
        const auto not_executed = m_not_executed.find(operation.operation_id);
        if (not_executed != m_not_executed.end() && not_executed->second) {
            return {ReconciliationKind::NOT_EXECUTED_PROVEN, {}, std::nullopt,
                    "simulated broker proves no side effect"};
        }
        return {ReconciliationKind::EXECUTION_UNKNOWN, {}, std::nullopt,
                "no authoritative history match"};
    }

private:
    const HistoryEntry* find_unique(const std::string& operation_id) const {
        const auto matches = matching_entries(operation_id);
        return matches.size() == 1 ? matches.front() : nullptr;
    }

    std::vector<const HistoryEntry*> matching_entries(const std::string& operation_id) const {
        std::vector<const HistoryEntry*> result;
        for (const auto& entry : m_state->history) {
            if (entry.operation_id == operation_id) result.push_back(&entry);
        }
        return result;
    }

    std::vector<const HistoryEntry*> matching_entries(
        const ExecutionOperationRecord& operation) const {
        std::vector<const HistoryEntry*> result;
        for (auto& entry : m_state->history) {
            const bool operation_match = entry.operation_id == operation.operation_id;
            const bool client_match = !operation.client_operation_id.empty() &&
                                       entry.client_operation_id == operation.client_operation_id;
            if ((operation_match || client_match) &&
                entry.account_id == operation.account_id &&
                entry.trade_id == operation.trade_id) {
                result.push_back(&entry);
            }
        }
        return result;
    }

    std::shared_ptr<State> m_state;
    bool m_broker_idempotency;
    bool m_history_available = true;
    std::map<std::string, SimulatedSubmissionPlan> m_plans;
    std::map<std::string, bool> m_not_executed;
};

/// \brief Recovery view reconstructed only from immutable operation journal events.
class ExecutionRecoveryManager {
public:
    explicit ExecutionRecoveryManager(FakeExecutionClock& clock) : m_clock(clock) {}

    static std::string operation_key(const ExecutionOperationRecord& operation) {
        return "exec-op:" + operation.operation_id + ":" +
               std::to_string(operation.state_revision);
    }

    static bool unresolved(ExecutionOperationState state) noexcept {
        return state == ExecutionOperationState::SUBMISSION_STARTED ||
               state == ExecutionOperationState::BROKER_ACCEPTED ||
               state == ExecutionOperationState::BROKER_ID_PERSISTED ||
               state == ExecutionOperationState::EXECUTION_UNKNOWN ||
               state == ExecutionOperationState::RECONCILIATION_REQUIRED;
    }

    static RecoveryResult recover(const TradingSystemStorage& storage) {
        RecoveryResult result;
        result.kind = RecoveryResultKind::READY;
        const auto events = storage.journal_events("EXECUTION_OPERATION");
        if (!events.succeeded()) {
            result.kind = events.kind == StorageResultKind::UNSUPPORTED_SCHEMA
                ? RecoveryResultKind::UNSUPPORTED_SCHEMA
                : RecoveryResultKind::STORAGE_ERROR;
            result.diagnostic = events.message.empty()
                ? "execution journal cannot be read" : events.message;
            return result;
        }

        std::map<std::string, std::vector<std::pair<std::uint64_t,
                                                     ExecutionOperationRecord>>> chains;
        for (const auto& event : events.values) {
            try {
                const auto operation = event.payload.get<ExecutionOperationRecord>();
                if (!operation.valid() || event.aggregate_id !=
                    "operation:" + operation.operation_id ||
                    event.state_revision != operation.state_revision ||
                    event.idempotency_key != operation_key(operation)) {
                    return corrupted(result, "execution journal event identity is invalid", event);
                }
                chains[operation.operation_id].emplace_back(event.event_id, operation);
            } catch (const std::exception& error) {
                return corrupted(result,
                                  std::string("execution journal payload is malformed: ") + error.what(),
                                  event);
            }
        }

        for (auto& chain : chains) {
            auto& entries = chain.second;
            std::sort(entries.begin(), entries.end(),
                      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
            std::uint64_t expected_revision = 0;
            std::optional<ExecutionOperationRecord> previous;
            for (const auto& entry : entries) {
                const auto& operation = entry.second;
                if (operation.state_revision != expected_revision) {
                    return corrupted(result, "execution journal revision chain has a gap", {});
                }
                if (previous) {
                    if (!same_identity(*previous, operation) ||
                        !transition_allowed(previous->state, operation.state) ||
                        (!previous->broker_trade_id.empty() &&
                         previous->broker_trade_id != operation.broker_trade_id)) {
                        return corrupted(result,
                                          "execution journal identity or transition conflicts", {});
                    }
                } else if (operation.state != ExecutionOperationState::PREPARED ||
                           !operation.broker_trade_id.empty()) {
                    return corrupted(result, "execution journal must begin with PREPARED", {});
                }
                result.operations[operation.operation_id] = operation;
                previous = operation;
                if (expected_revision == (std::numeric_limits<std::uint64_t>::max)()) {
                    return corrupted(result, "execution journal revision overflow", {});
                }
                ++expected_revision;
            }
        }
        return result;
    }

    /// \brief Reconstructs readiness from durable facts only.
    ///
    /// This does not claim that an in-memory RiskAdmission owner was hydrated;
    /// new admission must supply and verify those owners separately.
    RecoveryReadiness durable_readiness(const TradingSystemStorage& storage) const {
        const auto recovered = recover(storage);
        RecoveryReadiness result;
        result.recovery_kind = recovered.kind;
        result.journal_valid = recovered.succeeded();
        result.global_block = !recovered.succeeded();
        result.blocked_risk_scopes = recovered.affected_risk_scopes;
        result.diagnostic = recovered.diagnostic;
        if (!recovered.succeeded()) return result;

        for (const auto& item : recovered.operations) {
            if (unresolved(item.second.state)) {
                result.blocked_risk_scopes.insert(item.second.risk_scope_id);
            }
        }
        const auto reservations = storage.reservations();
        if (!reservations.succeeded()) {
            result.global_block = true;
            result.diagnostic = reservations.message;
            return result;
        }
        for (const auto& reservation : reservations.values) {
            if (reservation.state == RiskReservationState::RESERVED ||
                reservation.state == RiskReservationState::SUBMITTED ||
                reservation.state == RiskReservationState::EXECUTION_UNKNOWN) {
                result.blocked_risk_scopes.insert(reservation.risk_scope_id);
            }
        }
        const auto gates = storage.risk_scope_gates();
        if (!gates.succeeded()) {
            result.global_block = true;
            result.diagnostic = gates.message;
            return result;
        }
        for (const auto& gate : gates.values) {
            result.blocked_risk_scopes.insert(gate.risk_scope_id);
            result.blocked_risk_scopes.insert(gate.related_scope_id);
        }
        const auto completions = storage.journal_events("GROUP_COMPLETION");
        if (!completions.succeeded()) {
            result.global_block = true;
            result.diagnostic = completions.message;
            return result;
        }
        std::set<ExecutionGroupId> completed_groups;
        for (const auto& event : completions.values) {
            for (const auto& item : recovered.operations) {
                if (event.aggregate_id == "group:" + std::to_string(item.second.group_id)) {
                    completed_groups.insert(item.second.group_id);
                }
            }
        }
        for (const auto& item : recovered.operations) {
            if (!completed_groups.count(item.second.group_id)) {
                result.blocked_risk_scopes.insert(item.second.risk_scope_id);
            }
        }
        return result;
    }

    RecoveryReadiness admission_readiness(const TradingSystemStorage& storage,
                                          bool owners_hydrated = false) const {
        auto result = durable_readiness(storage);
        if (result.journal_valid && !owners_hydrated) {
            result.global_block = true;
            result.diagnostic = "series, risk and money-management owners are not hydrated";
        }
        return result;
    }

    /// \brief Returns the durable risk scopes currently blocked by recovery facts.
    std::set<std::uint64_t> blocked_risk_scopes(
        const TradingSystemStorage& storage) const {
        return durable_readiness(storage).blocked_risk_scopes;
    }

private:
    static bool same_money(const MoneyAmount& lhs, const MoneyAmount& rhs) noexcept {
        return lhs.units == rhs.units && lhs.precision == rhs.precision &&
               lhs.currency == rhs.currency;
    }

    static bool same_group(const ExecutionGroupIdentity& lhs,
                           const ExecutionGroupIdentity& rhs) noexcept {
        return lhs.group_id == rhs.group_id &&
               lhs.expected_members == rhs.expected_members;
    }

    static StorageResult validate_preparation_bindings(
        const TradeIntent& intent,
        const ExecutionRosterRecord& roster,
        const std::vector<ReservationRecord>& reservations,
        const std::vector<PhysicalTradeRecord>& physical_trades) {
        if (!same_group(intent.execution_group,
                        {roster.group_id, roster.expected_members}) ||
            intent.intent_id != roster.intent_id) {
            return {StorageResultKind::CONFLICT,
                    "intent identity conflicts with execution roster"};
        }

        for (const auto& member : roster.members) {
            const auto physical_count = std::count_if(
                physical_trades.begin(), physical_trades.end(),
                [&member](const PhysicalTradeRecord& value) {
                    return value.trade_id == member.trade_id;
                });
            if (physical_count != 1) {
                return {StorageResultKind::CONFLICT,
                        "physical trade roster identity is missing or duplicated"};
            }
            const auto physical = std::find_if(
                physical_trades.begin(), physical_trades.end(),
                [&member](const PhysicalTradeRecord& value) {
                    return value.trade_id == member.trade_id;
                });
            if (physical->intent_id != roster.intent_id ||
                physical->group_id != roster.group_id ||
                physical->account_id != member.account_id ||
                physical->risk_scope_id != member.risk_scope_id) {
                return {StorageResultKind::CONFLICT,
                        "physical trade does not belong to its roster member"};
            }

            const auto binding = std::find_if(
                roster.reservation_bindings.begin(),
                roster.reservation_bindings.end(),
                [&member](const RosterReservationBinding& value) {
                    return value.trade_id == member.trade_id;
                });
            if (binding == roster.reservation_bindings.end()) {
                return {StorageResultKind::CONFLICT,
                        "roster reservation binding is missing"};
            }
            const auto reservation_count = std::count_if(
                reservations.begin(), reservations.end(),
                [binding](const ReservationRecord& value) {
                    return value.reservation_id == binding->reservation_id;
                });
            if (reservation_count != 1) {
                return {StorageResultKind::CONFLICT,
                        "reservation roster identity is missing or duplicated"};
            }
            const auto reservation = std::find_if(
                reservations.begin(), reservations.end(),
                [binding](const ReservationRecord& value) {
                    return value.reservation_id == binding->reservation_id;
                });
            if (reservation->state != RiskReservationState::RESERVED ||
                reservation->intent_id != roster.intent_id ||
                !same_group(reservation->execution_group,
                            {roster.group_id, roster.expected_members}) ||
                reservation->cardinality != intent.cardinality ||
                reservation->risk_scope_id != member.risk_scope_id ||
                !same_money(reservation->stake, binding->stake) ||
                !same_money(reservation->potential_risk, binding->potential_risk) ||
                reservation->idempotency_key != binding->idempotency_key) {
                return {StorageResultKind::CONFLICT,
                        "reservation does not belong to its roster member"};
            }
        }
        return {StorageResultKind::APPLIED, {}};
    }

    static StorageResult validate_owner_admissions(
        const TradeIntent& intent,
        const std::vector<ReservationRecord>& reservations,
        const std::vector<const RiskAdmission*>& admission_owners) {
        if (reservations.size() != admission_owners.size()) {
            return {StorageResultKind::INVALID_ARGUMENT,
                    "one authoritative RiskAdmission owner is required per reservation"};
        }
        struct Aggregate {
            MoneyAmount stake;
            MoneyAmount potential_risk;
        };
        using OwnerKey = std::pair<RiskScopeKind, std::uint64_t>;
        struct OwnerKeyLess {
            bool operator()(const OwnerKey& lhs, const OwnerKey& rhs) const noexcept {
                return std::tie(lhs.first, lhs.second) < std::tie(rhs.first, rhs.second);
            }
        };
        std::map<OwnerKey, const RiskAdmission*, OwnerKeyLess> owners_by_scope;
        std::map<const RiskAdmission*, Aggregate> aggregates;
        for (std::size_t index = 0; index < reservations.size(); ++index) {
            const auto* owner = admission_owners[index];
            if (!owner) {
                return {StorageResultKind::INVALID_ARGUMENT,
                        "RiskAdmission owner is missing"};
            }
            const auto& reservation = reservations[index];
            const auto owner_scope = owner->scope();
            if (owner_scope.kind != reservation.risk_scope_kind ||
                owner_scope.scope_id != reservation.risk_scope_id) {
                return {StorageResultKind::CONFLICT,
                        "RiskAdmission owner does not own reservation risk scope"};
            }
            const OwnerKey owner_key{owner_scope.kind, owner_scope.scope_id};
            const auto owner_for_scope = owners_by_scope.find(owner_key);
            if (owner_for_scope == owners_by_scope.end()) {
                owners_by_scope.emplace(owner_key, owner);
            } else if (owner_for_scope->second != owner) {
                return {StorageResultKind::CONFLICT,
                        "multiple RiskAdmission owners claim one risk scope"};
            }
            auto found = aggregates.find(owner);
            if (found == aggregates.end()) {
                found = aggregates.emplace(owner,
                    Aggregate{reservation.stake, reservation.potential_risk}).first;
            } else {
                auto& aggregate = found->second;
                if (aggregate.stake.currency != reservation.stake.currency ||
                    aggregate.stake.precision != reservation.stake.precision ||
                    aggregate.potential_risk.currency != reservation.potential_risk.currency ||
                    aggregate.potential_risk.precision != reservation.potential_risk.precision ||
                    aggregate.stake.units >
                        (std::numeric_limits<std::int64_t>::max)() - reservation.stake.units ||
                    aggregate.potential_risk.units >
                        (std::numeric_limits<std::int64_t>::max)() -
                            reservation.potential_risk.units) {
                    return {StorageResultKind::CONFLICT,
                            "RiskAdmission owner reservations have incompatible money metadata"};
                }
                aggregate.stake.units += reservation.stake.units;
                aggregate.potential_risk.units += reservation.potential_risk.units;
            }
        }
        for (const auto& entry : aggregates) {
            RiskRequest request;
            request.intent_id = intent.intent_id;
            request.execution_group = intent.execution_group;
            request.cardinality = intent.cardinality;
            request.intent_status = intent.status;
            request.idempotency_key = intent.idempotency_key;
            request.stake = entry.second.stake;
            request.potential_risk = entry.second.potential_risk;
            const auto verified = entry.first->verify_group_admission(request);
            if (!verified.succeeded() || !verified.is_submission_admission()) {
                return {StorageResultKind::INVALID_STATE,
                        "reservation was not admitted by its RiskAdmission owner"};
            }
        }
        return {StorageResultKind::APPLIED, {}};
    }

    StorageResult validate_new_admission(
        const TradingSystemStorage& storage,
        const TradeIntent& intent,
        const ExecutionRosterRecord& roster,
        const std::vector<ReservationRecord>& reservations,
        const std::vector<const RiskAdmission*>& admission_owners) const {
        if (auto result = validate_owner_admissions(
                intent, reservations, admission_owners);
            result.kind != StorageResultKind::APPLIED) {
            return result;
        }
        const auto existing = storage.roster(roster.group_id);
        if (existing.kind != StorageResultKind::NOT_FOUND && !existing.found()) {
            return {existing.kind, existing.message};
        }
        const bool is_existing_group = existing.found() &&
            detail::roster_to_json(*existing.value) == detail::roster_to_json(roster);
        if (is_existing_group) return {StorageResultKind::APPLIED, {}};

        const auto readiness = durable_readiness(storage);
        if (!readiness.journal_valid || readiness.global_block) {
            return {StorageResultKind::INVALID_STATE,
                    readiness.diagnostic.empty()
                        ? "durable recovery is not ready for admission"
                        : readiness.diagnostic};
        }
        for (const auto& member : roster.members) {
            if (readiness.blocked_risk_scopes.count(member.risk_scope_id) != 0) {
                return {StorageResultKind::INVALID_STATE,
                        "risk scope is blocked by unresolved durable execution"};
            }
        }
        return {StorageResultKind::APPLIED, {}};
    }

    StorageResult validate_prepared_admission(
        const TradingSystemStorage& storage,
        const ExecutionOperationRecord& operation) const {
        const auto recovered = recover(storage);
        if (!recovered.succeeded()) {
            return {StorageResultKind::INVALID_STATE,
                    "execution journal recovery failed: " + recovered.diagnostic};
        }
        const auto found = recovered.operations.find(operation.operation_id);
        if (found == recovered.operations.end() ||
            found->second.state != ExecutionOperationState::PREPARED ||
            !same_identity(found->second, operation)) {
            return {StorageResultKind::CONFLICT,
                    "execution operation has no verified durable admission"};
        }
        const auto roster = storage.roster(operation.group_id);
        const auto physical = storage.physical_trade(operation.trade_id);
        if (!roster.found()) return {roster.kind, roster.message};
        if (!physical.found()) return {physical.kind, physical.message};
        if (!roster.value->valid() ||
            roster.value->intent_id != operation.intent_id) {
            return {StorageResultKind::CONFLICT,
                    "prepared operation roster identity is invalid"};
        }
        const auto member = std::find_if(
            roster.value->members.begin(), roster.value->members.end(),
            [&operation](const RosterMemberRecord& value) {
                return value.trade_id == operation.trade_id;
            });
        const auto binding = std::find_if(
            roster.value->reservation_bindings.begin(),
            roster.value->reservation_bindings.end(),
            [&operation](const RosterReservationBinding& value) {
                return value.trade_id == operation.trade_id;
            });
        if (member == roster.value->members.end() ||
            binding == roster.value->reservation_bindings.end() ||
            physical.value->intent_id != roster.value->intent_id ||
            physical.value->group_id != roster.value->group_id ||
            physical.value->account_id != operation.account_id ||
            physical.value->risk_scope_id != operation.risk_scope_id ||
            member->account_id != operation.account_id ||
            member->risk_scope_id != operation.risk_scope_id) {
            return {StorageResultKind::CONFLICT,
                    "prepared operation is not bound to its physical roster member"};
        }
        const auto reservation = storage.reservation(binding->reservation_id);
        if (!reservation.found()) return {reservation.kind, reservation.message};
        if (reservation.value->state != RiskReservationState::RESERVED ||
            reservation.value->intent_id != operation.intent_id ||
            reservation.value->risk_scope_id != operation.risk_scope_id ||
            reservation.value->execution_group.group_id != operation.group_id ||
            reservation.value->execution_group.expected_members != roster.value->expected_members) {
            return {StorageResultKind::INVALID_STATE,
                    "prepared operation reservation is not an admitted reservation"};
        }
        return {StorageResultKind::APPLIED, {}};
    }

    StorageResult persist_state(TradingSystemStorage& storage,
                                ExecutionOperationRecord operation) const {
        if (!operation.valid()) return {StorageResultKind::INVALID_ARGUMENT, "invalid execution operation"};
        auto unit = storage.begin();
        const auto event_id = unit.next_journal_event_id_for_append();
        if (event_id.kind != StorageResultKind::APPLIED) return {event_id.kind, event_id.message};
        JournalEvent event;
        event.event_id = event_id.value;
        event.event_type = "EXECUTION_OPERATION";
        event.aggregate_id = "operation:" + operation.operation_id;
        event.state_revision = operation.state_revision;
        event.idempotency_key = operation_key(operation);
        event.occurred_at_ms = operation.occurred_at_ms;
        event.payload = operation;
        const auto appended = unit.append_journal(event);
        if (appended.kind != StorageResultKind::APPLIED) return appended;
        return unit.commit();
    }

    StorageResult reject_without_submission(
        TradingSystemStorage& storage,
        const ExecutionOperationRecord& current) const {
        if (current.state != ExecutionOperationState::SUBMISSION_STARTED &&
            current.state != ExecutionOperationState::EXECUTION_UNKNOWN &&
            current.state != ExecutionOperationState::RECONCILIATION_REQUIRED) {
            return {StorageResultKind::INVALID_STATE,
                    "operation is not eligible for proven no-submission rejection"};
        }
        const auto roster = storage.roster(current.group_id);
        if (!roster.found()) return {roster.kind, roster.message};
        const auto binding = std::find_if(
            roster.value->reservation_bindings.begin(),
            roster.value->reservation_bindings.end(),
            [&current](const RosterReservationBinding& value) {
                return value.trade_id == current.trade_id;
            });
        if (binding == roster.value->reservation_bindings.end()) {
            return {StorageResultKind::NOT_FOUND, "operation reservation binding is missing"};
        }
        const auto reservation = storage.reservation(binding->reservation_id);
        const auto physical = storage.physical_trade(current.trade_id);
        if (!reservation.found()) return {reservation.kind, reservation.message};
        if (!physical.found()) return {physical.kind, physical.message};
        std::uint64_t operation_revision = 0;
        std::uint64_t physical_revision = 0;
        std::uint64_t reservation_revision = 0;
        if (!next_revision_of(current.state_revision, operation_revision) ||
            !next_revision_of(physical.value->state_revision, physical_revision) ||
            !next_revision_of(reservation.value->state_revision, reservation_revision)) {
            return {StorageResultKind::INVALID_ARGUMENT, "rejection revision overflow"};
        }

        auto released = *reservation.value;
        released.state = RiskReservationState::RELEASED;
        released.state_revision = reservation_revision;
        auto rejected_trade = *physical.value;
        rejected_trade.lifecycle_state = "REJECTED";
        rejected_trade.state_revision = physical_revision;
        auto rejected = current;
        rejected.state = ExecutionOperationState::REJECTED;
        rejected.state_revision = operation_revision;
        rejected.occurred_at_ms = m_clock.now_ms();

        auto unit = storage.begin();
        const auto operation_event_id = unit.next_journal_event_id_for_append();
        if (operation_event_id.kind != StorageResultKind::APPLIED) {
            return {operation_event_id.kind, operation_event_id.message};
        }
        const JournalEvent operation_event{
            operation_event_id.value, "EXECUTION_OPERATION",
            "operation:" + rejected.operation_id, rejected.state_revision,
            trading_storage_schema_version, operation_key(rejected),
            rejected.occurred_at_ms, rejected};
        if (auto result = unit.append_journal(operation_event);
            result.kind != StorageResultKind::APPLIED) return result;

        const auto settlement_id = unit.next_id("settlement");
        if (settlement_id.kind != StorageResultKind::APPLIED) {
            return {settlement_id.kind, settlement_id.message};
        }
        const auto settlement_event_id = unit.next_journal_event_id_for_append();
        if (settlement_event_id.kind != StorageResultKind::APPLIED) {
            return {settlement_event_id.kind, settlement_event_id.message};
        }
        TradeSettlement evidence;
        evidence.settlement_id = settlement_id.value;
        evidence.intent_id = current.intent_id;
        evidence.execution_group_id = current.group_id;
        evidence.trade_id = current.trade_id;
        evidence.kind = OutcomeKind::REJECTED;
        evidence.evidence = ExecutionEvidence::NOT_SUBMITTED;
        auto native_profit = reservation.value->stake;
        native_profit.units = 0;
        SettlementTransition transition;
        transition.journal = {settlement_event_id.value, "EXECUTION_SETTLED",
                              "trade:" + std::to_string(current.trade_id), 0,
                              trading_storage_schema_version,
                              "settlement:" + std::to_string(evidence.settlement_id),
                              m_clock.now_ms(), {}};
        transition.settlement = {evidence, native_profit, std::nullopt, true,
                                 trading_storage_schema_version};
        transition.physical_trade = rejected_trade;
        transition.expected_physical_trade_revision = physical.value->state_revision;
        if (auto result = unit.record_physical_settlement(transition);
            result.kind != StorageResultKind::APPLIED) return result;
        if (auto result = unit.put_reservation(
                released, reservation.value->state_revision);
            result.kind != StorageResultKind::APPLIED) return result;
        return unit.commit();
    }

public:
    /// \brief Rejects the legacy unbound preparation form.
    ///
    /// A caller-constructed ADMITTED intent and ReservationRecord are not
    /// evidence that RiskAdmission::reserve() succeeded.
    StorageResult prepare(TradingSystemStorage&,
                          const TradeIntent&,
                          const ExecutionRosterRecord&,
                          const std::vector<ReservationRecord>&,
                          const std::vector<PhysicalTradeRecord>&) const {
        return {StorageResultKind::INVALID_STATE,
                "prepare requires authoritative RiskAdmission owners"};
    }

    StorageResult prepare(TradingSystemStorage& storage,
                          const TradeIntent& intent,
                          const ExecutionRosterRecord& roster,
                          const std::vector<ReservationRecord>& reservations,
                          const std::vector<PhysicalTradeRecord>& physical_trades,
                          const std::vector<const RiskAdmission*>& admission_owners) const {
        if (!intent.valid() || intent.status != IntentStatus::ADMITTED || !roster.valid() ||
            reservations.size() != roster.members.size() ||
            physical_trades.size() != roster.members.size()) {
            return {StorageResultKind::INVALID_ARGUMENT, "invalid durable execution preparation"};
        }
        if (auto result = validate_preparation_bindings(
                intent, roster, reservations, physical_trades);
            result.kind != StorageResultKind::APPLIED) {
            return result;
        }
        if (auto result = validate_new_admission(
                storage, intent, roster, reservations, admission_owners);
            result.kind != StorageResultKind::APPLIED) {
            return result;
        }
        std::set<std::string> existing_operation_keys;
        for (const auto& physical : physical_trades) {
            const ExecutionOperationRecord operation{
                "group-" + std::to_string(roster.group_id) + ":trade-" +
                    std::to_string(physical.trade_id),
                intent.idempotency_key + ":trade-" + std::to_string(physical.trade_id),
                intent.intent_id, roster.group_id, physical.trade_id,
                physical.account_id, physical.risk_scope_id,
                ExecutionOperationState::PREPARED, 0, {}, m_clock.now_ms()};
            const auto existing = storage.journal_by_key(operation_key(operation));
            if (existing.kind != StorageResultKind::NOT_FOUND && !existing.found()) {
                return {existing.kind, existing.message};
            }
            if (existing.found()) {
                ExecutionOperationRecord persisted;
                try {
                    persisted = existing.value->payload.get<ExecutionOperationRecord>();
                } catch (const std::exception&) {
                    return {StorageResultKind::CONFLICT,
                            "execution preparation journal payload is malformed"};
                }
                if (existing.value->event_type != "EXECUTION_OPERATION" ||
                    existing.value->aggregate_id != "operation:" + operation.operation_id ||
                    existing.value->state_revision != operation.state_revision ||
                    existing.value->idempotency_key != operation_key(operation) ||
                    existing.value->occurred_at_ms != persisted.occurred_at_ms ||
                    !same_preparation_payload(persisted, operation)) {
                    return {StorageResultKind::CONFLICT,
                            "execution preparation journal identity conflicts"};
                }
                existing_operation_keys.insert(operation_key(operation));
            }
        }
        auto unit = storage.begin();
        std::set<std::uint64_t> admission_scopes;
        for (const auto& reservation : reservations) {
            admission_scopes.insert(reservation.risk_scope_id);
        }
        if (auto result = unit.validate_admission_scopes(
                admission_scopes, roster.group_id);
            result.kind != StorageResultKind::APPLIED) {
            return result;
        }
        if (auto result = unit.put_intent(intent); result.kind != StorageResultKind::APPLIED &&
            result.kind != StorageResultKind::DUPLICATE) return result;
        if (auto result = unit.put_roster(roster); result.kind != StorageResultKind::APPLIED &&
            result.kind != StorageResultKind::DUPLICATE) return result;
        for (std::size_t i = 0; i < reservations.size(); ++i) {
            if (reservations[i].state_revision != 0 || physical_trades[i].state_revision != 0) {
                return {StorageResultKind::INVALID_ARGUMENT,
                        "durable execution preparation requires revision-zero records"};
            }
            if (auto result = unit.put_reservation(reservations[i], 0);
                result.kind != StorageResultKind::APPLIED && result.kind != StorageResultKind::DUPLICATE) {
                return result;
            }
            if (auto result = unit.put_physical_trade(physical_trades[i], 0);
                result.kind != StorageResultKind::APPLIED && result.kind != StorageResultKind::DUPLICATE) {
                return result;
            }
            ExecutionOperationRecord operation{
                "group-" + std::to_string(roster.group_id) + ":trade-" +
                    std::to_string(physical_trades[i].trade_id),
                intent.idempotency_key + ":trade-" + std::to_string(physical_trades[i].trade_id),
                intent.intent_id, roster.group_id, physical_trades[i].trade_id,
                physical_trades[i].account_id, physical_trades[i].risk_scope_id,
                ExecutionOperationState::PREPARED, 0, {}, m_clock.now_ms()};
            if (existing_operation_keys.count(operation_key(operation)) != 0) continue;
            const auto event_id = unit.next_journal_event_id_for_append();
            if (event_id.kind != StorageResultKind::APPLIED) return {event_id.kind, event_id.message};
            JournalEvent event{event_id.value, "EXECUTION_OPERATION",
                               "operation:" + operation.operation_id, 0,
                               trading_storage_schema_version, operation_key(operation),
                               operation.occurred_at_ms, operation};
            if (auto result = unit.append_journal(event); result.kind != StorageResultKind::APPLIED &&
                result.kind != StorageResultKind::DUPLICATE) return result;
        }
        return unit.commit();
    }

private:
    StorageResult mark(TradingSystemStorage& storage, ExecutionOperationRecord operation) const {
        const auto all = recover(storage);
        if (!all.succeeded()) {
            return {StorageResultKind::INVALID_STATE,
                    "execution journal recovery failed: " + all.diagnostic};
        }
        const auto found = all.operations.find(operation.operation_id);
        if (found != all.operations.end()) {
            if (found->second.state_revision == (std::numeric_limits<std::uint64_t>::max)() ||
                operation.state_revision != found->second.state_revision + 1) {
                return {StorageResultKind::REVISION_CONFLICT, "execution operation revision conflict"};
            }
            if (!same_identity(operation, found->second) ||
                (!found->second.broker_trade_id.empty() &&
                 operation.broker_trade_id != found->second.broker_trade_id)) {
                return {StorageResultKind::CONFLICT, "execution operation identity changed"};
            }
            if (!transition_allowed(found->second.state, operation.state)) {
                return {StorageResultKind::INVALID_STATE,
                        "invalid execution operation state transition"};
            }
        } else {
            return {StorageResultKind::NOT_FOUND, "execution operation is not prepared"};
        }
        operation.occurred_at_ms = m_clock.now_ms();
        return persist_state(storage, std::move(operation));
    }

public:
    StorageResult mark_submission_started(TradingSystemStorage& storage,
                                          const ExecutionOperationRecord& current) const {
        std::uint64_t next_revision = 0;
        if (!next_revision_of(current.state_revision, next_revision)) {
            return {StorageResultKind::INVALID_ARGUMENT, "execution operation revision overflow"};
        }
        auto next = current;
        next.state = ExecutionOperationState::SUBMISSION_STARTED;
        next.state_revision = next_revision;
        return mark(storage, std::move(next));
    }

    StorageResult mark_unknown(TradingSystemStorage& storage,
                               const ExecutionOperationRecord& current) const {
        std::uint64_t next_revision = 0;
        if (!next_revision_of(current.state_revision, next_revision)) {
            return {StorageResultKind::INVALID_ARGUMENT, "execution operation revision overflow"};
        }
        auto next = current;
        next.state = ExecutionOperationState::EXECUTION_UNKNOWN;
        next.state_revision = next_revision;
        return mark(storage, std::move(next));
    }

    /// \brief Submits only a prepared operation and records every durable boundary.
    ///
    /// An operation in any unresolved state is deliberately rejected. A timeout
    /// is evidence of possible external side effect, never permission to resend.
    StorageResult submit(TradingSystemStorage& storage,
                         const ExecutionOperationRecord& current,
                         SimulatedExecutionAdapter& adapter) const {
        if (current.state != ExecutionOperationState::PREPARED) {
            return {StorageResultKind::INVALID_STATE,
                    "only a prepared operation may be submitted"};
        }
        // A durable PREPARED operation plus its immutable roster/reservation
        // is the admission permit. This deliberately permits already-admitted
        // members of one group to continue while a different group is blocked.
        if (auto admission = validate_prepared_admission(storage, current);
            admission.kind != StorageResultKind::APPLIED) {
            return admission;
        }
        std::uint64_t started_revision = 0;
        if (!next_revision_of(current.state_revision, started_revision)) {
            return {StorageResultKind::INVALID_ARGUMENT, "execution operation revision overflow"};
        }
        auto started = current;
        started.state = ExecutionOperationState::SUBMISSION_STARTED;
        started.state_revision = started_revision;
        auto result = mark(storage, started);
        if (result.kind != StorageResultKind::APPLIED) return result;

        const auto response = adapter.submit({current.operation_id,
                                               current.client_operation_id,
                                               current.account_id,
                                               current.trade_id});
        auto observed = started;
        if (!next_revision_of(started.state_revision, observed.state_revision)) {
            return {StorageResultKind::INVALID_ARGUMENT, "execution operation revision overflow"};
        }
        if (!response.accepted) {
            if (!response.side_effect_occurred) {
                return reject_without_submission(storage, started);
            }
            observed.state = ExecutionOperationState::EXECUTION_UNKNOWN;
        } else if (response.response_lost) {
            observed.state = ExecutionOperationState::EXECUTION_UNKNOWN;
        } else {
            observed.state = response.broker_id_available
                ? ExecutionOperationState::BROKER_ID_PERSISTED
                : ExecutionOperationState::BROKER_ACCEPTED;
            observed.broker_trade_id = response.broker_trade_id;
        }
        return mark(storage, std::move(observed));
    }

    ReconciliationResult reconcile(TradingSystemStorage& storage,
                                   const ExecutionOperationRecord& operation,
                                   const SimulatedExecutionAdapter& adapter) const {
        const auto operations = recover(storage);
        if (!operations.succeeded()) {
            return {ReconciliationKind::EXECUTION_UNKNOWN, {}, std::nullopt,
                    "execution journal recovery failed: " + operations.diagnostic};
        }
        const auto found = operations.operations.find(operation.operation_id);
        if (found == operations.operations.end()) {
            return {ReconciliationKind::EXECUTION_UNKNOWN, {}, std::nullopt,
                    "execution operation is missing"};
        }
        const auto result = adapter.reconcile(found->second);
        auto next = found->second;
        if (result.kind == ReconciliationKind::MATCH_CONFIRMED) {
            next.state = result.broker_trade_id.empty()
                ? ExecutionOperationState::BROKER_ACCEPTED
                : ExecutionOperationState::BROKER_ID_PERSISTED;
            next.broker_trade_id = result.broker_trade_id;
        } else if (result.kind == ReconciliationKind::NOT_EXECUTED_PROVEN) {
            const auto rejected = reject_without_submission(storage, found->second);
            if (!rejected.succeeded()) {
                return {ReconciliationKind::EXECUTION_UNKNOWN, {}, std::nullopt,
                        rejected.message};
            }
            return result;
        } else {
            next.state = ExecutionOperationState::RECONCILIATION_REQUIRED;
        }
        if (next.state == found->second.state &&
            (next.broker_trade_id.empty() ||
             next.broker_trade_id == found->second.broker_trade_id)) {
            return result;
        }
        if (!next_revision_of(found->second.state_revision, next.state_revision)) {
            return {ReconciliationKind::EXECUTION_UNKNOWN, {}, std::nullopt,
                    "execution operation revision overflow"};
        }
        const auto persisted = mark(storage, next);
        if (persisted.kind != StorageResultKind::APPLIED) {
            return {ReconciliationKind::EXECUTION_UNKNOWN, {}, std::nullopt, persisted.message};
        }
        return result;
    }

    /// \brief Commits a confirmed terminal settlement with the operation fact.
    ///
    /// When a logical transition is supplied, the existing Batch 5B atomic
    /// settlement path is used in the same Unit of Work. Without it, only the
    /// physical fact is committed; logical series/MM projections remain owned by
    /// the caller and cannot be guessed by recovery.
    StorageResult settle_reconciled(
        TradingSystemStorage& storage,
        const ExecutionOperationRecord& operation,
        const TradeSettlement& settlement,
        std::optional<SettlementTransition> logical_transition = std::nullopt) const {
        const auto current_operations = recover(storage);
        if (!current_operations.succeeded()) {
            return {StorageResultKind::INVALID_STATE,
                    "execution journal recovery failed: " + current_operations.diagnostic};
        }
        const auto current = current_operations.operations.find(operation.operation_id);
        if (current == current_operations.operations.end()) {
            return {StorageResultKind::NOT_FOUND, "execution operation is missing"};
        }
        if (operation.client_operation_id != current->second.client_operation_id ||
            operation.intent_id != current->second.intent_id ||
            operation.group_id != current->second.group_id ||
            operation.trade_id != current->second.trade_id ||
            operation.account_id != current->second.account_id ||
            operation.risk_scope_id != current->second.risk_scope_id) {
            return {StorageResultKind::CONFLICT,
                    "stale execution operation identity"};
        }
        if (!settlement.valid() || !settlement.terminal() || !settlement.net_profit ||
            settlement.intent_id != operation.intent_id ||
            settlement.execution_group_id != operation.group_id ||
            settlement.trade_id != operation.trade_id ||
            settlement.evidence != ExecutionEvidence::EXECUTED) {
            return {StorageResultKind::INVALID_ARGUMENT, "settlement does not match execution operation"};
        }
        if (current->second.state == ExecutionOperationState::SETTLED) {
            const auto persisted = storage.settlements_for_group(current->second.group_id);
            if (!persisted.succeeded()) return {persisted.kind, persisted.message};
            std::vector<TradeSettlement> evidence;
            for (const auto& value : persisted.values) {
                if (value.settlement.trade_id == settlement.trade_id) {
                    evidence.push_back(value.settlement);
                }
            }
            std::stable_sort(evidence.begin(), evidence.end(), canonical_settlement_before);
            std::optional<TradeSettlement> canonical_terminal;
            for (const auto& value : evidence) {
                if (!value.terminal()) continue;
                if (!canonical_terminal) {
                    canonical_terminal = value;
                } else if (!same_terminal_evidence(*canonical_terminal, value)) {
                    return {StorageResultKind::CONFLICT,
                            "conflicting terminal settlement evidence"};
                }
            }
            if (!canonical_terminal) {
                return {StorageResultKind::INVALID_STATE,
                        "settled operation has no canonical terminal evidence"};
            }
            if (same_terminal_evidence(*canonical_terminal, settlement)) {
                if (logical_transition) {
                    if (!same_terminal_evidence(
                            logical_transition->settlement.settlement,
                            *canonical_terminal)) {
                        return {StorageResultKind::CONFLICT,
                                "logical transition does not match terminal evidence"};
                    }
                    const auto physical = storage.physical_trade(operation.trade_id);
                    if (!physical.found()) return {physical.kind, physical.message};
                    auto normalized = *logical_transition;
                    // The physical fact is canonical by durable SettlementId;
                    // a late callback may carry an equivalent broker event ID.
                    normalized.settlement.settlement = *canonical_terminal;
                    normalized.journal.idempotency_key =
                        "settlement:" + std::to_string(canonical_terminal->settlement_id);
                    if (normalized.money_management) {
                        normalized.money_management->last_settlement_id =
                            canonical_terminal->settlement_id;
                    }
                    if (normalized.completion_journal) {
                        normalized.completion_journal->idempotency_key =
                            normalized.journal.idempotency_key + ":logical";
                    }
                    normalized.physical_trade = *physical.value;
                    normalized.expected_physical_trade_revision = physical.value->state_revision;
                    auto unit = storage.begin();
                    const auto completed = unit.apply_group_completion_atomic(normalized);
                    if (!completed.succeeded()) return completed;
                    const auto committed = unit.commit();
                    return committed.succeeded() ? completed : committed;
                }
                return {StorageResultKind::DUPLICATE, {}};
            }
            return {StorageResultKind::CONFLICT,
                    "terminal settlement conflicts with persisted evidence"};
        }
        if (current->second.state != ExecutionOperationState::BROKER_ACCEPTED &&
            current->second.state != ExecutionOperationState::BROKER_ID_PERSISTED &&
            current->second.state != ExecutionOperationState::EXECUTED) {
            return {StorageResultKind::INVALID_STATE,
                    "only an identified broker execution may be settled"};
        }
        const auto physical = storage.physical_trade(operation.trade_id);
        if (!physical.found()) return {physical.kind, physical.message};
        auto updated_physical = *physical.value;
        updated_physical.lifecycle_state = "SETTLED";
        updated_physical.broker_trade_id = current->second.broker_trade_id;
        if (!next_revision_of(physical.value->state_revision, updated_physical.state_revision)) {
            return {StorageResultKind::INVALID_ARGUMENT, "physical trade revision overflow"};
        }

        auto next = current->second;
        next.state = ExecutionOperationState::SETTLED;
        if (!next_revision_of(current->second.state_revision, next.state_revision)) {
            return {StorageResultKind::INVALID_ARGUMENT, "execution operation revision overflow"};
        }
        next.broker_trade_id = current->second.broker_trade_id;
        next.occurred_at_ms = m_clock.now_ms();

        auto unit = storage.begin();
        const auto operation_event_id = unit.next_journal_event_id_for_append();
        if (operation_event_id.kind != StorageResultKind::APPLIED) {
            return {operation_event_id.kind, operation_event_id.message};
        }
        JournalEvent operation_event{operation_event_id.value, "EXECUTION_OPERATION",
                                     "operation:" + next.operation_id,
                                     next.state_revision, trading_storage_schema_version,
                                     operation_key(next), next.occurred_at_ms, next};
        if (auto appended = unit.append_journal(operation_event);
            appended.kind != StorageResultKind::APPLIED) return appended;

        SettlementTransition transition;
        if (logical_transition) {
            transition = std::move(*logical_transition);
            if (!(transition.settlement.settlement == settlement)) {
                return {StorageResultKind::CONFLICT,
                        "logical transition settlement conflicts with reconciliation"};
            }
            transition.physical_trade = updated_physical;
            transition.expected_physical_trade_revision = physical.value->state_revision;
        } else {
            transition.settlement = {settlement, *settlement.net_profit, std::nullopt, true,
                                     trading_storage_schema_version};
            transition.journal = {0, "EXECUTION_SETTLED", "trade:" +
                                  std::to_string(operation.trade_id), 0,
                                  trading_storage_schema_version,
                                  "settlement:" + std::to_string(settlement.settlement_id),
                                  m_clock.now_ms(), {}};
            const auto settlement_event_id = unit.next_journal_event_id_for_append();
            if (settlement_event_id.kind != StorageResultKind::APPLIED) {
                return {settlement_event_id.kind, settlement_event_id.message};
            }
            transition.journal.event_id = settlement_event_id.value;
            transition.physical_trade = updated_physical;
            transition.expected_physical_trade_revision = physical.value->state_revision;
        }
        if (transition.journal.event_id == 0) {
            const auto settlement_event_id = unit.next_journal_event_id_for_append();
            if (settlement_event_id.kind != StorageResultKind::APPLIED) {
                return {settlement_event_id.kind, settlement_event_id.message};
            }
            transition.journal.event_id = settlement_event_id.value;
        }
        const auto settled = unit.apply_settlement_atomic(transition);
        if (settled.kind != StorageResultKind::APPLIED &&
            settled.kind != StorageResultKind::DUPLICATE) return settled;
        return unit.commit();
    }

private:
    static bool next_revision_of(std::uint64_t current,
                                 std::uint64_t& next) noexcept {
        if (current == (std::numeric_limits<std::uint64_t>::max)()) return false;
        next = current + 1;
        return true;
    }

    static bool same_identity(const ExecutionOperationRecord& lhs,
                              const ExecutionOperationRecord& rhs) noexcept {
        return lhs.operation_id == rhs.operation_id &&
               lhs.client_operation_id == rhs.client_operation_id &&
               lhs.intent_id == rhs.intent_id && lhs.group_id == rhs.group_id &&
               lhs.trade_id == rhs.trade_id && lhs.account_id == rhs.account_id &&
               lhs.risk_scope_id == rhs.risk_scope_id;
    }

    static bool same_preparation_payload(
        const ExecutionOperationRecord& lhs,
        const ExecutionOperationRecord& rhs) {
        auto left = lhs;
        auto right = rhs;
        // Creation time is durable evidence, but not preparation identity when
        // the same request is replayed after a restart.
        left.occurred_at_ms = 0;
        right.occurred_at_ms = 0;
        return nlohmann::json(left) == nlohmann::json(right);
    }

    static RecoveryResult corrupted(RecoveryResult result,
                                    std::string diagnostic,
                                    const JournalEvent& event) {
        result.kind = RecoveryResultKind::CORRUPTED;
        result.diagnostic = std::move(diagnostic);
        try {
            const auto risk_scope_id = event.payload.value(
                "risk_scope_id", std::uint64_t{0});
            if (risk_scope_id != 0) result.affected_risk_scopes.insert(risk_scope_id);
        } catch (...) {
            // A malformed payload cannot identify a safe scope; global_block remains set.
        }
        return result;
    }

    static bool transition_allowed(ExecutionOperationState from,
                                   ExecutionOperationState to) noexcept {
        if (from == to) {
            return from == ExecutionOperationState::EXECUTION_UNKNOWN ||
                   from == ExecutionOperationState::RECONCILIATION_REQUIRED;
        }
        switch (from) {
            case ExecutionOperationState::PREPARED:
                return to == ExecutionOperationState::SUBMISSION_STARTED;
            case ExecutionOperationState::SUBMISSION_STARTED:
                return to == ExecutionOperationState::BROKER_ACCEPTED ||
                       to == ExecutionOperationState::BROKER_ID_PERSISTED ||
                       to == ExecutionOperationState::EXECUTION_UNKNOWN ||
                       to == ExecutionOperationState::REJECTED ||
                       to == ExecutionOperationState::RECONCILIATION_REQUIRED;
            case ExecutionOperationState::BROKER_ACCEPTED:
                return to == ExecutionOperationState::BROKER_ID_PERSISTED ||
                       to == ExecutionOperationState::EXECUTED ||
                       to == ExecutionOperationState::SETTLED;
            case ExecutionOperationState::BROKER_ID_PERSISTED:
                return to == ExecutionOperationState::EXECUTED ||
                       to == ExecutionOperationState::SETTLED;
            case ExecutionOperationState::EXECUTED:
                return to == ExecutionOperationState::SETTLED;
            case ExecutionOperationState::EXECUTION_UNKNOWN:
            case ExecutionOperationState::RECONCILIATION_REQUIRED:
                return to == ExecutionOperationState::BROKER_ACCEPTED ||
                       to == ExecutionOperationState::BROKER_ID_PERSISTED ||
                       to == ExecutionOperationState::REJECTED ||
                       to == ExecutionOperationState::RECONCILIATION_REQUIRED;
            case ExecutionOperationState::SETTLED:
            case ExecutionOperationState::REJECTED:
            case ExecutionOperationState::CANCELLED:
                return false;
        }
        return false;
    }

    FakeExecutionClock& m_clock;
};

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_EXECUTION_RECOVERY_HPP_INCLUDED
