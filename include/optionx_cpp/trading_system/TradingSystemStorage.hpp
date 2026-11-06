#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_STORAGE_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_STORAGE_HPP_INCLUDED

/// \file TradingSystemStorage.hpp
/// \brief Shared MDBX storage and atomic local transitions for Batch 5B.

#include "RiskAdmission.hpp"
#include "SeriesProgression.hpp"

#include <mdbx_containers/KeyValueTable.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace optionx::trading_system {

inline constexpr std::uint32_t trading_storage_schema_version = 1;

enum class StorageResultKind {
    APPLIED,
    DUPLICATE,
    CONFLICT,
    REVISION_CONFLICT,
    NOT_FOUND,
    INVALID_STATE,
    INVALID_ARGUMENT,
    UNSUPPORTED_SCHEMA,
    PENDING_VALUATION,
    STORAGE_ERROR
};

inline const char* to_string(StorageResultKind value) noexcept {
    switch (value) {
        case StorageResultKind::APPLIED: return "APPLIED";
        case StorageResultKind::DUPLICATE: return "DUPLICATE";
        case StorageResultKind::CONFLICT: return "CONFLICT";
        case StorageResultKind::REVISION_CONFLICT: return "REVISION_CONFLICT";
        case StorageResultKind::NOT_FOUND: return "NOT_FOUND";
        case StorageResultKind::INVALID_STATE: return "INVALID_STATE";
        case StorageResultKind::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case StorageResultKind::UNSUPPORTED_SCHEMA: return "UNSUPPORTED_SCHEMA";
        case StorageResultKind::PENDING_VALUATION: return "PENDING_VALUATION";
        case StorageResultKind::STORAGE_ERROR: return "STORAGE_ERROR";
    }
    return "STORAGE_ERROR";
}

struct StorageResult {
    StorageResultKind kind = StorageResultKind::INVALID_STATE;
    std::string message;

    bool succeeded() const noexcept {
        return kind == StorageResultKind::APPLIED || kind == StorageResultKind::DUPLICATE;
    }
};

struct SequenceResult {
    StorageResultKind kind = StorageResultKind::INVALID_STATE;
    std::uint64_t value = 0;
    std::string message;
};

template <class T>
struct StorageReadResult {
    StorageResultKind kind = StorageResultKind::INVALID_STATE;
    std::optional<T> value;
    std::string message;

    bool found() const noexcept {
        return kind == StorageResultKind::APPLIED && value.has_value();
    }
};

template <class T>
struct StorageCollectionResult {
    StorageResultKind kind = StorageResultKind::INVALID_STATE;
    std::vector<T> values;
    std::string message;

    bool succeeded() const noexcept {
        return kind == StorageResultKind::APPLIED;
    }
};

/// \brief Immutable valuation evidence retained with a converted decision.
struct ValuationFact {
    CurrencyType source = CurrencyType::UNKNOWN;
    CurrencyType target = CurrencyType::UNKNOWN;
    std::int64_t rate_numerator = 0;
    std::int64_t rate_denominator = 1;
    std::int64_t observed_at_ms = 0;
    std::string provider;
    std::string snapshot_id;
    std::string rounding_rule = "EXACT_FIXED_POINT";
    bool fresh = true;
    std::uint8_t precision = 0;

    bool valid() const noexcept {
        return source != CurrencyType::UNKNOWN && target != CurrencyType::UNKNOWN &&
               rate_numerator > 0 && rate_denominator > 0 && !snapshot_id.empty() &&
               !rounding_rule.empty();
    }
};

struct JournalEvent {
    std::uint64_t event_id = 0;
    std::string event_type;
    std::string aggregate_id;
    std::uint64_t state_revision = 0;
    std::uint32_t schema_version = trading_storage_schema_version;
    std::string idempotency_key;
    std::int64_t occurred_at_ms = 0;
    nlohmann::json payload = nlohmann::json::object();
};

struct RosterMemberRecord {
    std::uint64_t trade_id = 0;
    std::uint64_t account_id = 0;
    std::uint64_t risk_scope_id = 0;
    MoneyAmount stake;

    bool valid() const noexcept {
        return trade_id != 0 && account_id != 0 && risk_scope_id != 0 &&
               stake.valid_metadata() && stake.units > 0;
    }
};

/// \brief Immutable physical roster committed before an external submission.
struct ExecutionRosterRecord {
    ExecutionGroupId group_id = 0;
    IntentId intent_id = 0;
    std::uint32_t expected_members = 0;
    std::vector<RosterMemberRecord> members;
    std::uint32_t schema_version = trading_storage_schema_version;

    bool valid() const noexcept {
        if (group_id == 0 || intent_id == 0 || expected_members == 0 ||
            members.size() != expected_members) return false;
        std::set<std::uint64_t> trade_ids;
        for (const auto& member : members) {
            if (!member.valid() || !trade_ids.insert(member.trade_id).second) return false;
        }
        return true;
    }
};

struct ReservationRecord {
    std::uint64_t reservation_id = 0;
    std::uint64_t risk_scope_id = 0;
    RiskScopeKind risk_scope_kind = RiskScopeKind::ACCOUNT;
    IntentId intent_id = 0;
    ExecutionGroupIdentity execution_group;
    ExecutionCardinality cardinality = ExecutionCardinality::ZERO_OR_ONE;
    MoneyAmount stake;
    RiskReservationState state = RiskReservationState::RESERVED;
    std::uint64_t state_revision = 0;
    std::uint64_t account_revision = 0;
    MoneyAmount potential_risk;
    std::string idempotency_key;
    std::uint32_t schema_version = trading_storage_schema_version;

    bool valid() const noexcept {
        const bool valid_scope_kind = risk_scope_kind == RiskScopeKind::ACCOUNT ||
                                      risk_scope_kind == RiskScopeKind::STRATEGY ||
                                      risk_scope_kind == RiskScopeKind::GROUP;
        const bool valid_state = state == RiskReservationState::RESERVED ||
                                 state == RiskReservationState::SUBMITTED ||
                                 state == RiskReservationState::EXECUTION_UNKNOWN ||
                                 state == RiskReservationState::RELEASED ||
                                 state == RiskReservationState::SETTLED;
        return reservation_id != 0 && risk_scope_id != 0 && valid_scope_kind && intent_id != 0 &&
               execution_group.valid_for(cardinality) && stake.valid_metadata() &&
               stake.units > 0 && potential_risk.valid_metadata() &&
               potential_risk.units > 0 && stake.currency == potential_risk.currency &&
               stake.precision == potential_risk.precision && valid_state &&
               !idempotency_key.empty();
    }

    RiskReservation to_risk_reservation() const {
        return {reservation_id, {risk_scope_kind, risk_scope_id}, intent_id,
                execution_group, cardinality, idempotency_key, stake, potential_risk,
                state, account_revision};
    }
};

struct PhysicalTradeRecord {
    std::uint64_t trade_id = 0;
    IntentId intent_id = 0;
    ExecutionGroupId group_id = 0;
    std::uint64_t account_id = 0;
    std::uint64_t risk_scope_id = 0;
    std::string lifecycle_state;
    std::string broker_trade_id;
    std::uint64_t state_revision = 0;
    std::uint32_t schema_version = trading_storage_schema_version;

    bool valid() const noexcept {
        return trade_id != 0 && intent_id != 0 && group_id != 0 && account_id != 0 &&
               risk_scope_id != 0 && !lifecycle_state.empty();
    }
};

/// \brief Durable account identity binding without credentials or transport state.
struct AccountIdentityRecord {
    std::uint64_t account_id = 0;
    std::string platform_type;
    std::string external_identity;
    std::string realm;
    std::uint64_t canonical_account_id = 0;
    std::uint64_t risk_scope_id = 0;
    std::uint64_t identity_revision = 0;
    bool active = true;
    std::uint32_t schema_version = trading_storage_schema_version;

    bool valid() const noexcept {
        return account_id != 0 && canonical_account_id != 0 && risk_scope_id != 0 &&
               !platform_type.empty() && schema_version == trading_storage_schema_version &&
               (external_identity.empty() || !realm.empty());
    }
};

enum class RiskScopeGateState { READY, BLOCKED };

/// \brief Durable fail-closed gate for an unresolved account/scope conflict.
struct RiskScopeGateRecord {
    std::uint64_t risk_scope_id = 0;
    std::uint64_t related_scope_id = 0;
    RiskScopeGateState state = RiskScopeGateState::READY;
    std::string conflict_id;
    std::uint64_t state_revision = 0;
    std::uint32_t schema_version = trading_storage_schema_version;

    bool valid() const noexcept {
        return risk_scope_id != 0 && state == RiskScopeGateState::BLOCKED &&
               related_scope_id != 0 && !conflict_id.empty();
    }
};

struct SettlementRecord {
    TradeSettlement settlement;
    MoneyAmount native_profit;
    std::optional<ValuationFact> valuation;
    bool logical_outcome_ready = true;
    std::uint32_t schema_version = trading_storage_schema_version;

    bool valid() const noexcept {
        return settlement.valid() && native_profit.valid_metadata() &&
               (!settlement.net_profit ||
                (settlement.net_profit->units == native_profit.units &&
                 settlement.net_profit->precision == native_profit.precision &&
                 settlement.net_profit->currency == native_profit.currency)) &&
               (!valuation || valuation->valid());
    }
};

/// \brief Immutable second-stage fact that resolves a native settlement.
struct ValuationResolutionRecord {
    SettlementId settlement_id = 0;
    ValuationFact valuation;
    MoneyAmount logical_profit;
    CurrencyType logical_currency = CurrencyType::UNKNOWN;
    std::uint64_t state_revision = 0;
    std::string idempotency_key;
    std::uint32_t schema_version = trading_storage_schema_version;

