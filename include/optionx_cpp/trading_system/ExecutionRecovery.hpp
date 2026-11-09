#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_EXECUTION_RECOVERY_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_EXECUTION_RECOVERY_HPP_INCLUDED

/// \file ExecutionRecovery.hpp
/// \brief Offline execution journal, simulated broker and fail-closed recovery.

#include "TradingSystemStorage.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
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

    explicit SimulatedExecutionAdapter(bool broker_idempotency = true)
        : m_broker_idempotency(broker_idempotency) {}

    void set_plan(const std::string& operation_id, SimulatedSubmissionPlan plan) {
        m_plans[operation_id] = plan;
    }

    void set_history_available(bool available) noexcept { m_history_available = available; }
    void set_proven_not_executed(const std::string& operation_id, bool value) {
        m_not_executed[operation_id] = value;
    }

    void add_history_entry(HistoryEntry entry) { m_history.push_back(std::move(entry)); }

    SimulatedSubmissionResult submit(const SimulatedExecutionCommand& command) {
        ++m_submission_counts[command.operation_id];
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
        entry.broker_trade_id = "broker-" + std::to_string(++m_next_broker_id);
        m_history.push_back(entry);
        const bool id_available = plan != SimulatedSubmissionPlan::ACCEPTED_WITHOUT_BROKER_ID;
        return {true, true, plan == SimulatedSubmissionPlan::ACCEPTED_RESPONSE_LOST,
                id_available, id_available ? entry.broker_trade_id : std::string{}};
    }

    std::size_t submission_count(const std::string& operation_id) const noexcept {
        const auto found = m_submission_counts.find(operation_id);
        return found == m_submission_counts.end() ? 0U : found->second;
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
        for (const auto& entry : m_history) {
            if (entry.operation_id == operation_id) result.push_back(&entry);
        }
        return result;
    }

    std::vector<const HistoryEntry*> matching_entries(
        const ExecutionOperationRecord& operation) const {
        std::vector<const HistoryEntry*> result;
        for (const auto& entry : m_history) {
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

    bool m_broker_idempotency;
    bool m_history_available = true;
    std::uint64_t m_next_broker_id = 0;
    std::map<std::string, SimulatedSubmissionPlan> m_plans;
    std::map<std::string, std::size_t> m_submission_counts;
    std::map<std::string, bool> m_not_executed;
    std::vector<HistoryEntry> m_history;
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

    static std::map<std::string, ExecutionOperationRecord> latest(
        const TradingSystemStorage& storage) {
        std::map<std::string, ExecutionOperationRecord> result;
        std::map<std::string, std::uint64_t> latest_event_ids;
        const auto events = storage.journal_events("EXECUTION_OPERATION");
        if (!events.succeeded()) return result;
        for (const auto& event : events.values) {
            try {
                const auto operation = event.payload.get<ExecutionOperationRecord>();
                if (!operation.valid()) continue;
                const auto found = result.find(operation.operation_id);
                const auto previous_event = latest_event_ids.find(operation.operation_id);
                if (found == result.end() ||
                    operation.state_revision > found->second.state_revision ||
                    (operation.state_revision == found->second.state_revision &&
                     (previous_event == latest_event_ids.end() ||
                      event.event_id > previous_event->second))) {
                    result[operation.operation_id] = operation;
                    latest_event_ids[operation.operation_id] = event.event_id;
                }
            } catch (...) {
                // A malformed operation is deliberately omitted from the ready view.
            }
        }
        return result;
    }

    std::set<std::uint64_t> blocked_risk_scopes(
        const TradingSystemStorage& storage) const {
        std::set<std::uint64_t> result;
        for (const auto& item : latest(storage)) {
            if (unresolved(item.second.state)) result.insert(item.second.risk_scope_id);
        }
        return result;
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

    StorageResult prepare(TradingSystemStorage& storage,
                          const TradeIntent& intent,
                          const ExecutionRosterRecord& roster,
                          const std::vector<ReservationRecord>& reservations,
                          const std::vector<PhysicalTradeRecord>& physical_trades) const {
        if (!intent.valid() || intent.status != IntentStatus::ADMITTED || !roster.valid() ||
            reservations.size() != roster.members.size() ||
            physical_trades.size() != roster.members.size()) {
            return {StorageResultKind::INVALID_ARGUMENT, "invalid durable execution preparation"};
        }
        auto unit = storage.begin();
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

    StorageResult mark(TradingSystemStorage& storage, ExecutionOperationRecord operation) const {
        const auto all = latest(storage);
        const auto found = all.find(operation.operation_id);
        if (found != all.end()) {
            if (operation.state_revision != found->second.state_revision + 1) {
                return {StorageResultKind::REVISION_CONFLICT, "execution operation revision conflict"};
            }
            if (operation.client_operation_id != found->second.client_operation_id ||
                operation.trade_id != found->second.trade_id ||
                operation.account_id != found->second.account_id ||
                operation.risk_scope_id != found->second.risk_scope_id) {
                return {StorageResultKind::CONFLICT, "execution operation identity changed"};
            }
            if (!transition_allowed(found->second.state, operation.state)) {
                return {StorageResultKind::INVALID_STATE,
                        "invalid execution operation state transition"};
            }
        } else if (operation.state_revision != 0) {
            return {StorageResultKind::NOT_FOUND, "execution operation is not prepared"};
        }
        operation.occurred_at_ms = m_clock.now_ms();
        return persist_state(storage, std::move(operation));
    }

    StorageResult mark_submission_started(TradingSystemStorage& storage,
                                          const ExecutionOperationRecord& current) const {
        auto next = current;
        next.state = ExecutionOperationState::SUBMISSION_STARTED;
        next.state_revision = current.state_revision + 1;
        return mark(storage, std::move(next));
    }

    StorageResult mark_unknown(TradingSystemStorage& storage,
                               const ExecutionOperationRecord& current) const {
        auto next = current;
        next.state = ExecutionOperationState::EXECUTION_UNKNOWN;
        next.state_revision = current.state_revision + 1;
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
        auto started = current;
        started.state = ExecutionOperationState::SUBMISSION_STARTED;
        started.state_revision = current.state_revision + 1;
        auto result = mark(storage, started);
        if (result.kind != StorageResultKind::APPLIED) return result;

        const auto response = adapter.submit({current.operation_id,
                                               current.client_operation_id,
                                               current.account_id,
                                               current.trade_id});
        auto observed = started;
        observed.state_revision = started.state_revision + 1;
        if (!response.accepted) {
            observed.state = response.side_effect_occurred
                ? ExecutionOperationState::EXECUTION_UNKNOWN
                : ExecutionOperationState::REJECTED;
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
        const auto operations = latest(storage);
        const auto found = operations.find(operation.operation_id);
        if (found == operations.end()) {
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
            next.state = ExecutionOperationState::REJECTED;
        } else {
            next.state = ExecutionOperationState::RECONCILIATION_REQUIRED;
        }
        if (next.state == found->second.state &&
            (next.broker_trade_id.empty() ||
             next.broker_trade_id == found->second.broker_trade_id)) {
            return result;
        }
        next.state_revision = found->second.state_revision + 1;
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
        const auto current_operations = latest(storage);
        const auto current = current_operations.find(operation.operation_id);
        if (current == current_operations.end() || current->second.state == ExecutionOperationState::SETTLED) {
            return current == current_operations.end()
                ? StorageResult{StorageResultKind::NOT_FOUND, "execution operation is missing"}
                : StorageResult{StorageResultKind::DUPLICATE, {}};
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
        if (!settlement.valid() || settlement.intent_id != operation.intent_id ||
            settlement.execution_group_id != operation.group_id ||
            settlement.trade_id != operation.trade_id ||
            settlement.evidence != ExecutionEvidence::EXECUTED) {
            return {StorageResultKind::INVALID_ARGUMENT, "settlement does not match execution operation"};
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
        updated_physical.state_revision = physical.value->state_revision + 1;

        auto next = current->second;
        next.state = ExecutionOperationState::SETTLED;
        next.state_revision = current->second.state_revision + 1;
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
    static bool transition_allowed(ExecutionOperationState from,
                                   ExecutionOperationState to) noexcept {
        if (from == to) {
            return from == ExecutionOperationState::EXECUTION_UNKNOWN ||
                   from == ExecutionOperationState::RECONCILIATION_REQUIRED;
        }
        switch (from) {
            case ExecutionOperationState::PREPARED:
                return to == ExecutionOperationState::SUBMISSION_STARTED ||
                       to == ExecutionOperationState::BROKER_ACCEPTED ||
                       to == ExecutionOperationState::BROKER_ID_PERSISTED ||
                       to == ExecutionOperationState::REJECTED;
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
