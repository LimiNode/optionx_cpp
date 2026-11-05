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
#include <limits>
#include <memory>
#include <optional>
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
    IntentId intent_id = 0;
    RiskReservationState state = RiskReservationState::RESERVED;
    std::uint64_t state_revision = 0;
    MoneyAmount potential_risk;
    std::string idempotency_key;
    std::uint32_t schema_version = trading_storage_schema_version;

    bool valid() const noexcept {
        return reservation_id != 0 && risk_scope_id != 0 && intent_id != 0 &&
               potential_risk.valid_metadata() && potential_risk.units > 0 &&
               !idempotency_key.empty();
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

/// \brief Complete durable checkpoint, including idempotency history.
struct SeriesCheckpoint {
    TradeSeries series;
    SeriesStatus status = SeriesStatus::ACTIVE;
    IntentId current_intent_id = 0;
    ExecutionGroupId current_group_id = 0;
    std::uint32_t current_step = 0;
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
        {"intent_id", value.intent_id}, {"state", static_cast<int>(value.state)},
        {"state_revision", value.state_revision}, {"potential_risk", money_to_json(value.potential_risk)},
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

inline ReservationRecord reservation_from_json(const nlohmann::json& value) {
    ReservationRecord result;
    result.reservation_id = value.value("reservation_id", std::uint64_t{0});
    result.risk_scope_id = value.value("risk_scope_id", std::uint64_t{0});
    result.intent_id = value.value("intent_id", IntentId{0});
    result.state = static_cast<RiskReservationState>(value.value("state", 0));
    result.state_revision = value.value("state_revision", std::uint64_t{0});
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

inline nlohmann::json checkpoint_to_json(const SeriesCheckpoint& value) {
    nlohmann::json receipts = nlohmann::json::array();
    for (const auto& receipt : value.receipts) receipts.push_back(settlement_to_json(receipt));
    return {{"series", value.series}, {"status", static_cast<int>(value.status)},
            {"current_intent_id", value.current_intent_id}, {"current_group_id", value.current_group_id},
            {"current_step", value.current_step}, {"receipts", std::move(receipts)},
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
          m_settlements(m_connection, "ts_settlements"),
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
                                          return value.get<TradeIntent>();
                                      });
    }

    StorageReadResult<SettlementRecord> settlement(SettlementId id) const {
        return read_json<SettlementRecord>(m_settlements, std::to_string(id), detail::settlement_record_from_json);
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
        return read_all_json<SettlementRecord>(
            m_settlements, detail::settlement_record_from_json,
            [](const SettlementRecord& value) { return !value.logical_outcome_ready; });
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
    mutable mdbxc::KeyValueTable<std::string, std::string> m_settlements;
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
            if (document.contains("schema_version") &&
                document.at("schema_version").get<std::uint32_t>() != trading_storage_schema_version) {
                return {StorageResultKind::UNSUPPORTED_SCHEMA, std::nullopt, "unsupported durable schema"};
            }
            auto parsed = parser(document);
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
                if (document.contains("schema_version") &&
                    document.at("schema_version").get<std::uint32_t>() !=
                        trading_storage_schema_version) {
                    return {StorageResultKind::UNSUPPORTED_SCHEMA, {},
                            "unsupported durable schema"};
                }
                auto parsed = parser(document);
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
        return put_immutable(m_storage.m_intents, std::to_string(intent.intent_id), nlohmann::json(intent).dump());
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
        return put_revisioned(m_storage.m_series, std::to_string(value.series.series_id),
                              detail::checkpoint_to_json(value).dump(), value.series.state_revision, expected_revision);
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

    /// \brief Applies a settlement and all supplied projections before commit.
    StorageResult apply_settlement_atomic(const SettlementTransition& transition) {
        if (!transition.settlement.valid() || transition.journal.event_id == 0 ||
            transition.journal.idempotency_key.empty()) {
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
            auto persisted = transition.settlement;
            const bool valuation_ready =
                transition.settlement.native_profit.currency == transition.logical_currency ||
                (transition.settlement.valuation && transition.settlement.valuation->fresh);
            if (!valuation_ready) persisted.logical_outcome_ready = false;

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

            if (!valuation_ready) {
                return {StorageResultKind::PENDING_VALUATION,
                        "native settlement stored; valuation is missing or stale"};
            }

            if (transition.series) {
                const auto result = put_series(*transition.series, transition.expected_series_revision);
                if (result.kind != StorageResultKind::APPLIED) return result;
            }
            if (transition.physical_trade) {
                const auto result = put_physical_trade(
                    *transition.physical_trade, transition.expected_physical_trade_revision);
                if (result.kind != StorageResultKind::APPLIED) return result;
            }
            if (transition.reservation) {
                const auto result = put_reservation(*transition.reservation,
                                                    transition.expected_reservation_revision);
                if (result.kind != StorageResultKind::APPLIED) return result;
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
                                 std::uint64_t new_revision, std::uint64_t expected_revision) {
        try {
            const auto existing = table.find(key, m_transaction);
            if (existing) {
                nlohmann::json current = nlohmann::json::parse(*existing);
                const auto current_revision = current.value("state_revision", std::uint64_t{0});
                if (current_revision == new_revision && *existing == encoded) {
                    return {StorageResultKind::DUPLICATE, {}};
                }
                if (current_revision != expected_revision) {
                    return fail({StorageResultKind::REVISION_CONFLICT, "stale durable revision"});
                }
                if (new_revision != expected_revision + 1) {
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