    bool valid() const noexcept {
        return settlement_id != 0 && valuation.valid() && logical_profit.valid_metadata() &&
               logical_profit.currency == logical_currency && !idempotency_key.empty();
    }
};

/// \brief Complete durable checkpoint, including idempotency history.
struct SeriesCheckpoint {
    TradeSeries series;
    SeriesStatus status = SeriesStatus::ACTIVE;
    IntentId current_intent_id = 0;
    ExecutionGroupId current_group_id = 0;
    std::uint32_t current_step = 0;
    GroupOutcome current_outcome;
    std::vector<TradeSettlement> receipts;
    std::vector<IntentId> used_intents;
    std::vector<std::string> used_intent_keys;
    std::uint32_t schema_version = trading_storage_schema_version;
};

struct MoneyManagementStateRecord {
    std::string module_instance_id;
    std::string state_type;
    std::uint32_t schema_version = trading_storage_schema_version;
    std::uint64_t state_revision = 0;
    SettlementId last_settlement_id = 0;
    nlohmann::json payload = nlohmann::json::object();

    bool valid() const noexcept {
        return !module_instance_id.empty() && !state_type.empty();
    }
};

struct SettlementTransition {
    JournalEvent journal;
    SettlementRecord settlement;
    std::optional<SeriesCheckpoint> series;
    std::optional<PhysicalTradeRecord> physical_trade;
    std::optional<ReservationRecord> reservation;
    std::optional<MoneyManagementStateRecord> money_management;
    std::uint64_t expected_series_revision = 0;
    std::uint64_t expected_physical_trade_revision = 0;
    std::uint64_t expected_reservation_revision = 0;
    std::uint64_t expected_money_management_revision = 0;
    CurrencyType logical_currency = CurrencyType::UNKNOWN;
    std::vector<ReservationRecord> group_reservations;
    std::vector<std::uint64_t> expected_group_reservation_revisions;
};

struct ValuationTransition {
    JournalEvent journal;
    SettlementId settlement_id = 0;
    ValuationFact valuation;
    CurrencyType logical_currency = CurrencyType::UNKNOWN;
    std::optional<SeriesCheckpoint> series;
    std::optional<PhysicalTradeRecord> physical_trade;
    std::optional<ReservationRecord> reservation;
    std::optional<MoneyManagementStateRecord> money_management;
    std::uint64_t expected_series_revision = 0;
    std::uint64_t expected_physical_trade_revision = 0;
    std::uint64_t expected_reservation_revision = 0;
    std::uint64_t expected_money_management_revision = 0;
    std::vector<ReservationRecord> group_reservations;
    std::vector<std::uint64_t> expected_group_reservation_revisions;
};

