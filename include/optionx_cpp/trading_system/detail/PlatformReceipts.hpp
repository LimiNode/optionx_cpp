#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_PLATFORM_RECEIPTS_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_PLATFORM_RECEIPTS_HPP_INCLUDED

/// \file PlatformReceipts.hpp
/// \brief Internal bound receipt application in the existing execution owner.

#include <cstring>

namespace optionx::trading_system::detail::platform_receipts {
namespace pm = platform_mapping;

/// \brief Copyable capability restored only from the owner's immutable journal binding.
class Binding {
public:
    Binding(const Binding&) = default;
    Binding(Binding&&) = default;
    Binding& operator=(const Binding&) = default;
    const nlohmann::json& document() const noexcept { return m_document; }
    std::uint64_t event_id() const noexcept { return m_event_id; }
    std::string operation_id() const { return m_document.at("operation").at("operation_id"); }
    TradeRequest request() const { return m_document.at("request").get<TradeRequest>(); }

private:
    friend class optionx::trading_system::ExecutionRecoveryManager;
    Binding(std::uint64_t event_id, nlohmann::json document)
        : m_event_id(event_id), m_document(std::move(document)) {}
    std::uint64_t m_event_id;
    nlohmann::json m_document;
};

struct BindingResult {
    StorageResultKind kind = StorageResultKind::INVALID_STATE;
    std::optional<Binding> binding;
    std::string message;
    bool succeeded() const noexcept { return binding.has_value(); }
};

struct ApplyResult {
    StorageResultKind kind = StorageResultKind::INVALID_STATE;
    std::string message;
    std::optional<ExecutionOperationRecord> operation;
    SettlementId settlement_id = 0;
    std::size_t journal_point_reads = 0;
    std::size_t terminal_index_visits = 0;
    bool succeeded() const noexcept {
        return kind == StorageResultKind::APPLIED || kind == StorageResultKind::DUPLICATE;
    }
};

inline std::uint64_t binary64_bits(double value) noexcept {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

inline std::string binding_key(const std::string& operation_id) {
    return "platform-binding:" + operation_id;
}

inline std::string receipt_key(const std::string& operation_id, const std::string& receipt_id) {
    return "platform-receipt:" + nlohmann::json::array({operation_id, receipt_id}).dump();
}

inline nlohmann::json account_identity(const AccountIdentityRecord& value) {
    auto result = detail::account_to_json(value);
    result.erase("active");
    result.erase("identity_revision");
    return result;
}

inline bool version_one(const nlohmann::json& value) {
    return value.is_number_integer() && value == 1;
}

inline pm::MoneyProfile read_profile(const nlohmann::json& value) {
    const auto& currency = value.at("currency");
    const auto& precision = value.at("precision");
    if (!currency.is_number_integer() || currency < 0 || currency > (std::numeric_limits<int>::max)() ||
        !precision.is_number_integer() || precision < 0 || precision > detail::max_money_precision) {
        throw std::invalid_argument("invalid platform money profile encoding");
    }
    const pm::MoneyProfile result{static_cast<CurrencyType>(currency.get<int>()), precision.get<int>()};
    if (pm::profile_error(result) != pm::MappingError::NONE) {
        throw std::invalid_argument("unsupported platform money profile");
    }
    return result;
}

inline pm::MoneyProfile profile(const Binding& binding) {
    return read_profile(binding.document());
}

inline void validate_binding(const nlohmann::json& value) {
    if (!version_one(value.at("version")) || value.at("normalization") != pm::money_normalization_rule) {
        throw std::invalid_argument("unsupported platform binding version/normalization");
    }
    const auto operation = value.at("operation").get<ExecutionOperationRecord>();
    const auto account = detail::account_from_json(value.at("account"));
    const auto request = value.at("request").get<TradeRequest>();
    const auto money = read_profile(value);
    if (operation.state != ExecutionOperationState::PREPARED || operation.state_revision != 0 ||
        !operation.broker_trade_id.empty() || pm::binding_error(operation, account) != pm::MappingError::NONE ||
        pm::profile_error(money) != pm::MappingError::NONE || request.trade_id != operation.trade_id ||
        request.account_id != static_cast<std::int64_t>(operation.account_id) ||
        request.account_type != pm::account_realm(account) || request.currency != money.currency ||
        !std::isfinite(request.amount) || request.amount <= 0 ||
        value.at("request_amount_bits") != binary64_bits(request.amount)) {
        throw std::invalid_argument("invalid immutable platform binding");
    }
}

inline bool opening_confirmed(ExecutionOperationState state) noexcept {
    return state == ExecutionOperationState::BROKER_ACCEPTED ||
           state == ExecutionOperationState::BROKER_ID_PERSISTED ||
           state == ExecutionOperationState::EXECUTED || state == ExecutionOperationState::SETTLED;
}

inline bool terminal(ExecutionOperationState state) noexcept {
    return state == ExecutionOperationState::SETTLED || state == ExecutionOperationState::REJECTED ||
           state == ExecutionOperationState::CANCELLED;
}

inline std::string broker_key(const Binding& binding, const std::string& broker_id) {
    const auto account = detail::account_from_json(binding.document().at("account"));
    return "platform-broker:" + nlohmann::json::array(
        {"INTRADE_BAR", account.realm, account.risk_scope_id, broker_id}).dump();
}

inline StorageResult append(TradingSystemStorage::UnitOfWork& unit, const std::string& type,
                            const std::string& key, const std::string& aggregate,
                            std::uint64_t revision, std::int64_t now, const nlohmann::json& payload) {
    const auto id = unit.next_journal_event_id_for_append();
    if (id.kind != StorageResultKind::APPLIED) return {id.kind, id.message};
    return unit.append_journal({id.value, type, aggregate, revision,
                                trading_storage_schema_version, key, now, payload});
}

// Both reads share the write snapshot. Legitimate owner chains are contiguous;
// startup recovery audits the chain, while ordinary receipts never reload it.
inline StorageResult current_operation(TradingSystemStorage::UnitOfWork& unit,
                                      const Binding& binding, std::uint64_t revision,
                                      JournalEvent& event, ExecutionOperationRecord& operation) {
    const auto saved = unit.journal_by_key(binding_key(binding.operation_id()));
    if (!saved.found()) return {saved.kind, saved.message};
    if (saved.value->event_type != "PLATFORM_BINDING" || saved.value->event_id != binding.event_id() ||
        saved.value->payload != binding.document()) {
        return {StorageResultKind::CONFLICT, "immutable binding capability mismatch"};
    }
    validate_binding(saved.value->payload);
    auto identity = binding.document().at("operation").get<ExecutionOperationRecord>();
    identity.state_revision = revision;
    const auto found = unit.journal_by_key(ExecutionRecoveryManager::operation_key(identity));
    if (!found.found()) return {found.kind, found.message};
    event = *found.value;
    operation = event.payload.get<ExecutionOperationRecord>();
    if (!operation.valid() || operation.state_revision != revision ||
        event.event_type != "EXECUTION_OPERATION" || event.state_revision != revision ||
        event.aggregate_id != "operation:" + identity.operation_id ||
        event.idempotency_key != ExecutionRecoveryManager::operation_key(identity) ||
        operation.operation_id != identity.operation_id || operation.client_operation_id != identity.client_operation_id ||
        operation.trade_id != identity.trade_id || operation.account_id != identity.account_id ||
        operation.intent_id != identity.intent_id || operation.group_id != identity.group_id ||
        operation.risk_scope_id != identity.risk_scope_id ||
        (revision != 0 && event.event_id <= binding.event_id())) {
        return {StorageResultKind::CONFLICT, "operation differs from captured identity"};
    }
    if (event.payload.contains("platform_receipt_version") && !version_one(event.payload.at("platform_receipt_version"))) {
        return {StorageResultKind::UNSUPPORTED_SCHEMA, "unsupported platform operation extension"};
    }
    if (revision == (std::numeric_limits<std::uint64_t>::max)()) {
        return {StorageResultKind::INVALID_STATE, "operation revision exhausted"};
    }
    ++identity.state_revision;
    const auto next = unit.journal_by_key(ExecutionRecoveryManager::operation_key(identity));
    if (next.found()) return {StorageResultKind::CONFLICT, "stale operation revision"};
    if (next.kind != StorageResultKind::NOT_FOUND) return {next.kind, next.message};
    const auto captured = detail::account_from_json(binding.document().at("account"));
    const auto account = unit.account(operation.account_id);
    if (!account.found()) return {account.kind, account.message};
    if (account_identity(captured) != account_identity(*account.value) ||
        account.value->identity_revision < captured.identity_revision) {
        return {StorageResultKind::CONFLICT, "account identity changed after capture"};
    }
    return {StorageResultKind::APPLIED, {}};
}

inline nlohmann::json input_facts(const Binding& binding, const TradeResult& result) {
    return {{"binding_event_id", binding.event_id()}, {"raw", nlohmann::json(result)},
            {"normalization", pm::money_normalization_rule},
            {"currency", binding.document().at("currency")}, {"precision", binding.document().at("precision")},
            {"request_amount_bits", binding.document().at("request_amount_bits")},
            {"result_amount_bits", binary64_bits(result.amount)}, {"profit_bits", binary64_bits(result.profit)}};
}

inline void validate_receipt(const JournalEvent& event) {
    if (event.event_type != "PLATFORM_RECEIPT" || !version_one(event.payload.at("version")) ||
        event.payload.at("input").at("normalization") != pm::money_normalization_rule) {
        throw std::invalid_argument("unsupported platform receipt version/normalization");
    }
    const auto money = read_profile(event.payload.at("input"));
    const auto& input = event.payload.at("input");
    for (const auto* field : {"binding_event_id", "request_amount_bits", "result_amount_bits", "profit_bits"}) {
        if (!input.at(field).is_number_unsigned()) throw std::invalid_argument("invalid receipt numeric provenance");
    }
    const auto operation = event.payload.at("operation").get<ExecutionOperationRecord>();
    if (!operation.valid() || event.aggregate_id != "operation:" + operation.operation_id ||
        event.state_revision != operation.state_revision || !event.payload.at("settlement_id").is_number_unsigned()) {
        throw std::invalid_argument("invalid receipt operation projection");
    }
    const bool has_settlement = event.payload.at("settlement_id") != 0;
    const bool has_profit = !event.payload.at("normalized_profit").is_null();
    if (terminal(operation.state) != has_settlement ||
        (operation.state == ExecutionOperationState::SETTLED) != has_profit ||
        operation.state == ExecutionOperationState::CANCELLED) {
        throw std::invalid_argument("receipt terminality differs from its settlement projection");
    }
    if (!event.payload.at("normalized_profit").is_null()) {
        const auto& encoded = event.payload.at("normalized_profit");
        if (!encoded.at("units").is_number_integer() ||
            encoded.at("units") > (std::numeric_limits<std::int64_t>::max)() ||
            !encoded.at("precision").is_number_integer() ||
            encoded.at("precision") < 0 || encoded.at("precision") > detail::max_money_precision) {
            throw std::invalid_argument("invalid stored financial encoding");
        }
        const auto net = detail::money_from_json(encoded);
        if (!net.valid_metadata() || net.currency != money.currency || net.precision != money.precision) {
            throw std::invalid_argument("invalid stored normalized financial fact");
        }
        if (operation.state != ExecutionOperationState::SETTLED || event.payload.at("settlement_id") == 0) {
            throw std::invalid_argument("financial receipt lacks terminal identity");
        }
    }
}

inline bool settlement_matches_receipt(const JournalEvent& event, const SettlementRecord& fact) {
    const auto operation = event.payload.at("operation").get<ExecutionOperationRecord>();
    const auto& settlement = fact.settlement;
    if (settlement.settlement_id != event.payload.at("settlement_id") ||
        settlement.trade_id != operation.trade_id || settlement.intent_id != operation.intent_id ||
        settlement.execution_group_id != operation.group_id) return false;
    const auto& normalized = event.payload.at("normalized_profit");
    return normalized.is_null()
        ? settlement.kind == OutcomeKind::REJECTED && settlement.evidence == ExecutionEvidence::NOT_EXECUTED_PROVEN
        : settlement.is_trading_result() && settlement.net_profit &&
          detail::money_to_json(*settlement.net_profit) == normalized;
}

inline bool receipt_matches_binding(const JournalEvent& event, const Binding& binding) {
    const auto operation = event.payload.at("operation").get<ExecutionOperationRecord>();
    const auto captured = binding.document().at("operation").get<ExecutionOperationRecord>();
    const auto& input = event.payload.at("input");
    return event.aggregate_id == "operation:" + captured.operation_id &&
        operation.operation_id == captured.operation_id && operation.client_operation_id == captured.client_operation_id &&
        operation.trade_id == captured.trade_id && operation.account_id == captured.account_id &&
        operation.intent_id == captured.intent_id && operation.group_id == captured.group_id &&
        operation.risk_scope_id == captured.risk_scope_id &&
        input.at("binding_event_id") == binding.event_id() &&
        input.at("currency") == binding.document().at("currency") &&
        input.at("precision") == binding.document().at("precision") &&
        input.at("request_amount_bits") == binding.document().at("request_amount_bits");
}

} // namespace optionx::trading_system::detail::platform_receipts

namespace optionx::trading_system {

inline detail::platform_receipts::BindingResult ExecutionRecoveryManager::capture_platform_binding(
    TradingSystemStorage& storage, const ExecutionOperationRecord& operation,
    CurrencyType currency, int precision) const {
    namespace pr = detail::platform_receipts;
    namespace pm = detail::platform_mapping;
    try {
        auto unit = storage.begin();
        const auto stored = unit.journal_by_key(operation_key(operation));
        const auto account = unit.account(operation.account_id);
        const auto intent = unit.intent(operation.intent_id);
        const auto roster = unit.roster(operation.group_id);
        if (!stored.found()) return {stored.kind, {}, stored.message};
        if (!account.found()) return {account.kind, {}, account.message};
        if (!intent.found()) return {intent.kind, {}, intent.message};
        if (!roster.found()) return {roster.kind, {}, roster.message};
        if (operation.state != ExecutionOperationState::PREPARED || operation.state_revision != 0 ||
            stored.value->event_type != "EXECUTION_OPERATION" || stored.value->payload != nlohmann::json(operation)) {
            return {StorageResultKind::CONFLICT, {}, "binding must capture the prepared operation"};
        }
        auto next = operation;
        ++next.state_revision;
        const auto submitted = unit.journal_by_key(operation_key(next));
        if (submitted.kind != StorageResultKind::NOT_FOUND) {
            return {submitted.found() ? StorageResultKind::INVALID_STATE : submitted.kind, {},
                    "binding must be durable before submission"};
        }
        const auto mapped = pm::map_request(*intent.value, *roster.value, operation, *account.value, {currency, precision});
        if (!mapped.succeeded()) return {StorageResultKind::INVALID_ARGUMENT, {}, "request mapping failed"};
        const auto reservation_binding = std::find_if(roster.value->reservation_bindings.begin(),
            roster.value->reservation_bindings.end(), [&](const auto& value) { return value.trade_id == operation.trade_id; });
        if (reservation_binding == roster.value->reservation_bindings.end()) {
            return {StorageResultKind::CONFLICT, {}, "member reservation is missing"};
        }
        const auto risk = unit.reservation(reservation_binding->reservation_id);
        const auto physical = unit.physical_trade(operation.trade_id);
        if (!physical.found() || physical.value->account_id != operation.account_id ||
            physical.value->risk_scope_id != operation.risk_scope_id ||
            physical.value->intent_id != operation.intent_id || physical.value->group_id != operation.group_id ||
            !risk.found() || risk.value->risk_scope_kind != RiskScopeKind::ACCOUNT ||
            risk.value->state != RiskReservationState::RESERVED ||
            !detail::reservation_matches_binding(*risk.value, *reservation_binding, *roster.value)) {
            return {StorageResultKind::INVALID_STATE, {}, "binding requires admitted account-scope risk"};
        }
        const nlohmann::json document = {{"version", 1}, {"operation", nlohmann::json(operation)},
            {"account", detail::account_to_json(*account.value)}, {"request", nlohmann::json(*mapped.value)},
            {"currency", static_cast<int>(currency)}, {"precision", precision},
            {"normalization", pm::money_normalization_rule}, {"request_amount_bits", pr::binary64_bits(mapped.value->amount)}};
        const auto key = pr::binding_key(operation.operation_id);
        const auto existing = unit.journal_by_key(key);
        if (existing.found()) {
            if (existing.value->event_type != "PLATFORM_BINDING" || existing.value->payload != document) {
                return {StorageResultKind::CONFLICT, {}, "binding is immutable"};
            }
            return {StorageResultKind::DUPLICATE, pr::Binding(existing.value->event_id, document), {}};
        }
        if (existing.kind != StorageResultKind::NOT_FOUND) return {existing.kind, {}, existing.message};
        const auto appended = pr::append(unit, "PLATFORM_BINDING", key, "operation:" + operation.operation_id,
                                         0, m_clock.now_ms(), document);
        if (!appended.succeeded()) return {appended.kind, {}, appended.message};
        const auto saved = unit.journal_by_key(key);
        if (!saved.found()) return {saved.kind, {}, saved.message};
        const auto committed = unit.commit();
        if (!committed.succeeded()) return {committed.kind, {}, committed.message};
        return {StorageResultKind::APPLIED, pr::Binding(saved.value->event_id, document), {}};
    } catch (const std::exception& error) {
        return {StorageResultKind::STORAGE_ERROR, {}, error.what()};
    }
}

inline detail::platform_receipts::BindingResult ExecutionRecoveryManager::restore_platform_binding(
    const TradingSystemStorage& storage, const std::string& operation_id) const {
    namespace pr = detail::platform_receipts;
    const auto saved = storage.journal_by_key(pr::binding_key(operation_id));
    if (!saved.found()) return {saved.kind, {}, saved.message};
    try {
        pr::validate_binding(saved.value->payload);
        if (saved.value->event_type != "PLATFORM_BINDING" || saved.value->aggregate_id != "operation:" + operation_id ||
            saved.value->payload.at("operation").at("operation_id") != operation_id) {
            return {StorageResultKind::CONFLICT, {}, "binding journal identity mismatch"};
        }
        return {StorageResultKind::APPLIED, pr::Binding(saved.value->event_id, saved.value->payload), {}};
    } catch (const std::exception& error) {
        return {StorageResultKind::UNSUPPORTED_SCHEMA, {}, error.what()};
    }
}

inline StorageReadResult<JournalEvent> ExecutionRecoveryManager::platform_receipt_fact(
    const TradingSystemStorage& storage, const detail::platform_receipts::Binding& binding,
    const std::string& receipt_id) const {
    namespace pr = detail::platform_receipts;
    const auto saved_binding = storage.journal_by_key(pr::binding_key(binding.operation_id()));
    if (!saved_binding.found()) return {saved_binding.kind, {}, saved_binding.message};
    const auto fact = storage.journal_by_key(pr::receipt_key(binding.operation_id(), receipt_id));
    if (!fact.found()) return fact;
    try {
        pr::validate_binding(saved_binding.value->payload);
        pr::validate_receipt(*fact.value);
        if (saved_binding.value->event_id != binding.event_id() || saved_binding.value->payload != binding.document() ||
            !pr::receipt_matches_binding(*fact.value, binding)) {
            return {StorageResultKind::CONFLICT, {}, "receipt provenance differs from immutable binding"};
        }
        const auto settlement_id = fact.value->payload.at("settlement_id").get<SettlementId>();
        if (settlement_id != 0) {
            const auto settlement = storage.settlement(settlement_id);
            if (!settlement.found()) return {settlement.kind, {}, settlement.message};
            if (!pr::settlement_matches_receipt(*fact.value, *settlement.value)) {
                return {StorageResultKind::CONFLICT, {}, "receipt differs from canonical settlement"};
            }
        }
        return fact;
    } catch (const std::exception& error) {
        return {StorageResultKind::UNSUPPORTED_SCHEMA, {}, error.what()};
    }
}

inline detail::platform_receipts::ApplyResult ExecutionRecoveryManager::apply_platform_receipt(
    TradingSystemStorage::UnitOfWork& unit, const detail::platform_receipts::Binding& binding,
    const TradeResult& raw, const std::string& receipt_id, std::uint64_t expected_revision) const {
    namespace pr = detail::platform_receipts;
    namespace pm = detail::platform_mapping;
    pr::ApplyResult output;
    auto finish = [&](StorageResult result) {
        output.kind = result.kind;
        output.message = result.message;
        output.journal_point_reads = unit.journal_point_reads();
        output.terminal_index_visits = unit.terminal_index_visits();
        if (!result.succeeded()) {
            unit.reject(result);
            output.operation.reset();
            output.settlement_id = 0;
        }
        return output;
    };
    try {
        if (receipt_id.empty()) return finish({StorageResultKind::INVALID_ARGUMENT, "receipt identity is required"});
        const auto durable_binding = unit.journal_by_key(pr::binding_key(binding.operation_id()));
        if (!durable_binding.found()) return finish({durable_binding.kind, durable_binding.message});
        if (durable_binding.value->event_type != "PLATFORM_BINDING" ||
            durable_binding.value->event_id != binding.event_id() || durable_binding.value->payload != binding.document()) {
            return finish({StorageResultKind::CONFLICT, "immutable binding capability mismatch"});
        }
        pr::validate_binding(binding.document());
        const auto key = pr::receipt_key(binding.operation_id(), receipt_id);
        const auto facts = pr::input_facts(binding, raw);
        const auto existing = unit.journal_by_key(key);
        if (existing.found()) {
            pr::validate_receipt(*existing.value);
            if (!pr::receipt_matches_binding(*existing.value, binding) || existing.value->payload.at("input") != facts) {
                return finish({StorageResultKind::CONFLICT, "receipt identity reused with different facts"});
            }
            output.settlement_id = existing.value->payload.at("settlement_id").get<SettlementId>();
            if (output.settlement_id != 0) {
                const auto canonical = unit.settlement(output.settlement_id);
                if (!canonical.found()) return finish({canonical.kind, canonical.message});
                if (!pr::settlement_matches_receipt(*existing.value, *canonical.value)) {
                    return finish({StorageResultKind::CONFLICT, "receipt differs from canonical settlement"});
                }
            }
            return finish({StorageResultKind::DUPLICATE, {}});
        }
        if (existing.kind != StorageResultKind::NOT_FOUND) return finish({existing.kind, existing.message});
        JournalEvent current_event;
        ExecutionOperationRecord current;
        const auto checked = pr::current_operation(unit, binding, expected_revision, current_event, current);
        if (!checked.succeeded()) return finish(checked);
        if (current.state == ExecutionOperationState::PREPARED) {
            return finish({StorageResultKind::INVALID_STATE, "submission boundary is not durable"});
        }
        const auto mapped = pm::map_observation(raw, binding.request(), current,
            detail::account_from_json(binding.document().at("account")), pr::profile(binding));
        if (mapped.error != pm::MappingError::NONE) {
            return finish({StorageResultKind::INVALID_ARGUMENT, "platform observation mapping failed"});
        }
        const bool financial = mapped.kind == pm::ObservationKind::FINANCIAL_RESULT;
        const bool failed_open = mapped.kind == pm::ObservationKind::FAILED_OPEN;
        if (!mapped.broker_trade_id.empty()) {
            const auto claim_key = pr::broker_key(binding, mapped.broker_trade_id);
            const nlohmann::json claim = {{"version", 1}, {"operation_id", current.operation_id},
                {"binding_event_id", binding.event_id()}, {"broker_id", mapped.broker_trade_id}};
            const auto prior = unit.journal_by_key(claim_key);
            if (prior.found()) {
                if (prior.value->event_type != "PLATFORM_BROKER_BINDING" || prior.value->payload != claim) {
                    return finish({StorageResultKind::CONFLICT, "broker execution belongs to another binding"});
                }
            } else if (prior.kind != StorageResultKind::NOT_FOUND) {
                return finish({prior.kind, prior.message});
            } else if (pr::opening_confirmed(current.state) && !current.broker_trade_id.empty()) {
                return finish({StorageResultKind::CONFLICT, "confirmed execution lacks its broker ownership claim"});
            } else if (mapped.proves_opening && !pr::terminal(current.state)) {
                const auto claimed = pr::append(unit, "PLATFORM_BROKER_BINDING", claim_key,
                    "operation:" + current.operation_id, 0, m_clock.now_ms(), claim);
                if (!claimed.succeeded()) return finish(claimed);
            }
        }
        output.operation = current;
        if (pr::terminal(current.state)) {
            if (!financial && !failed_open) return finish({StorageResultKind::DUPLICATE, {}});
            const auto id = current_event.payload.value("platform_settlement_id", SettlementId{0});
            const auto source = unit.journal_by_key(current_event.payload.at("platform_receipt_key").get<std::string>());
            if (!source.found()) return finish({source.kind, source.message});
            pr::validate_receipt(*source.value);
            const auto saved = unit.settlement(id);
            if (!saved.found()) return finish({saved.kind, saved.message});
            if (!pr::receipt_matches_binding(*source.value, binding) ||
                !pr::settlement_matches_receipt(*source.value, *saved.value)) {
                return finish({StorageResultKind::CONFLICT, "terminal receipt differs from canonical evidence"});
            }
            const auto& evidence = saved.value->settlement;
            if ((financial && evidence.is_trading_result() && evidence.kind == *mapped.outcome &&
                 evidence.net_profit && evidence.net_profit->units == mapped.net_profit->units &&
                 evidence.net_profit->currency == mapped.net_profit->currency &&
                 evidence.net_profit->precision == mapped.net_profit->precision) ||
                (failed_open && evidence.kind == OutcomeKind::REJECTED &&
                 evidence.evidence == ExecutionEvidence::NOT_EXECUTED_PROVEN)) {
                output.settlement_id = id;
                return finish({StorageResultKind::DUPLICATE, {}});
            }
            return finish({StorageResultKind::CONFLICT, "observation conflicts with terminal evidence"});
        }
        if (failed_open && pr::opening_confirmed(current.state)) {
            return finish({StorageResultKind::CONFLICT, "failed opening contradicts prior acceptance"});
        }
        const auto physical = unit.physical_trade(current.trade_id);
        if (!physical.found()) return finish({physical.kind, physical.message});
        if (physical.value->account_id != current.account_id || physical.value->group_id != current.group_id ||
            physical.value->intent_id != current.intent_id || physical.value->risk_scope_id != current.risk_scope_id ||
            (!physical.value->broker_trade_id.empty() && physical.value->broker_trade_id != mapped.broker_trade_id)) {
            return finish({StorageResultKind::CONFLICT, "physical identity differs from operation"});
        }
        auto advance = [&](ExecutionOperationState state, const std::string& broker, SettlementId settlement_id) -> StorageResult {
            if (!transition_allowed(current.state, state) || !next_revision_of(current.state_revision, current.state_revision)) {
                return {StorageResultKind::INVALID_STATE, "invalid platform operation transition"};
            }
            current.state = state;
            current.broker_trade_id = broker;
            current.occurred_at_ms = m_clock.now_ms();
            nlohmann::json payload = current;
            payload["platform_receipt_version"] = 1;
            payload["platform_receipt_key"] = key;
            payload["platform_settlement_id"] = settlement_id;
            return pr::append(unit, "EXECUTION_OPERATION", operation_key(current),
                "operation:" + current.operation_id, current.state_revision, m_clock.now_ms(), payload);
        };
        bool physical_changed = false;
        if (mapped.proves_opening) {
            const auto target = mapped.broker_trade_id.empty() ? ExecutionOperationState::BROKER_ACCEPTED
                                                              : ExecutionOperationState::BROKER_ID_PERSISTED;
            if (!pr::opening_confirmed(current.state) ||
                (current.broker_trade_id.empty() && !mapped.broker_trade_id.empty())) {
                const auto accepted = advance(target, mapped.broker_trade_id, 0);
                if (!accepted.succeeded()) return finish(accepted);
                physical_changed = true;
            }
        }
        auto updated = *physical.value;
        if (financial || failed_open) {
            const auto sequence = unit.next_id("settlement");
            if (sequence.kind != StorageResultKind::APPLIED) return finish({sequence.kind, sequence.message});
            output.settlement_id = sequence.value;
            TradeSettlement settlement{sequence.value, current.intent_id, current.group_id, current.trade_id,
                financial ? *mapped.outcome : OutcomeKind::REJECTED,
                financial ? ExecutionEvidence::EXECUTED : ExecutionEvidence::NOT_EXECUTED_PROVEN,
                financial ? mapped.net_profit : std::nullopt};
            const auto advanced = advance(financial ? ExecutionOperationState::SETTLED : ExecutionOperationState::REJECTED,
                                           current.broker_trade_id, sequence.value);
            if (!advanced.succeeded()) return finish(advanced);
            if (!next_revision_of(updated.state_revision, updated.state_revision)) {
                return finish({StorageResultKind::INVALID_STATE, "physical revision exhausted"});
            }
            updated.lifecycle_state = financial ? "SETTLED" : "REJECTED";
            updated.broker_trade_id = current.broker_trade_id;
            auto native = financial ? *mapped.net_profit :
                MoneyAmount{0, static_cast<std::uint8_t>(pr::profile(binding).precision), pr::profile(binding).currency};
            const auto event_id = unit.next_journal_event_id_for_append();
            if (event_id.kind != StorageResultKind::APPLIED) return finish({event_id.kind, event_id.message});
            SettlementTransition transition;
            transition.journal = {event_id.value, "EXECUTION_SETTLED", "trade:" + std::to_string(current.trade_id),
                0, trading_storage_schema_version, "settlement:" + std::to_string(sequence.value), m_clock.now_ms(),
                {{"platform_receipt_key", key}, {"version", 1}}};
            transition.settlement = {settlement, native, std::nullopt, true};
            transition.physical_trade = updated;
            transition.expected_physical_trade_revision = physical.value->state_revision;
            const auto recorded = unit.record_physical_settlement(transition);
            if (!recorded.succeeded()) return finish(recorded);
            if (failed_open) {
                const auto roster = unit.roster(current.group_id);
                if (!roster.found()) return finish({roster.kind, roster.message});
                const auto member = std::find_if(roster.value->reservation_bindings.begin(),
                    roster.value->reservation_bindings.end(), [&](const auto& value) { return value.trade_id == current.trade_id; });
                if (member == roster.value->reservation_bindings.end()) {
                    return finish({StorageResultKind::CONFLICT, "physical reservation binding is missing"});
                }
                const auto risk = unit.reservation(member->reservation_id);
                if (!risk.found()) return finish({risk.kind, risk.message});
                auto released = *risk.value;
                if (!next_revision_of(released.state_revision, released.state_revision)) {
                    return finish({StorageResultKind::INVALID_STATE, "reservation revision exhausted"});
                }
                released.state = RiskReservationState::RELEASED;
                const auto release = unit.put_reservation(released, risk.value->state_revision);
                if (!release.succeeded()) return finish(release);
            }
        } else if (physical_changed) {
            if (!next_revision_of(updated.state_revision, updated.state_revision)) {
                return finish({StorageResultKind::INVALID_STATE, "physical revision exhausted"});
            }
            updated.lifecycle_state = to_string(current.state);
            updated.broker_trade_id = current.broker_trade_id;
            const auto written = unit.put_physical_trade(updated, physical.value->state_revision);
            if (!written.succeeded()) return finish(written);
        }
        const auto receipt = pr::append(unit, "PLATFORM_RECEIPT", key, "operation:" + current.operation_id,
            current.state_revision, m_clock.now_ms(), {{"version", 1}, {"input", facts},
                {"operation", nlohmann::json(current)}, {"settlement_id", output.settlement_id},
                {"normalized_profit", mapped.net_profit ? detail::money_to_json(*mapped.net_profit) : nlohmann::json(nullptr)}});
        if (!receipt.succeeded()) return finish(receipt);
        output.operation = current;
        return finish({StorageResultKind::APPLIED, {}});
    } catch (const std::exception& error) {
        return finish({StorageResultKind::STORAGE_ERROR, error.what()});
    }
}

inline detail::platform_receipts::ApplyResult ExecutionRecoveryManager::apply_platform_receipt(
    TradingSystemStorage& storage, const detail::platform_receipts::Binding& binding,
    const TradeResult& result, const std::string& receipt_id, std::uint64_t expected_revision) const {
    auto unit = storage.begin();
    auto applied = apply_platform_receipt(unit, binding, result, receipt_id, expected_revision);
    if (!applied.succeeded()) return applied;
    const auto committed = unit.commit();
    if (!committed.succeeded()) {
        applied.kind = committed.kind;
        applied.message = committed.message;
        applied.operation.reset();
        applied.settlement_id = 0;
    }
    return applied;
}

inline StorageResult ExecutionRecoveryManager::request_known_result(
    TradingSystemStorage& storage, const detail::platform_receipts::Binding& binding,
    std::uint64_t expected_revision, platforms::BaseTradingApi& api,
    const AccountIdentityRecord& endpoint_account, std::function<void(TradeResult)> callback) const {
    namespace pr = detail::platform_receipts;
    TradeResultQuery query;
    std::unique_ptr<TradeResult> seed;
    try {
        {
            auto unit = storage.begin();
            JournalEvent event;
            ExecutionOperationRecord operation;
            const auto checked = pr::current_operation(unit, binding, expected_revision, event, operation);
            if (!checked.succeeded()) return checked;
            const auto captured = detail::account_from_json(binding.document().at("account"));
            if (!callback || pr::account_identity(endpoint_account) != pr::account_identity(captured) ||
                endpoint_account.identity_revision < captured.identity_revision ||
                !pr::opening_confirmed(operation.state) || pr::terminal(operation.state)) {
                return {StorageResultKind::INVALID_STATE, "result query requires a bound confirmed opening"};
            }
            const auto& id = operation.broker_trade_id;
            const auto parsed = std::from_chars(id.data(), id.data() + id.size(), query.option_id);
            if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || query.option_id <= 0 ||
                std::to_string(query.option_id) != id) {
                return {StorageResultKind::INVALID_STATE, "known numeric broker identity is required"};
            }
            const auto claim = unit.journal_by_key(pr::broker_key(binding, id));
            if (!claim.found() || claim.value->event_type != "PLATFORM_BROKER_BINDING" ||
                !pr::version_one(claim.value->payload.at("version")) ||
                claim.value->payload.at("binding_event_id") != binding.event_id() ||
                claim.value->payload.at("operation_id") != operation.operation_id) {
                return {StorageResultKind::CONFLICT, "broker identity is not owned by this binding"};
            }
            query.trade_id = static_cast<std::uint32_t>(operation.trade_id);
            seed = binding.request().create_trade_result_unique();
            seed->platform_type = PlatformType::INTRADE_BAR;
            seed->option_id = query.option_id;
        }
        // No transaction, owner pointer or platform lifetime is captured by the callback.
        const bool scheduled = api.fetch_trade_result(query, std::move(seed),
            [callback = std::move(callback)](std::unique_ptr<TradeResult> result) {
                if (result) callback(*result);
            });
        return scheduled ? StorageResult{StorageResultKind::APPLIED, {}}
                         : StorageResult{StorageResultKind::INVALID_STATE, "result query was not scheduled"};
    } catch (const std::exception& error) {
        return {StorageResultKind::STORAGE_ERROR, error.what()};
    }
}

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_PLATFORM_RECEIPTS_HPP_INCLUDED