namespace detail {

inline nlohmann::json money_to_json(const MoneyAmount& value) {
    return {{"units", value.units}, {"precision", value.precision}, {"currency", value.currency}};
}

inline MoneyAmount money_from_json(const nlohmann::json& value) {
    return {
        value.value("units", std::int64_t{0}),
        value.value("precision", std::uint8_t{0}),
        value.value("currency", CurrencyType::UNKNOWN)
    };
}

inline nlohmann::json valuation_to_json(const ValuationFact& value) {
    return {
        {"source", value.source}, {"target", value.target},
        {"rate_numerator", value.rate_numerator},
        {"rate_denominator", value.rate_denominator},
        {"observed_at_ms", value.observed_at_ms}, {"provider", value.provider},
        {"snapshot_id", value.snapshot_id}, {"rounding_rule", value.rounding_rule},
        {"fresh", value.fresh}, {"precision", value.precision}
    };
}

inline ValuationFact valuation_from_json(const nlohmann::json& value) {
    return {
        value.value("source", CurrencyType::UNKNOWN),
        value.value("target", CurrencyType::UNKNOWN),
        value.value("rate_numerator", std::int64_t{0}),
        value.value("rate_denominator", std::int64_t{1}),
        value.value("observed_at_ms", std::int64_t{0}),
        value.value("provider", std::string{}),
        value.value("snapshot_id", std::string{}),
        value.value("rounding_rule", std::string{"EXACT_FIXED_POINT"}),
        value.value("fresh", true),
        value.value("precision", std::uint8_t{0})
    };
}

inline nlohmann::json settlement_to_json(const TradeSettlement& value) {
    nlohmann::json result = {
        {"settlement_id", value.settlement_id}, {"intent_id", value.intent_id},
        {"execution_group_id", value.execution_group_id}, {"trade_id", value.trade_id},
        {"kind", static_cast<int>(value.kind)}, {"evidence", static_cast<int>(value.evidence)}
    };
    if (value.net_profit) result["net_profit"] = money_to_json(*value.net_profit);
    return result;
}

inline TradeSettlement settlement_from_json(const nlohmann::json& value) {
    TradeSettlement result;
    result.settlement_id = value.value("settlement_id", SettlementId{0});
    result.intent_id = value.value("intent_id", IntentId{0});
    result.execution_group_id = value.value("execution_group_id", ExecutionGroupId{0});
    result.trade_id = value.value("trade_id", std::uint64_t{0});
    result.kind = static_cast<OutcomeKind>(value.value("kind", 5));
    result.evidence = static_cast<ExecutionEvidence>(value.value("evidence", 1));
    if (value.contains("net_profit")) result.net_profit = money_from_json(value.at("net_profit"));
    return result;
}

inline nlohmann::json group_outcome_to_json(const GroupOutcome& value) {
    nlohmann::json result = {
        {"final", value.final}, {"unresolved", value.unresolved},
        {"all_members_terminal", value.all_members_terminal},
        {"completed_members", value.completed_members},
        {"aggregation_status", static_cast<int>(value.aggregation_status)}};
    if (value.kind) result["kind"] = static_cast<int>(*value.kind);
    if (value.net_profit) result["net_profit"] = money_to_json(*value.net_profit);
    return result;
}

inline GroupOutcome group_outcome_from_json(const nlohmann::json& value) {
    GroupOutcome result;
    result.final = value.value("final", false);
    result.unresolved = value.value("unresolved", false);
    result.all_members_terminal = value.value("all_members_terminal", false);
    result.completed_members = value.value("completed_members", std::uint32_t{0});
    result.aggregation_status = static_cast<AggregationStatus>(
        value.value("aggregation_status", 0));
    if (value.contains("kind")) {
        result.kind = static_cast<OutcomeKind>(value.at("kind").get<int>());
    }
    if (value.contains("net_profit")) result.net_profit = money_from_json(value.at("net_profit"));
    return result;
}

inline nlohmann::json journal_to_json(const JournalEvent& value) {
    return {
        {"event_id", value.event_id}, {"event_type", value.event_type},
        {"aggregate_id", value.aggregate_id}, {"state_revision", value.state_revision},
        {"schema_version", value.schema_version}, {"idempotency_key", value.idempotency_key},
        {"occurred_at_ms", value.occurred_at_ms}, {"payload", value.payload}
    };
}

inline JournalEvent journal_from_json(const nlohmann::json& value) {
    JournalEvent result;
    result.event_id = value.value("event_id", std::uint64_t{0});
    result.event_type = value.value("event_type", std::string{});
    result.aggregate_id = value.value("aggregate_id", std::string{});
    result.state_revision = value.value("state_revision", std::uint64_t{0});
    result.schema_version = value.value("schema_version", std::uint32_t{0});
    result.idempotency_key = value.value("idempotency_key", std::string{});
    result.occurred_at_ms = value.value("occurred_at_ms", std::int64_t{0});
    result.payload = value.value("payload", nlohmann::json::object());
    return result;
}

inline nlohmann::json reservation_to_json(const ReservationRecord& value) {
    return {
        {"reservation_id", value.reservation_id}, {"risk_scope_id", value.risk_scope_id},
        {"risk_scope_kind", static_cast<int>(value.risk_scope_kind)},
        {"intent_id", value.intent_id}, {"execution_group", value.execution_group},
        {"cardinality", static_cast<int>(value.cardinality)}, {"stake", money_to_json(value.stake)},
        {"state", static_cast<int>(value.state)}, {"state_revision", value.state_revision},
        {"account_revision", value.account_revision},
        {"potential_risk", money_to_json(value.potential_risk)},
        {"idempotency_key", value.idempotency_key}, {"schema_version", value.schema_version}
    };
}

inline nlohmann::json physical_trade_to_json(const PhysicalTradeRecord& value) {
    return {{"trade_id", value.trade_id}, {"intent_id", value.intent_id},
            {"group_id", value.group_id}, {"account_id", value.account_id},
            {"risk_scope_id", value.risk_scope_id}, {"lifecycle_state", value.lifecycle_state},
            {"broker_trade_id", value.broker_trade_id}, {"state_revision", value.state_revision},
            {"schema_version", value.schema_version}};
}

inline PhysicalTradeRecord physical_trade_from_json(const nlohmann::json& value) {
    return {value.value("trade_id", std::uint64_t{0}), value.value("intent_id", IntentId{0}),
            value.value("group_id", ExecutionGroupId{0}), value.value("account_id", std::uint64_t{0}),
            value.value("risk_scope_id", std::uint64_t{0}),
            value.value("lifecycle_state", std::string{}),
            value.value("broker_trade_id", std::string{}),
            value.value("state_revision", std::uint64_t{0}),
            value.value("schema_version", std::uint32_t{0})};
}

inline nlohmann::json account_to_json(const AccountIdentityRecord& value) {
    return {{"account_id", value.account_id}, {"platform_type", value.platform_type},
            {"external_identity", value.external_identity}, {"realm", value.realm},
            {"canonical_account_id", value.canonical_account_id},
            {"risk_scope_id", value.risk_scope_id},
            {"identity_revision", value.identity_revision}, {"active", value.active},
            {"schema_version", value.schema_version}};
}

inline AccountIdentityRecord account_from_json(const nlohmann::json& value) {
    return {value.value("account_id", std::uint64_t{0}),
            value.value("platform_type", std::string{}),
            value.value("external_identity", std::string{}),
            value.value("realm", std::string{}),
            value.value("canonical_account_id", std::uint64_t{0}),
            value.value("risk_scope_id", std::uint64_t{0}),
            value.value("identity_revision", std::uint64_t{0}),
            value.value("active", true),
            value.value("schema_version", std::uint32_t{0})};
}

inline nlohmann::json risk_scope_gate_to_json(const RiskScopeGateRecord& value) {
    return {{"risk_scope_id", value.risk_scope_id}, {"related_scope_id", value.related_scope_id},
            {"state", static_cast<int>(value.state)}, {"conflict_id", value.conflict_id},
            {"state_revision", value.state_revision}, {"schema_version", value.schema_version}};
}

inline RiskScopeGateRecord risk_scope_gate_from_json(const nlohmann::json& value) {
    return {value.value("risk_scope_id", std::uint64_t{0}),
            value.value("related_scope_id", std::uint64_t{0}),
            static_cast<RiskScopeGateState>(value.value("state", 1)),
            value.value("conflict_id", std::string{}),
            value.value("state_revision", std::uint64_t{0}),
            value.value("schema_version", std::uint32_t{0})};
}

inline ReservationRecord reservation_from_json(const nlohmann::json& value) {
    ReservationRecord result;
    result.reservation_id = value.value("reservation_id", std::uint64_t{0});
    result.risk_scope_id = value.value("risk_scope_id", std::uint64_t{0});
    result.risk_scope_kind = static_cast<RiskScopeKind>(value.value("risk_scope_kind", 0));
    result.intent_id = value.value("intent_id", IntentId{0});
    result.execution_group = value.value("execution_group", ExecutionGroupIdentity{});
    result.cardinality = static_cast<ExecutionCardinality>(value.value("cardinality", 0));
    result.stake = money_from_json(value.value("stake", nlohmann::json::object()));
    result.state = static_cast<RiskReservationState>(value.value("state", 0));
    result.state_revision = value.value("state_revision", std::uint64_t{0});
    result.account_revision = value.value("account_revision", std::uint64_t{0});
    result.potential_risk = money_from_json(value.value("potential_risk", nlohmann::json::object()));
    result.idempotency_key = value.value("idempotency_key", std::string{});
    result.schema_version = value.value("schema_version", std::uint32_t{0});
    return result;
}

inline nlohmann::json roster_to_json(const ExecutionRosterRecord& value) {
    nlohmann::json members = nlohmann::json::array();
    for (const auto& member : value.members) {
        members.push_back({{"trade_id", member.trade_id}, {"account_id", member.account_id},
                           {"risk_scope_id", member.risk_scope_id}, {"stake", money_to_json(member.stake)}});
    }
    return {{"group_id", value.group_id}, {"intent_id", value.intent_id},
            {"expected_members", value.expected_members}, {"members", std::move(members)},
            {"schema_version", value.schema_version}};
}

inline ExecutionRosterRecord roster_from_json(const nlohmann::json& value) {
    ExecutionRosterRecord result;
    result.group_id = value.value("group_id", ExecutionGroupId{0});
    result.intent_id = value.value("intent_id", IntentId{0});
    result.expected_members = value.value("expected_members", std::uint32_t{0});
    result.schema_version = value.value("schema_version", std::uint32_t{0});
    for (const auto& item : value.value("members", nlohmann::json::array())) {
        result.members.push_back({item.value("trade_id", std::uint64_t{0}),
                                  item.value("account_id", std::uint64_t{0}),
                                  item.value("risk_scope_id", std::uint64_t{0}),
                                  money_from_json(item.value("stake", nlohmann::json::object()))});
    }
    return result;
}

inline nlohmann::json settlement_record_to_json(const SettlementRecord& value) {
    nlohmann::json result = {{"settlement", settlement_to_json(value.settlement)},
                             {"native_profit", money_to_json(value.native_profit)},
                             {"logical_outcome_ready", value.logical_outcome_ready},
                             {"schema_version", value.schema_version}};
    if (value.valuation) result["valuation"] = valuation_to_json(*value.valuation);
    return result;
}

inline SettlementRecord settlement_record_from_json(const nlohmann::json& value) {
    SettlementRecord result;
    result.settlement = settlement_from_json(value.value("settlement", nlohmann::json::object()));
    result.native_profit = money_from_json(value.value("native_profit", nlohmann::json::object()));
    result.logical_outcome_ready = value.value("logical_outcome_ready", false);
    result.schema_version = value.value("schema_version", std::uint32_t{0});
    if (value.contains("valuation")) result.valuation = valuation_from_json(value.at("valuation"));
    return result;
}

inline nlohmann::json valuation_resolution_to_json(const ValuationResolutionRecord& value) {
    return {{"settlement_id", value.settlement_id}, {"valuation", valuation_to_json(value.valuation)},
            {"logical_profit", money_to_json(value.logical_profit)},
            {"logical_currency", value.logical_currency}, {"state_revision", value.state_revision},
            {"idempotency_key", value.idempotency_key}, {"schema_version", value.schema_version}};
}

inline ValuationResolutionRecord valuation_resolution_from_json(const nlohmann::json& value) {
    return {value.value("settlement_id", SettlementId{0}),
            valuation_from_json(value.value("valuation", nlohmann::json::object())),
            money_from_json(value.value("logical_profit", nlohmann::json::object())),
            value.value("logical_currency", CurrencyType::UNKNOWN),
            value.value("state_revision", std::uint64_t{0}),
            value.value("idempotency_key", std::string{}),
            value.value("schema_version", std::uint32_t{0})};
}

inline nlohmann::json checkpoint_to_json(const SeriesCheckpoint& value) {
    nlohmann::json receipts = nlohmann::json::array();
    for (const auto& receipt : value.receipts) receipts.push_back(settlement_to_json(receipt));
    return {{"series", value.series}, {"status", static_cast<int>(value.status)},
            {"current_intent_id", value.current_intent_id}, {"current_group_id", value.current_group_id},
            {"current_step", value.current_step}, {"current_outcome", group_outcome_to_json(value.current_outcome)},
            {"receipts", std::move(receipts)},
            {"used_intents", value.used_intents}, {"used_intent_keys", value.used_intent_keys},
            {"schema_version", value.schema_version}};
}

inline SeriesCheckpoint checkpoint_from_json(const nlohmann::json& value) {
    SeriesCheckpoint result;
    result.series = value.value("series", TradeSeries{});
    result.status = static_cast<SeriesStatus>(value.value("status", 0));
    result.current_intent_id = value.value("current_intent_id", IntentId{0});
    result.current_group_id = value.value("current_group_id", ExecutionGroupId{0});
    result.current_step = value.value("current_step", std::uint32_t{0});
    result.current_outcome = group_outcome_from_json(
        value.value("current_outcome", nlohmann::json::object()));
    for (const auto& receipt : value.value("receipts", nlohmann::json::array())) {
        result.receipts.push_back(settlement_from_json(receipt));
    }
    result.used_intents = value.value("used_intents", std::vector<IntentId>{});
    result.used_intent_keys = value.value("used_intent_keys", std::vector<std::string>{});
    result.schema_version = value.value("schema_version", std::uint32_t{0});
    return result;
}

inline nlohmann::json mm_to_json(const MoneyManagementStateRecord& value) {
    return {{"module_instance_id", value.module_instance_id}, {"state_type", value.state_type},
            {"schema_version", value.schema_version}, {"state_revision", value.state_revision},
            {"last_settlement_id", value.last_settlement_id}, {"payload", value.payload}};
}

inline MoneyManagementStateRecord mm_from_json(const nlohmann::json& value) {
    MoneyManagementStateRecord result;
    result.module_instance_id = value.value("module_instance_id", std::string{});
    result.state_type = value.value("state_type", std::string{});
    result.schema_version = value.value("schema_version", std::uint32_t{0});
    result.state_revision = value.value("state_revision", std::uint64_t{0});
    result.last_settlement_id = value.value("last_settlement_id", SettlementId{0});
    result.payload = value.value("payload", nlohmann::json::object());
    return result;
}

inline bool json_equal(const std::string& lhs, const nlohmann::json& rhs) {
    try { return nlohmann::json::parse(lhs) == rhs; } catch (...) { return false; }
}

inline bool valid_group_outcome(const GroupOutcome& value) noexcept {
    const bool valid_aggregation_status =
        value.aggregation_status == AggregationStatus::PENDING ||
        value.aggregation_status == AggregationStatus::READY ||
        value.aggregation_status == AggregationStatus::NUMERIC_OVERFLOW;
    if (!valid_aggregation_status || value.completed_members == 0 && value.all_members_terminal) {
        return false;
    }
    if (value.kind) {
        const auto kind = *value.kind;
        if (kind != OutcomeKind::WIN && kind != OutcomeKind::LOSS &&
            kind != OutcomeKind::DRAW && kind != OutcomeKind::REJECTED &&
            kind != OutcomeKind::CANCELLED && kind != OutcomeKind::UNKNOWN) {
            return false;
        }
    }
    return !value.net_profit || value.net_profit->valid_metadata();
}

inline bool durable_record_valid(const JournalEvent& value) noexcept {
    return value.event_id != 0 && !value.event_type.empty() &&
           !value.aggregate_id.empty() && !value.idempotency_key.empty();
}

inline bool durable_record_valid(const TradeIntent& value) noexcept { return value.valid(); }
inline bool durable_record_valid(const SettlementRecord& value) noexcept { return value.valid(); }
inline bool durable_record_valid(const ValuationResolutionRecord& value) noexcept {
    return value.valid();
}
inline bool durable_record_valid(const ExecutionRosterRecord& value) noexcept { return value.valid(); }
inline bool durable_record_valid(const ReservationRecord& value) noexcept { return value.valid(); }
inline bool durable_record_valid(const PhysicalTradeRecord& value) noexcept { return value.valid(); }
inline bool durable_record_valid(const AccountIdentityRecord& value) noexcept { return value.valid(); }
inline bool durable_record_valid(const RiskScopeGateRecord& value) noexcept { return value.valid(); }
inline bool durable_record_valid(const MoneyManagementStateRecord& value) noexcept {
    return value.valid();
}

inline bool durable_record_valid(const SeriesCheckpoint& value) noexcept {
    const bool valid_status = value.status == SeriesStatus::ACTIVE ||
                              value.status == SeriesStatus::WAITING_OUTCOME ||
                              value.status == SeriesStatus::UNRESOLVED ||
                              value.status == SeriesStatus::COMPLETED ||
                              value.status == SeriesStatus::BLOCKED_ERROR;
    if (!value.series.valid() || !valid_status || !valid_group_outcome(value.current_outcome)) {
        return false;
    }
    std::set<SettlementId> settlement_ids;
    for (const auto& receipt : value.receipts) {
        if (!receipt.valid() || !settlement_ids.insert(receipt.settlement_id).second) return false;
    }
    for (const auto intent_id : value.used_intents) {
        if (intent_id == 0) return false;
    }
    for (const auto& key : value.used_intent_keys) {
        if (key.empty()) return false;
    }
    return true;
}

template<class T>
bool durable_record_valid(const T&) noexcept { return true; }

inline std::optional<MoneyAmount> convert_money(
    const MoneyAmount& native, const ValuationFact& valuation, CurrencyType target) {
    if (!native.valid_metadata() || !valuation.valid() || !valuation.fresh ||
        valuation.source != native.currency || valuation.target != target ||
        valuation.precision > detail::max_money_precision ||
        (valuation.rounding_rule != "EXACT_FIXED_POINT" &&
         valuation.rounding_rule != "ROUND_HALF_UP")) {
        return std::nullopt;
    }

    std::uint64_t numerator = static_cast<std::uint64_t>(valuation.rate_numerator);
    std::uint64_t denominator = static_cast<std::uint64_t>(valuation.rate_denominator);
    const auto scale = valuation.precision > native.precision
        ? detail::power_of_ten(static_cast<std::uint8_t>(valuation.precision - native.precision))
        : detail::power_of_ten(static_cast<std::uint8_t>(native.precision - valuation.precision));
    if (valuation.precision > native.precision) {
        const auto divisor = std::gcd(numerator, denominator);
        numerator /= divisor;
        denominator /= divisor;
        if (scale != 0 && numerator > (std::numeric_limits<std::uint64_t>::max)() / scale) {
            return std::nullopt;
        }
        numerator *= scale;
    } else if (valuation.precision < native.precision) {
        const auto divisor = std::gcd(numerator, denominator);
        numerator /= divisor;
        denominator /= divisor;
        if (scale != 0 && denominator > (std::numeric_limits<std::uint64_t>::max)() / scale) {
            return std::nullopt;
        }
        denominator *= scale;
    }

    const bool negative = native.units < 0;
    const std::uint64_t magnitude = negative
        ? static_cast<std::uint64_t>(-(native.units + 1)) + 1
        : static_cast<std::uint64_t>(native.units);
    const auto first_divisor = std::gcd(magnitude, denominator);
    const auto reduced_magnitude = magnitude / first_divisor;
    denominator /= first_divisor;
    const auto second_divisor = std::gcd(numerator, denominator);
    numerator /= second_divisor;
    denominator /= second_divisor;
    if (reduced_magnitude != 0 && numerator >
        (std::numeric_limits<std::uint64_t>::max)() / reduced_magnitude) {
        return std::nullopt;
    }
    const auto product = reduced_magnitude * numerator;
    auto result_magnitude = product / denominator;
    const auto remainder = product % denominator;
    if (remainder != 0) {
        if (valuation.rounding_rule == "EXACT_FIXED_POINT") return std::nullopt;
        if (remainder >= denominator - remainder) {
            if (result_magnitude == (std::numeric_limits<std::uint64_t>::max)()) return std::nullopt;
            ++result_magnitude;
        }
    }
    const auto max_positive = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    const auto max_magnitude = negative ? max_positive + 1 : max_positive;
    if (result_magnitude > max_magnitude) return std::nullopt;
    MoneyAmount result{0, valuation.precision, target};
    if (!negative) {
        result.units = static_cast<std::int64_t>(result_magnitude);
    } else if (result_magnitude == max_positive + 1) {
        result.units = (std::numeric_limits<std::int64_t>::min)();
    } else {
        result.units = -static_cast<std::int64_t>(result_magnitude);
    }
    return result;
}

inline bool same_group_outcome(const GroupOutcome& lhs, const GroupOutcome& rhs) noexcept {
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

inline std::optional<GroupOutcome> normalize_group_outcome(
    const GroupOutcome& native, const ValuationFact* valuation, CurrencyType target) {
    auto result = native;
    if (!result.net_profit) return result;
    if (result.net_profit->currency == target) return result;
    if (!valuation) return std::nullopt;
    const auto converted = convert_money(*result.net_profit, *valuation, target);
    if (!converted) return std::nullopt;
    result.net_profit = *converted;
    return result;
}

} // namespace detail

/// \class TradingSystemStorage
/// \brief Owns one shared MDBX environment and the Batch 5B named tables.
class TradingSystemStorage {
public:
    explicit TradingSystemStorage(mdbxc::Config config)
        : m_config(normalize_config(std::move(config))), m_connection(mdbxc::Connection::create(m_config)),
          m_journal(m_connection, "ts_journal"), m_journal_ids(m_connection, "ts_journal_ids"),
          m_intents(m_connection, "ts_intents"), m_rosters(m_connection, "ts_rosters"),
          m_reservations(m_connection, "ts_reservations"),
          m_physical_trades(m_connection, "ts_physical_trades"),
          m_accounts(m_connection, "ts_accounts"),
          m_risk_scope_gates(m_connection, "ts_risk_scope_gates"),
          m_settlements(m_connection, "ts_settlements"),
          m_valuation_resolutions(m_connection, "ts_valuation_resolutions"),
          m_series(m_connection, "ts_series"), m_mm_state(m_connection, "ts_mm_state"),
          m_sequences(m_connection, "ts_sequences") {}

    ~TradingSystemStorage() {
        if (m_connection && m_connection->is_connected()) m_connection->shutdown();
    }

    const std::shared_ptr<mdbxc::Connection>& connection() const noexcept { return m_connection; }

    TradingSystemStorage(const TradingSystemStorage&) = delete;
    TradingSystemStorage& operator=(const TradingSystemStorage&) = delete;

    StorageReadResult<JournalEvent> journal_by_key(const std::string& key) const {
        return read_json<JournalEvent>(m_journal, key, detail::journal_from_json);
    }

    /// \brief Reads journal facts used by the future recovery stage.
    ///
    /// An empty event type returns all events. The query is read-only and
    /// returns canonical facts, not mutable runtime projections.
    StorageCollectionResult<JournalEvent> journal_events(
        const std::string& event_type = {}) const {
        return read_all_json<JournalEvent>(
            m_journal, detail::journal_from_json,
            [&event_type](const JournalEvent& value) {
                return event_type.empty() || value.event_type == event_type;
            });
    }

    StorageReadResult<TradeIntent> intent(IntentId id) const {
        return read_json<TradeIntent>(m_intents, std::to_string(id),
                                      [](const nlohmann::json& value) {
                                          return value.contains("intent")
                                              ? value.at("intent").get<TradeIntent>()
                                              : value.get<TradeIntent>();
                                      });
    }

    StorageReadResult<SettlementRecord> settlement(SettlementId id) const {
        return read_json<SettlementRecord>(m_settlements, std::to_string(id), detail::settlement_record_from_json);
    }

    StorageReadResult<ValuationResolutionRecord> valuation_resolution(SettlementId id) const {
        return read_json<ValuationResolutionRecord>(
            m_valuation_resolutions, std::to_string(id), detail::valuation_resolution_from_json);
    }

    StorageCollectionResult<SettlementRecord> settlements_for_group(
        ExecutionGroupId id) const {
        return read_all_json<SettlementRecord>(
            m_settlements, detail::settlement_record_from_json,
            [id](const SettlementRecord& value) {
                return value.settlement.execution_group_id == id;
            });
    }

    StorageCollectionResult<SettlementRecord> pending_valuations() const {
        auto all = read_all_json<SettlementRecord>(
            m_settlements, detail::settlement_record_from_json,
            [](const SettlementRecord&) { return true; });
        if (!all.succeeded()) return all;

        StorageCollectionResult<SettlementRecord> result{StorageResultKind::APPLIED, {}, {}};
        for (const auto& value : all.values) {
            if (value.logical_outcome_ready) continue;
            const auto resolution = valuation_resolution(value.settlement.settlement_id);
            if (resolution.kind == StorageResultKind::NOT_FOUND) {
                result.values.push_back(value);
            } else if (resolution.kind != StorageResultKind::APPLIED) {
                return {resolution.kind, {}, resolution.message};
            }
        }
        return result;
    }

    StorageReadResult<ExecutionRosterRecord> roster(ExecutionGroupId id) const {
        return read_json<ExecutionRosterRecord>(m_rosters, std::to_string(id), detail::roster_from_json);
    }

    StorageReadResult<ReservationRecord> reservation(std::uint64_t id) const {
        return read_json<ReservationRecord>(m_reservations, std::to_string(id), detail::reservation_from_json);
    }

    StorageCollectionResult<ReservationRecord> reservations_for_scope(
        std::uint64_t risk_scope_id) const {
        return read_all_json<ReservationRecord>(
            m_reservations, detail::reservation_from_json,
            [risk_scope_id](const ReservationRecord& value) {
                return value.risk_scope_id == risk_scope_id;
            });
    }

    StorageReadResult<PhysicalTradeRecord> physical_trade(std::uint64_t id) const {
        return read_json<PhysicalTradeRecord>(m_physical_trades, std::to_string(id),
                                              detail::physical_trade_from_json);
    }

    StorageReadResult<AccountIdentityRecord> account(std::uint64_t id) const {
        return read_json<AccountIdentityRecord>(m_accounts, std::to_string(id),
                                                detail::account_from_json);
    }

    StorageReadResult<RiskScopeGateRecord> risk_scope_gate(std::uint64_t id) const {
        return read_json<RiskScopeGateRecord>(m_risk_scope_gates, std::to_string(id),
                                              detail::risk_scope_gate_from_json);
    }

    StorageCollectionResult<AccountIdentityRecord> accounts_for_scope(
        std::uint64_t risk_scope_id) const {
        return read_all_json<AccountIdentityRecord>(
            m_accounts, detail::account_from_json,
            [risk_scope_id](const AccountIdentityRecord& value) {
                return value.risk_scope_id == risk_scope_id;
            });
    }

    StorageCollectionResult<PhysicalTradeRecord> physical_trades_in_state(
        const std::string& lifecycle_state) const {
        return read_all_json<PhysicalTradeRecord>(
            m_physical_trades, detail::physical_trade_from_json,
            [&lifecycle_state](const PhysicalTradeRecord& value) {
                return lifecycle_state.empty() || value.lifecycle_state == lifecycle_state;
            });
    }

    StorageReadResult<SeriesCheckpoint> series(SeriesId id) const {
        return read_json<SeriesCheckpoint>(m_series, std::to_string(id), detail::checkpoint_from_json);
    }

    StorageReadResult<MoneyManagementStateRecord> money_management(const std::string& id) const {
        return read_json<MoneyManagementStateRecord>(m_mm_state, id, detail::mm_from_json);
    }

    class UnitOfWork;
    UnitOfWork begin();

private:
    friend class UnitOfWork;
    mdbxc::Config m_config;
    std::shared_ptr<mdbxc::Connection> m_connection;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_journal;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_journal_ids;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_intents;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_rosters;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_reservations;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_physical_trades;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_accounts;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_risk_scope_gates;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_settlements;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_valuation_resolutions;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_series;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_mm_state;
    mutable mdbxc::KeyValueTable<std::string, std::string> m_sequences;

    static mdbxc::Config normalize_config(mdbxc::Config config) {
        if (config.max_dbs < 16) config.max_dbs = 16;
        return config;
    }

    template<class T, class Parser>
    StorageReadResult<T> read_json(const mdbxc::KeyValueTable<std::string, std::string>& table,
                                   const std::string& key, Parser parser) const {
        try {
            auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
            const auto value = table.find(key, txn);
            if (!value) return {StorageResultKind::NOT_FOUND, std::nullopt, {}};
            const auto document = nlohmann::json::parse(*value);
            if (!document.contains("schema_version")) {
                return {StorageResultKind::STORAGE_ERROR, std::nullopt,
                        "missing durable schema"};
            }
            if (document.at("schema_version").get<std::uint32_t>() != trading_storage_schema_version) {
                return {StorageResultKind::UNSUPPORTED_SCHEMA, std::nullopt, "unsupported durable schema"};
            }
            auto parsed = parser(document);
            if (!detail::durable_record_valid(parsed)) {
                return {StorageResultKind::STORAGE_ERROR, std::nullopt,
                        "invalid durable record"};
            }
            return {StorageResultKind::APPLIED, std::move(parsed), {}};
        } catch (const std::exception& error) {
            return {StorageResultKind::STORAGE_ERROR, std::nullopt, error.what()};
        }
    }

    template<class T, class Parser, class Predicate>
    StorageCollectionResult<T> read_all_json(
        mdbxc::KeyValueTable<std::string, std::string>& table,
        Parser parser,
        Predicate predicate) const {
        try {
            std::vector<std::pair<std::string, std::string>> encoded;
            table.load(encoded);
            std::vector<T> values;
            values.reserve(encoded.size());
            for (const auto& entry : encoded) {
                const auto document = nlohmann::json::parse(entry.second);
                if (!document.contains("schema_version")) {
                    return {StorageResultKind::STORAGE_ERROR, {}, "missing durable schema"};
                }
                if (document.at("schema_version").get<std::uint32_t>() !=
                    trading_storage_schema_version) {
                    return {StorageResultKind::UNSUPPORTED_SCHEMA, {},
                            "unsupported durable schema"};
                }
                auto parsed = parser(document);
                if (!detail::durable_record_valid(parsed)) {
                    return {StorageResultKind::STORAGE_ERROR, {}, "invalid durable record"};
                }
                if (predicate(parsed)) values.push_back(std::move(parsed));
            }
            return {StorageResultKind::APPLIED, std::move(values), {}};
        } catch (const std::exception& error) {
            return {StorageResultKind::STORAGE_ERROR, {}, error.what()};
        }
    }
};

/// \class TradingSystemStorage::UnitOfWork
/// \brief Coordinates all Batch 5B writes through one transaction.
class TradingSystemStorage::UnitOfWork {
public:
    explicit UnitOfWork(TradingSystemStorage& storage)
        : m_storage(storage), m_transaction(storage.m_connection->transaction(mdbxc::TransactionMode::WRITABLE)) {}

    ~UnitOfWork() {
        if (!m_closed) m_transaction.rollback();
    }

    UnitOfWork(const UnitOfWork&) = delete;
    UnitOfWork& operator=(const UnitOfWork&) = delete;

    StorageResult append_journal(const JournalEvent& event) {
        if (!valid_schema(event.schema_version) || event.event_id == 0 ||
            event.event_type.empty() || event.aggregate_id.empty() || event.idempotency_key.empty()) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "invalid journal identity or schema"});
        }
        try {
            const auto encoded = detail::journal_to_json(event).dump();
            const auto existing = m_storage.m_journal.find(event.idempotency_key, m_transaction);
            if (existing) {
                return existing->compare(encoded) == 0
                    ? StorageResult{StorageResultKind::DUPLICATE, {}}
                    : fail({StorageResultKind::CONFLICT, "journal idempotency payload conflict"});
            }
            const auto event_key = std::to_string(event.event_id);
            if (m_storage.m_journal_ids.find(event_key, m_transaction)) {
                return fail({StorageResultKind::CONFLICT, "journal event id already exists"});
            }
            m_storage.m_journal.insert(event.idempotency_key, encoded, m_transaction);
            m_storage.m_journal_ids.insert(event_key, event.idempotency_key, m_transaction);
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    StorageResult put_intent(const TradeIntent& intent) {
        if (!intent.valid()) return fail({StorageResultKind::INVALID_ARGUMENT, "invalid intent"});
        const nlohmann::json encoded = {
            {"schema_version", trading_storage_schema_version}, {"intent", nlohmann::json(intent)}};
        return put_immutable(m_storage.m_intents, std::to_string(intent.intent_id), encoded.dump());
    }

    /// \brief Allocates the next durable identifier inside this transaction.
    SequenceResult next_id(const std::string& sequence_name) {
        if (sequence_name.empty()) return fail_sequence("sequence name is empty");
        try {
            const auto current = m_storage.m_sequences.find(sequence_name, m_transaction);
            std::size_t parsed_length = 0;
            const auto next = current
                ? std::stoull(*current, &parsed_length)
                : std::uint64_t{1};
            if (current && parsed_length != current->size()) {
                return fail_sequence("durable identifier sequence is malformed");
            }
            if (next == 0 || next == (std::numeric_limits<std::uint64_t>::max)()) {
                return fail_sequence("durable identifier sequence exhausted");
            }
            m_storage.m_sequences.insert_or_assign(
                sequence_name, std::to_string(next + 1), m_transaction);
            return {StorageResultKind::APPLIED, next, {}};
        } catch (const std::exception& error) {
            m_failed = true;
            return {StorageResultKind::STORAGE_ERROR, 0, error.what()};
        }
    }

    /// \brief Persists an account binding or its deactivated tombstone.
    ///
    /// A matching external identity may be represented by aliases only when
    /// it resolves to the same risk scope. Ambiguous scope reuse is rejected.
    StorageResult put_account(const AccountIdentityRecord& value) {
        if (!valid_schema(value.schema_version) || !value.valid()) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "invalid account identity"});
        }
        try {
            std::vector<std::pair<std::string, std::string>> encoded;
            m_storage.m_accounts.load(encoded, m_transaction);
            for (const auto& entry : encoded) {
                const auto existing = detail::account_from_json(nlohmann::json::parse(entry.second));
                if (existing.account_id == value.account_id ||
                    value.external_identity.empty() ||
                    existing.external_identity != value.external_identity ||
                    existing.platform_type != value.platform_type ||
                    existing.realm != value.realm) {
                    continue;
                }
                if (existing.risk_scope_id != value.risk_scope_id) {
                    const auto lower = std::min(existing.risk_scope_id, value.risk_scope_id);
                    const auto upper = std::max(existing.risk_scope_id, value.risk_scope_id);
                    const auto conflict_id = "account-scope:" + std::to_string(lower) + ":" +
                                             std::to_string(upper) + ":" + value.external_identity;
                    const auto first_gate = persist_scope_gate(
                        value.risk_scope_id, existing.risk_scope_id, conflict_id);
                    if (first_gate.kind != StorageResultKind::APPLIED &&
                        first_gate.kind != StorageResultKind::DUPLICATE) return first_gate;
                    const auto second_gate = persist_scope_gate(
                        existing.risk_scope_id, value.risk_scope_id, conflict_id);
                    if (second_gate.kind != StorageResultKind::APPLIED &&
                        second_gate.kind != StorageResultKind::DUPLICATE) return second_gate;
                    return {StorageResultKind::CONFLICT,
                            "external account identity maps to another risk scope"};
                }
            }
            return put_revisioned(m_storage.m_accounts, std::to_string(value.account_id),
                                  detail::account_to_json(value).dump(),
                                  value.identity_revision, value.identity_revision == 0
                                      ? 0 : value.identity_revision - 1,
                                  "identity_revision");
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    StorageResult put_roster(const ExecutionRosterRecord& roster) {
        if (!valid_schema(roster.schema_version)) {
            return fail({StorageResultKind::UNSUPPORTED_SCHEMA, "unsupported roster schema"});
        }
        if (!valid_schema(roster.schema_version) || !roster.valid()) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "invalid execution roster"});
        }
        return put_immutable(m_storage.m_rosters, std::to_string(roster.group_id), detail::roster_to_json(roster).dump());
    }

    StorageResult put_reservation(const ReservationRecord& value, std::uint64_t expected_revision) {
        if (!valid_schema(value.schema_version)) {
            return fail({StorageResultKind::UNSUPPORTED_SCHEMA, "unsupported reservation schema"});
        }
        if (!valid_schema(value.schema_version) || !value.valid()) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "invalid reservation"});
        }
        return put_revisioned(m_storage.m_reservations, std::to_string(value.reservation_id),
                              detail::reservation_to_json(value).dump(), value.state_revision, expected_revision);
    }

    StorageResult put_physical_trade(const PhysicalTradeRecord& value,
                                     std::uint64_t expected_revision) {
        if (!valid_schema(value.schema_version)) {
            return fail({StorageResultKind::UNSUPPORTED_SCHEMA, "unsupported physical-trade schema"});
        }
        if (!value.valid()) return fail({StorageResultKind::INVALID_ARGUMENT, "invalid physical trade"});
        return put_revisioned(m_storage.m_physical_trades, std::to_string(value.trade_id),
                              detail::physical_trade_to_json(value).dump(),
                              value.state_revision, expected_revision);
    }

    StorageResult put_series(const SeriesCheckpoint& value, std::uint64_t expected_revision) {
        if (!valid_schema(value.schema_version)) {
            return fail({StorageResultKind::UNSUPPORTED_SCHEMA, "unsupported series schema"});
        }
        if (!valid_schema(value.schema_version) || !value.series.valid()) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "invalid series checkpoint"});
        }
        try {
            const auto key = std::to_string(value.series.series_id);
            const auto encoded = detail::checkpoint_to_json(value).dump();
            const auto existing = m_storage.m_series.find(key, m_transaction);
            if (existing) {
                const auto current = detail::checkpoint_from_json(nlohmann::json::parse(*existing));
                if (current.series.state_revision == value.series.state_revision &&
                    *existing == encoded) {
                    return {StorageResultKind::DUPLICATE, {}};
                }
                if (current.series.state_revision != expected_revision) {
                    return fail({StorageResultKind::REVISION_CONFLICT, "stale durable series revision"});
                }
                if (expected_revision == (std::numeric_limits<std::uint64_t>::max)() ||
                    value.series.state_revision != expected_revision + 1) {
                    return fail({StorageResultKind::INVALID_ARGUMENT,
                                 "series revision must advance by one"});
                }
            } else if (expected_revision != 0 || value.series.state_revision != 0) {
                return fail({StorageResultKind::REVISION_CONFLICT, "missing durable series record"});
            }
            m_storage.m_series.insert_or_assign(key, encoded, m_transaction);
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    StorageResult put_money_management(const MoneyManagementStateRecord& value,
                                       std::uint64_t expected_revision) {
        if (!valid_schema(value.schema_version)) {
            return fail({StorageResultKind::UNSUPPORTED_SCHEMA, "unsupported money-management schema"});
        }
        if (!valid_schema(value.schema_version) || !value.valid()) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "invalid money-management state"});
        }
        return put_revisioned(m_storage.m_mm_state, value.module_instance_id,
                              detail::mm_to_json(value).dump(), value.state_revision, expected_revision);
    }

    StorageResult put_settlement(const SettlementRecord& value) {
        if (!valid_schema(value.schema_version)) {
            return fail({StorageResultKind::UNSUPPORTED_SCHEMA, "unsupported settlement schema"});
        }
        if (!valid_schema(value.schema_version) || !value.valid()) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "invalid settlement record"});
        }
        return put_immutable(m_storage.m_settlements, std::to_string(value.settlement.settlement_id),
                             detail::settlement_record_to_json(value).dump());
    }

    StorageResult put_valuation_resolution(const ValuationResolutionRecord& value) {
        if (!valid_schema(value.schema_version) || !value.valid()) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "invalid valuation resolution"});
        }
        return put_immutable(m_storage.m_valuation_resolutions,
                             std::to_string(value.settlement_id),
                             detail::valuation_resolution_to_json(value).dump());
    }

    /// \brief Applies a settlement and all supplied projections before commit.
    StorageResult apply_settlement_atomic(const SettlementTransition& transition) {
        if (!transition.settlement.valid() || transition.journal.event_id == 0 ||
            transition.journal.idempotency_key.empty() || !transition.physical_trade) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "invalid settlement transition"});
        }
        if (transition.logical_currency == CurrencyType::UNKNOWN) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "logical currency is required"});
        }
        if (transition.settlement.valuation &&
            (transition.settlement.valuation->source != transition.settlement.native_profit.currency ||
             transition.settlement.valuation->target != transition.logical_currency)) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "valuation fact does not match native and logical currencies"});
        }

        try {
            const auto physical_binding = validate_physical_bindings(transition);
            if (physical_binding.kind != StorageResultKind::APPLIED) return physical_binding;

            ExecutionGroupAggregator aggregate;
            const auto aggregate_result = build_group_aggregate(
                transition.settlement.settlement.execution_group_id,
                &transition.settlement, aggregate);
            if (aggregate_result.kind != StorageResultKind::APPLIED) return aggregate_result;

            const bool has_logical_projections =
                transition.series || transition.reservation || transition.money_management ||
                !transition.group_reservations.empty();
            const bool group_terminal = aggregate.outcome().final &&
                                        aggregate.outcome().all_members_terminal &&
                                        aggregate.outcome().aggregation_status == AggregationStatus::READY;
            const auto physical_valuation = transition.settlement.native_profit.currency ==
                                                    transition.logical_currency
                                                ? std::optional<MoneyAmount>(transition.settlement.native_profit)
                                                : ((transition.settlement.valuation &&
                                                    transition.settlement.valuation->fresh)
                                                       ? detail::convert_money(
                                                             transition.settlement.native_profit,
                                                             *transition.settlement.valuation,
                                                             transition.logical_currency)
                                                       : std::nullopt);
            const bool valuation_ready = physical_valuation.has_value();
            auto persisted = transition.settlement;
            persisted.logical_outcome_ready = valuation_ready;

            if (has_logical_projections &&
                (!transition.series || !transition.reservation || !transition.money_management)) {
                return fail({StorageResultKind::INVALID_ARGUMENT,
                             "logical settlement requires all logical projections"});
            }
            if (has_logical_projections && !group_terminal) {
                return fail({StorageResultKind::INVALID_STATE,
                             "physical settlement does not complete execution group"});
            }
            if (has_logical_projections && !valuation_ready) {
                return fail({StorageResultKind::INVALID_ARGUMENT,
                             "logical settlement requires deterministic valuation"});
            }
            if (has_logical_projections) {
                const auto normalized = detail::normalize_group_outcome(
                    aggregate.outcome(), transition.settlement.valuation ?
                                             &*transition.settlement.valuation : nullptr,
                    transition.logical_currency);
                if (!normalized || !transition.series ||
                    !detail::same_group_outcome(*normalized, transition.series->current_outcome)) {
                    return fail({StorageResultKind::CONFLICT,
                                 "logical outcome does not match authoritative group aggregate"});
                }
                const auto bindings = validate_terminal_bindings(transition);
                if (bindings.kind != StorageResultKind::APPLIED) return bindings;
            }

            const auto existing = m_storage.m_settlements.find(
                std::to_string(transition.settlement.settlement.settlement_id), m_transaction);
            if (existing) {
                const auto encoded = detail::settlement_record_to_json(persisted).dump();
                if (*existing == encoded) return {StorageResultKind::DUPLICATE, {}};
                return fail({StorageResultKind::CONFLICT, "settlement payload conflict"});
            }

            const auto journal_result = append_journal(transition.journal);
            if (journal_result.kind != StorageResultKind::APPLIED) return journal_result;
            const auto settlement_result = put_settlement(persisted);
            if (settlement_result.kind != StorageResultKind::APPLIED) return settlement_result;

            const auto physical_result = put_physical_trade(
                *transition.physical_trade, transition.expected_physical_trade_revision);
            if (physical_result.kind != StorageResultKind::APPLIED) return physical_result;

            if (!has_logical_projections) {
                return valuation_ready
                           ? StorageResult{StorageResultKind::APPLIED, {}}
                           : StorageResult{StorageResultKind::PENDING_VALUATION,
                                           "native settlement stored; valuation is missing or stale"};
            }

            if (transition.series) {
                const auto result = put_series(*transition.series, transition.expected_series_revision);
                if (result.kind != StorageResultKind::APPLIED) return result;
            }
            if (transition.reservation) {
                const auto result = put_reservation(*transition.reservation,
                                                    transition.expected_reservation_revision);
                if (result.kind != StorageResultKind::APPLIED) return result;
            }
            if (!transition.group_reservations.empty()) {
                if (transition.group_reservations.size() !=
                    transition.expected_group_reservation_revisions.size()) {
                    return fail({StorageResultKind::INVALID_ARGUMENT,
                                 "group reservation revision list is incomplete"});
                }
                for (std::size_t index = 0; index < transition.group_reservations.size(); ++index) {
                    const auto result = put_reservation(
                        transition.group_reservations[index],
                        transition.expected_group_reservation_revisions[index]);
                    if (result.kind != StorageResultKind::APPLIED) return result;
                }
            }
            if (transition.money_management) {
                const auto result = put_money_management(*transition.money_management,
                                                         transition.expected_money_management_revision);
                if (result.kind != StorageResultKind::APPLIED) return result;
            }
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    /// \brief Resolves a previously persisted native settlement without rewriting it.
    ///
    /// The valuation resolution is a separate immutable fact. Logical
    /// projections are committed in the same transaction as that fact.
    StorageResult resolve_pending_valuation_atomic(const ValuationTransition& transition) {
        if (transition.settlement_id == 0 || transition.journal.event_id == 0 ||
            transition.journal.idempotency_key.empty() || transition.logical_currency == CurrencyType::UNKNOWN ||
            !transition.series || !transition.physical_trade || !transition.reservation ||
            !transition.money_management || !transition.valuation.valid() ||
            !transition.valuation.fresh) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "invalid valuation transition"});
        }
        try {
            const auto native_encoded = m_storage.m_settlements.find(
                std::to_string(transition.settlement_id), m_transaction);
            if (!native_encoded) return fail({StorageResultKind::NOT_FOUND, "native settlement is missing"});
            const auto native = detail::settlement_record_from_json(nlohmann::json::parse(*native_encoded));
            if (native.logical_outcome_ready ||
                native.native_profit.currency == transition.logical_currency ||
                transition.valuation.source != native.native_profit.currency ||
                transition.valuation.target != transition.logical_currency) {
                return fail({StorageResultKind::INVALID_STATE, "settlement is not pending valuation"});
            }
            const auto physical_binding = validate_physical_bindings(
                SettlementTransition{transition.journal, native, std::nullopt,
                                      transition.physical_trade, transition.reservation,
                                      std::nullopt, 0, 0, 0, 0, transition.logical_currency});
            if (physical_binding.kind != StorageResultKind::APPLIED) return physical_binding;

            ExecutionGroupAggregator aggregate;
            const auto aggregate_result = build_group_aggregate(
                native.settlement.execution_group_id, nullptr, aggregate);
            if (aggregate_result.kind != StorageResultKind::APPLIED) return aggregate_result;
            if (!aggregate.outcome().final || !aggregate.outcome().all_members_terminal ||
                aggregate.outcome().aggregation_status != AggregationStatus::READY) {
                return fail({StorageResultKind::INVALID_STATE,
                             "valuation cannot advance a non-terminal execution group"});
            }
            const auto logical_profit = detail::convert_money(
                native.native_profit, transition.valuation, transition.logical_currency);
            const auto normalized = detail::normalize_group_outcome(
                aggregate.outcome(), &transition.valuation, transition.logical_currency);
            if (!logical_profit || !normalized || !transition.series ||
                !detail::same_group_outcome(*normalized, transition.series->current_outcome)) {
                return fail({StorageResultKind::INVALID_ARGUMENT,
                             "valuation cannot deterministically reproduce logical outcome"});
            }
            ValuationResolutionRecord resolution{
                transition.settlement_id, transition.valuation, *logical_profit,
                transition.logical_currency, 0, transition.journal.idempotency_key,
                trading_storage_schema_version};
            const auto existing = m_storage.m_valuation_resolutions.find(
                std::to_string(transition.settlement_id), m_transaction);
            if (existing) {
                if (*existing == detail::valuation_resolution_to_json(resolution).dump()) {
                    return {StorageResultKind::DUPLICATE, {}};
                }
                return fail({StorageResultKind::CONFLICT, "valuation resolution payload conflict"});
            }

            SettlementTransition bindings;
            bindings.settlement = native;
            bindings.series = transition.series;
            bindings.physical_trade = transition.physical_trade;
            bindings.reservation = transition.reservation;
            bindings.money_management = transition.money_management;
            bindings.logical_currency = transition.logical_currency;
            bindings.group_reservations = transition.group_reservations;
            bindings.expected_group_reservation_revisions =
                transition.expected_group_reservation_revisions;
            const auto binding_result = validate_terminal_bindings(bindings);
            if (binding_result.kind != StorageResultKind::APPLIED) return binding_result;

            const auto journal_result = append_journal(transition.journal);
            if (journal_result.kind != StorageResultKind::APPLIED) return journal_result;
            const auto resolution_result = put_valuation_resolution(resolution);
            if (resolution_result.kind != StorageResultKind::APPLIED) return resolution_result;
            if (auto result = put_series(*transition.series, transition.expected_series_revision);
                result.kind != StorageResultKind::APPLIED) return result;
            if (auto result = put_physical_trade(*transition.physical_trade,
                                                 transition.expected_physical_trade_revision);
                result.kind != StorageResultKind::APPLIED) return result;
            if (auto result = put_reservation(*transition.reservation,
                                               transition.expected_reservation_revision);
                result.kind != StorageResultKind::APPLIED) return result;
            if (!transition.group_reservations.empty()) {
                if (transition.group_reservations.size() !=
                    transition.expected_group_reservation_revisions.size()) {
                    return fail({StorageResultKind::INVALID_ARGUMENT,
                                 "group reservation revision list is incomplete"});
                }
                for (std::size_t index = 0; index < transition.group_reservations.size(); ++index) {
                    if (auto result = put_reservation(
                            transition.group_reservations[index],
                            transition.expected_group_reservation_revisions[index]);
                        result.kind != StorageResultKind::APPLIED) return result;
                }
            }
            if (auto result = put_money_management(*transition.money_management,
                                                   transition.expected_money_management_revision);
                result.kind != StorageResultKind::APPLIED) return result;
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    StorageResult commit() {
        if (m_closed) return {StorageResultKind::INVALID_STATE, "unit of work is closed"};
        if (m_failed) {
            rollback();
            return {StorageResultKind::INVALID_STATE, "unit of work was aborted after an operation failure"};
        }
        try {
            m_transaction.commit();
            m_closed = true;
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            m_failed = true;
            rollback();
            return {StorageResultKind::STORAGE_ERROR, error.what()};
        }
    }

    void rollback() noexcept {
        if (m_closed) return;
        try { m_transaction.rollback(); } catch (...) {}
        m_closed = true;
    }

private:
    StorageResult persist_scope_gate(
        std::uint64_t risk_scope_id, std::uint64_t related_scope_id,
        const std::string& conflict_id) {
        RiskScopeGateRecord value{risk_scope_id, related_scope_id,
                                  RiskScopeGateState::BLOCKED, conflict_id, 0,
                                  trading_storage_schema_version};
        try {
            const auto key = std::to_string(risk_scope_id);
            const auto existing = m_storage.m_risk_scope_gates.find(key, m_transaction);
            if (existing) {
                const auto current = detail::risk_scope_gate_from_json(
                    nlohmann::json::parse(*existing));
                if (current.valid() && current.conflict_id == conflict_id) {
                    return {StorageResultKind::DUPLICATE, {}};
                }
                return fail({StorageResultKind::CONFLICT,
                             "risk scope already has another durable conflict gate"});
            }
            m_storage.m_risk_scope_gates.insert(
                key, detail::risk_scope_gate_to_json(value).dump(), m_transaction);
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    StorageResult validate_physical_bindings(const SettlementTransition& transition) {
        if (!transition.physical_trade) {
            return fail({StorageResultKind::INVALID_ARGUMENT, "physical trade projection is required"});
        }
        const auto roster_encoded = m_storage.m_rosters.find(
            std::to_string(transition.settlement.settlement.execution_group_id), m_transaction);
        if (!roster_encoded) {
            return fail({StorageResultKind::INVALID_STATE, "execution roster is missing"});
        }
        try {
            const auto roster = detail::roster_from_json(nlohmann::json::parse(*roster_encoded));
            if (!roster.valid() || roster.intent_id != transition.settlement.settlement.intent_id) {
                return fail({StorageResultKind::CONFLICT, "settlement does not match execution roster"});
            }
            const auto member = std::find_if(
                roster.members.begin(), roster.members.end(),
                [&transition](const RosterMemberRecord& value) {
                    return value.trade_id == transition.settlement.settlement.trade_id;
                });
            if (member == roster.members.end()) {
                return fail({StorageResultKind::CONFLICT, "settlement trade is not in execution roster"});
            }
            const auto& physical = *transition.physical_trade;
            if (physical.trade_id != member->trade_id || physical.intent_id != roster.intent_id ||
                physical.group_id != roster.group_id || physical.account_id != member->account_id ||
                physical.risk_scope_id != member->risk_scope_id) {
                return fail({StorageResultKind::CONFLICT, "physical trade binding conflicts with roster"});
            }
            if (transition.reservation) {
                const auto& reservation = *transition.reservation;
                if (reservation.intent_id != roster.intent_id ||
                    reservation.risk_scope_id != member->risk_scope_id) {
                    return fail({StorageResultKind::CONFLICT, "reservation binding conflicts with roster"});
                }
            }
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    StorageResult build_group_aggregate(
        ExecutionGroupId group_id, const SettlementRecord* incoming,
        ExecutionGroupAggregator& aggregate) {
        const auto roster_encoded = m_storage.m_rosters.find(
            std::to_string(group_id), m_transaction);
        if (!roster_encoded) {
            return fail({StorageResultKind::INVALID_STATE, "execution roster is missing"});
        }
        try {
            const auto roster = detail::roster_from_json(nlohmann::json::parse(*roster_encoded));
            if (!roster.valid()) return fail({StorageResultKind::STORAGE_ERROR, "invalid execution roster"});

            std::vector<std::pair<std::string, std::string>> encoded;
            m_storage.m_settlements.load(encoded, m_transaction);
            std::vector<SettlementRecord> records;
            std::set<SettlementId> settlement_ids;
            CurrencyType currency = incoming ? incoming->native_profit.currency : CurrencyType::UNKNOWN;
            std::uint8_t precision = incoming ? incoming->native_profit.precision : 0;
            for (const auto& entry : encoded) {
                const auto record = detail::settlement_record_from_json(
                    nlohmann::json::parse(entry.second));
                if (!record.valid()) return fail({StorageResultKind::STORAGE_ERROR, "invalid settlement record"});
                if (record.settlement.execution_group_id != group_id) continue;
                if (record.settlement.intent_id != roster.intent_id) {
                    return fail({StorageResultKind::CONFLICT, "settlement intent conflicts with roster"});
                }
                if (currency == CurrencyType::UNKNOWN) {
                    currency = record.native_profit.currency;
                    precision = record.native_profit.precision;
                }
                if (record.native_profit.currency != currency ||
                    record.native_profit.precision != precision) {
                    return fail({StorageResultKind::CONFLICT, "group settlements use different native money metadata"});
                }
                records.push_back(record);
                settlement_ids.insert(record.settlement.settlement_id);
            }
            if (incoming && currency == CurrencyType::UNKNOWN) {
                currency = incoming->native_profit.currency;
                precision = incoming->native_profit.precision;
            }
            if (currency == CurrencyType::UNKNOWN) {
                return fail({StorageResultKind::INVALID_ARGUMENT, "group currency is unknown"});
            }
            std::vector<std::uint64_t> trade_ids;
            trade_ids.reserve(roster.members.size());
            for (const auto& member : roster.members) trade_ids.push_back(member.trade_id);
            aggregate = ExecutionGroupAggregator(
                roster.intent_id, {roster.group_id, roster.expected_members},
                trade_ids, currency, precision);
            if (!aggregate.valid()) {
                return fail({StorageResultKind::INVALID_STATE, "execution group roster is not aggregatable"});
            }
            for (const auto& record : records) {
                if (aggregate.apply(record.settlement) != ProgressionResult::APPLIED) {
                    return fail({StorageResultKind::CONFLICT, "settlement conflicts with execution group aggregate"});
                }
            }
            if (incoming && !settlement_ids.count(incoming->settlement.settlement_id)) {
                if (aggregate.apply(incoming->settlement) != ProgressionResult::APPLIED) {
                    return fail({StorageResultKind::CONFLICT, "incoming settlement conflicts with execution group aggregate"});
                }
            }
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    StorageResult validate_terminal_bindings(const SettlementTransition& transition) {
        const auto physical = validate_physical_bindings(transition);
        if (physical.kind != StorageResultKind::APPLIED) return physical;
        const auto roster_encoded = m_storage.m_rosters.find(
            std::to_string(transition.settlement.settlement.execution_group_id), m_transaction);
        if (!roster_encoded) return fail({StorageResultKind::INVALID_STATE, "execution roster is missing"});
        try {
            const auto roster = detail::roster_from_json(nlohmann::json::parse(*roster_encoded));
            const auto member = std::find_if(
                roster.members.begin(), roster.members.end(),
                [&transition](const RosterMemberRecord& value) {
                    return value.trade_id == transition.settlement.settlement.trade_id;
                });
            const auto& reservation = *transition.reservation;
            if (reservation.intent_id != roster.intent_id ||
                reservation.risk_scope_id != member->risk_scope_id) {
                return fail({StorageResultKind::CONFLICT, "reservation binding conflicts with roster"});
            }
            const auto expected_reservation_state =
                transition.settlement.settlement.kind == OutcomeKind::REJECTED ||
                transition.settlement.settlement.kind == OutcomeKind::CANCELLED
                    ? RiskReservationState::RELEASED : RiskReservationState::SETTLED;
            if (reservation.state != expected_reservation_state) {
                return fail({StorageResultKind::INVALID_STATE, "reservation state is not terminal"});
            }
            std::set<std::uint64_t> reservation_ids{reservation.reservation_id};
            std::set<std::uint64_t> covered_scopes{reservation.risk_scope_id};
            std::set<std::uint64_t> roster_scopes;
            for (const auto& roster_member : roster.members) {
                roster_scopes.insert(roster_member.risk_scope_id);
            }
            for (const auto& group_reservation : transition.group_reservations) {
                if (!group_reservation.valid() ||
                    !reservation_ids.insert(group_reservation.reservation_id).second ||
                    group_reservation.intent_id != roster.intent_id ||
                    !roster_scopes.count(group_reservation.risk_scope_id) ||
                    group_reservation.state != expected_reservation_state) {
                    return fail({StorageResultKind::CONFLICT,
                                 "group reservation roster binding is invalid"});
                }
                covered_scopes.insert(group_reservation.risk_scope_id);
            }
            for (const auto& roster_member : roster.members) {
                if (!covered_scopes.count(roster_member.risk_scope_id)) {
                    return fail({StorageResultKind::INVALID_STATE,
                                 "logical completion has not settled every risk scope"});
                }
            }
            if (transition.series->current_group_id != 0 &&
                transition.series->current_group_id != roster.group_id) {
                return fail({StorageResultKind::CONFLICT, "series group conflicts with roster"});
            }
            if (transition.money_management->last_settlement_id !=
                transition.settlement.settlement.settlement_id) {
                return fail({StorageResultKind::CONFLICT, "money-management settlement binding is invalid"});
            }
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    template<class Table>
    StorageResult put_immutable(Table& table, const std::string& key, const std::string& encoded) {
        try {
            const auto existing = table.find(key, m_transaction);
            if (existing) {
                return *existing == encoded ? StorageResult{StorageResultKind::DUPLICATE, {}}
                                            : fail({StorageResultKind::CONFLICT, "immutable record conflict"});
            }
            table.insert(key, encoded, m_transaction);
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    template<class Table>
    StorageResult put_revisioned(Table& table, const std::string& key, const std::string& encoded,
                                 std::uint64_t new_revision, std::uint64_t expected_revision,
                                 const char* revision_field = "state_revision") {
        try {
            const auto existing = table.find(key, m_transaction);
            if (existing) {
                nlohmann::json current = nlohmann::json::parse(*existing);
                const auto current_revision = current.value(revision_field, std::uint64_t{0});
                if (current_revision == new_revision && *existing == encoded) {
                    return {StorageResultKind::DUPLICATE, {}};
                }
                if (current_revision != expected_revision) {
                    return fail({StorageResultKind::REVISION_CONFLICT, "stale durable revision"});
                }
                if (expected_revision == (std::numeric_limits<std::uint64_t>::max)() ||
                    new_revision != expected_revision + 1) {
                    return fail({StorageResultKind::INVALID_ARGUMENT, "revision must advance by one"});
                }
            } else if (expected_revision != 0 || new_revision != 0) {
                return fail({StorageResultKind::REVISION_CONFLICT, "missing durable record"});
            }
            table.insert_or_assign(key, encoded, m_transaction);
            return {StorageResultKind::APPLIED, {}};
        } catch (const std::exception& error) {
            return fail({StorageResultKind::STORAGE_ERROR, error.what()});
        }
    }

    static bool valid_schema(std::uint32_t value) noexcept {
        return value == trading_storage_schema_version;
    }

    StorageResult fail(StorageResult result) {
        m_failed = true;
        return result;
    }

    SequenceResult fail_sequence(const std::string& message) {
        m_failed = true;
        return {StorageResultKind::INVALID_ARGUMENT, 0, message};
    }

    TradingSystemStorage& m_storage;
    mdbxc::Transaction m_transaction;
    bool m_failed = false;
    bool m_closed = false;
};

inline TradingSystemStorage::UnitOfWork TradingSystemStorage::begin() {
    return UnitOfWork(*this);
}

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_STORAGE_HPP_INCLUDED
