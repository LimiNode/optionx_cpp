#pragma once
#ifndef OPTIONX_EXAMPLE_SYNTHETIC_HOST_HPP_INCLUDED
#define OPTIONX_EXAMPLE_SYNTHETIC_HOST_HPP_INCLUDED

/// \file SyntheticHost.hpp
/// \brief Single-threaded offline composition example, not a production runtime.

#include <optionx_cpp/trading_system.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace optionx_examples {
namespace ts = optionx::trading_system;

inline ts::MoneyAmount usd(std::int64_t units) {
    return {units, 2, optionx::CurrencyType::USD};
}

struct SyntheticTarget {
    std::uint64_t account_id;
    std::uint64_t risk_scope_id;
};

struct SyntheticConfig {
    ts::MoneyAmount base_stake = usd(100);
    bool loss_step = false;
    std::int64_t loss_multiplier = 2;
    std::vector<SyntheticTarget> targets{{11, 1000}};
    std::map<std::uint64_t, ts::RiskAccountSnapshot> accounts;
};

inline optionx::TradeSignal synthetic_buy() {
    optionx::TradeSignal signal;
    signal.signal_id = 7;
    signal.symbol = "EUR/USD";
    signal.order_type = optionx::OrderType::BUY;
    signal.currency = optionx::CurrencyType::USD;
    signal.duration = 60;
    return signal;
}

inline void require(const ts::StorageResult& result) {
    if (!result.succeeded()) {
        throw std::runtime_error(std::string(ts::to_string(result.kind)) + ": " + result.message);
    }
}

template<class T>
T read(const ts::StorageReadResult<T>& result) {
    if (!result.found()) throw std::runtime_error("missing or invalid durable fact: " + result.message);
    return *result.value;
}

inline ts::SeriesSnapshot snapshot(const ts::SeriesCheckpoint& checkpoint) {
    return {checkpoint.series, checkpoint.status, checkpoint.current_intent_id,
            checkpoint.current_group_id, checkpoint.current_step, checkpoint.current_outcome};
}

inline void assign_snapshot(ts::SeriesCheckpoint& checkpoint, const ts::SeriesSnapshot& state) {
    checkpoint.series = state.series;
    checkpoint.status = state.status;
    checkpoint.current_intent_id = state.current_intent_id;
    checkpoint.current_group_id = state.current_group_id;
    checkpoint.current_step = state.current_step;
    checkpoint.current_outcome = state.outcome;
}

/// \brief Explicit side-effect stages allow deterministic crash injection between calls.
/// \details All calls, including inspection, belong to one caller thread. The supplied
/// broker state outlives this host; all risk/series owners and the MDBX connection do not.
class SyntheticHost {
public:
    SyntheticHost(mdbxc::Config database, SyntheticConfig config, std::int64_t now_ms,
                  std::shared_ptr<ts::SimulatedExecutionAdapter::State> broker)
        : m_config(std::move(config)), m_clock(now_ms), m_manager(m_clock),
          m_storage(std::move(database)), m_broker(std::move(broker), false) {
        validate_config();
        const auto checkpoint = m_storage.series(1);
        const auto mm = m_storage.money_management("synthetic.mm");
        if (checkpoint.kind == ts::StorageResultKind::NOT_FOUND &&
            mm.kind == ts::StorageResultKind::NOT_FOUND) {
            ts::SeriesCheckpoint initial;
            initial.series = {1, 7, 0, 0};
            ts::MoneyManagementStateRecord state;
            state.module_instance_id = "synthetic.mm";
            state.state_type = "synthetic.capped-loss.v1";
            state.payload = {{"codec_version", 1}, {"configuration", configuration()},
                             {"after_loss", false}};
            auto unit = m_storage.begin();
            require(unit.put_series(initial, 0));
            require(unit.put_money_management(state, 0));
            require(unit.commit());
        }
        reload();
    }

    const ts::TradingSystemStorage& storage() const noexcept { return m_storage; }
    const ts::SeriesCheckpoint& series() const noexcept { return m_checkpoint; }
    const ts::MoneyManagementStateRecord& mm_state() const noexcept { return m_mm; }
    ts::SimulatedExecutionAdapter& broker() noexcept { return m_broker; }

    std::int64_t reserved(std::uint64_t scope) const {
        return m_risk.at(scope)->reserved_risk_units();
    }

    /// \brief Filters/sizes, admits each scope once, then atomically prepares the group.
    ts::StorageResult accept(const optionx::TradeSignal& signal) {
        const auto context = ts::TradingDecisionContext::from_signal(
            signal, m_clock.now_ms(), optionx::TradeTimeZone::utc());
        if (ts::SymbolFilter({"EURUSD"}).evaluate(context).kind != ts::PolicyDecisionKind::ALLOW ||
            ts::TradingWindowFilter({{9 * 3600, 18 * 3600}}).evaluate(context).kind !=
                ts::PolicyDecisionKind::ALLOW) {
            return {ts::StorageResultKind::INVALID_ARGUMENT, "filter rejected signal"};
        }
        const auto sizing = m_config.loss_step
            ? ts::LossStepStake(m_config.base_stake, m_config.loss_multiplier).evaluate(
                  context, m_mm.payload.at("after_loss").get<bool>())
            : ts::FixedStake(m_config.base_stake).evaluate(context);
        if (!sizing.amount) return {ts::StorageResultKind::INVALID_ARGUMENT, ts::to_string(sizing.reason)};
        if (signal.signal_id != 7 || signal.order_type != optionx::OrderType::BUY ||
            signal.currency != m_config.base_stake.currency ||
            m_checkpoint.status != ts::SeriesStatus::ACTIVE) {
            return {ts::StorageResultKind::INVALID_STATE, "synthetic series is not eligible"};
        }
        const auto readiness = m_manager.admission_readiness(m_storage, true);
        for (const auto& owner : m_risk) {
            if (!readiness.scope_admission_allowed(owner.first)) {
                return {ts::StorageResultKind::INVALID_STATE, "durable admission gate is closed"};
            }
        }

        ts::TradeIntent intent;
        ts::ExecutionRosterRecord roster;
        std::vector<ts::ReservationRecord> reservations;
        std::vector<ts::PhysicalTradeRecord> physical;
        // Allocate durable IDs first; failed admission may leave harmless sequence holes.
        {
            auto unit = m_storage.begin();
            intent.intent_id = allocate(unit, "intent");
            intent.execution_group = {allocate(unit, "group"),
                static_cast<std::uint32_t>(m_config.targets.size())};
            intent.origin_signal_id = 7;
            intent.series_id = 1;
            intent.series_step = static_cast<std::int32_t>(m_checkpoint.series.next_step);
            intent.cardinality = m_config.targets.size() == 1
                ? ts::ExecutionCardinality::EXACTLY_ONE : ts::ExecutionCardinality::SPLIT_GROUP;
            intent.idempotency_key = "synthetic-intent-" + std::to_string(intent.intent_id);
            signal.assign_to_request(intent.request);
            intent.request.amount = std::stod(sizing.amount->to_decimal());
            roster.group_id = intent.execution_group.group_id;
            roster.intent_id = intent.intent_id;
            roster.expected_members = intent.execution_group.expected_members;
            for (const auto& target : m_config.targets) {
                const auto trade_id = allocate(unit, "trade");
                const auto reservation_id = allocate(unit, "reservation");
                const auto key = intent.idempotency_key + ":risk:" + std::to_string(trade_id);
                roster.members.push_back({trade_id, target.account_id, target.risk_scope_id, *sizing.amount});
                roster.reservation_bindings.push_back({trade_id, reservation_id,
                    target.risk_scope_id, *sizing.amount, *sizing.amount, key});
                ts::ReservationRecord reservation;
                reservation.reservation_id = reservation_id;
                reservation.risk_scope_id = target.risk_scope_id;
                reservation.intent_id = intent.intent_id;
                reservation.execution_group = intent.execution_group;
                reservation.cardinality = intent.cardinality;
                reservation.stake = *sizing.amount;
                reservation.potential_risk = *sizing.amount;
                reservation.state = ts::RiskReservationState::RESERVED;
                reservation.account_revision = m_config.accounts.at(target.risk_scope_id).revision;
                reservation.idempotency_key = key;
                reservations.push_back(reservation);
                physical.push_back({trade_id, intent.intent_id, roster.group_id,
                    target.account_id, target.risk_scope_id, "ADMITTED", {}, 0});
            }
            require(unit.commit());
        }

        std::map<std::uint64_t, ts::MoneyAmount> amounts;
        for (const auto& reservation : reservations) {
            auto found = amounts.emplace(reservation.risk_scope_id,
                                         ts::MoneyAmount{0, sizing.amount->precision, sizing.amount->currency}).first;
            if (!add_units(found->second, reservation.stake)) {
                return {ts::StorageResultKind::INVALID_ARGUMENT, "scope stake overflow"};
            }
        }
        for (const auto& amount : amounts) {
            const auto admission = m_risk.at(amount.first)->reserve(
                ts::RiskRequest::from_intent(intent, amount.second, amount.second), m_clock.now_ms());
            if (!admission.succeeded()) {
                reload_risk(); // Discard any earlier scope admissions, never durable preparations.
                return {ts::StorageResultKind::INVALID_STATE, "risk admission rejected or unavailable"};
            }
        }
        intent.status = ts::IntentStatus::ADMITTED;
        std::vector<const ts::RiskAdmission*> owners;
        for (const auto& reservation : reservations) owners.push_back(m_risk.at(reservation.risk_scope_id).get());
        const auto prepared = m_manager.prepare(m_storage, intent, roster, reservations,
            physical, owners, m_checkpoint.series.state_revision);
        reload(); // Preparation leases are gone; only committed facts become owner state.
        return prepared;
    }

    std::vector<ts::ExecutionOperationRecord> operations(ts::ExecutionGroupId group) const {
        const auto recovered = ts::ExecutionRecoveryManager::recover(m_storage);
        if (!recovered.succeeded()) throw std::runtime_error(recovered.diagnostic);
        std::vector<ts::ExecutionOperationRecord> result;
        for (const auto& entry : recovered.operations) {
            if (entry.second.group_id == group) result.push_back(entry.second);
        }
        return result;
    }

    ts::StorageResult submit(ts::ExecutionGroupId group,
                            ts::SimulatedSubmissionPlan plan = ts::SimulatedSubmissionPlan::SUCCESS) {
        for (const auto& operation : operations(group)) {
            m_broker.set_plan(operation.operation_id, plan);
            const auto result = m_manager.submit(m_storage, operation, m_broker);
            if (!result.succeeded()) return result;
        }
        return {ts::StorageResultKind::APPLIED, {}};
    }

    /// \brief Simulated external outcome; no local storage or progression mutation.
    ts::TradeSettlement broker_result(ts::ExecutionGroupId group, std::size_t member,
                                     std::int64_t net_units) {
        const auto operation = operations(group).at(member);
        ts::TradeSettlement settlement;
        // Synthetic broker IDs are from the immutable physical trade sequence.
        settlement.settlement_id = operation.trade_id;
        settlement.intent_id = operation.intent_id;
        settlement.execution_group_id = group;
        settlement.trade_id = operation.trade_id;
        settlement.kind = net_units > 0 ? ts::OutcomeKind::WIN
            : net_units < 0 ? ts::OutcomeKind::LOSS : ts::OutcomeKind::DRAW;
        settlement.evidence = ts::ExecutionEvidence::EXECUTED;
        settlement.net_profit = m_config.base_stake;
        settlement.net_profit->units = net_units;
        if (!m_broker.attach_settlement(operation.operation_id, settlement)) {
            throw std::runtime_error("simulated execution not uniquely identified");
        }
        return settlement;
    }

    /// \brief Reconciles without submission, then persists available physical facts only.
    ts::StorageResult observe(ts::ExecutionGroupId group) {
        for (auto operation : operations(group)) {
            if (operation.state == ts::ExecutionOperationState::SETTLED) continue;
            const auto evidence = m_manager.reconcile(m_storage, operation, m_broker);
            if (evidence.kind != ts::ReconciliationKind::MATCH_CONFIRMED) {
                return {ts::StorageResultKind::INVALID_STATE, evidence.diagnostic};
            }
            if (evidence.settlement) {
                const auto result = m_manager.settle_reconciled(m_storage, operation, *evidence.settlement);
                if (!result.succeeded()) return result;
            }
        }
        return {ts::StorageResultKind::APPLIED, {}};
    }

    /// \brief Canonical group completion + series/MM + all reservation releases, one UOW.
    ts::StorageResult complete(ts::ExecutionGroupId group) {
        const auto roster = read(m_storage.roster(group));
        const auto records = m_storage.settlements_for_group(group);
        if (!records.succeeded()) return {records.kind, records.message};
        std::vector<std::uint64_t> ids;
        for (const auto& member : roster.members) ids.push_back(member.trade_id);
        ts::ExecutionGroupAggregator aggregate(roster.intent_id, {group, roster.expected_members},
            ids, m_config.base_stake.currency, m_config.base_stake.precision);
        std::vector<ts::TradeSettlement> receipts;
        for (const auto& record : records.values) {
            const auto applied = aggregate.apply(record.settlement);
            if (applied != ts::ProgressionResult::APPLIED && applied != ts::ProgressionResult::DUPLICATE) {
                return {ts::StorageResultKind::CONFLICT, "conflicting group evidence"};
            }
            receipts.push_back(record.settlement);
        }
        if (!aggregate.outcome().final || !aggregate.outcome().net_profit) {
            return {ts::StorageResultKind::INVALID_STATE, "group is not terminal and valued"};
        }
        auto candidate = *m_series;
        const auto progressed = candidate.apply_group_completion(receipts, m_checkpoint.series.state_revision);
        if (progressed == ts::ProgressionResult::DUPLICATE) return {ts::StorageResultKind::DUPLICATE, {}};
        if (progressed != ts::ProgressionResult::APPLIED) {
            return {ts::StorageResultKind::INVALID_STATE, "canonical series completion rejected"};
        }
        auto checkpoint = m_checkpoint;
        assign_snapshot(checkpoint, candidate.snapshot());
        checkpoint.receipts.insert(checkpoint.receipts.end(), receipts.begin(), receipts.end());
        auto mm = m_mm;
        ++mm.state_revision;
        mm.last_settlement_id = receipts.front().settlement_id;
        const bool after_loss = m_mm.payload.at("after_loss").get<bool>();
        mm.payload["after_loss"] = m_config.loss_step && !after_loss &&
            aggregate.outcome().kind == ts::OutcomeKind::LOSS;
        ts::SettlementTransition transition;
        transition.settlement = records.values.front();
        transition.journal.idempotency_key = "settlement:" + std::to_string(mm.last_settlement_id);
        transition.journal.occurred_at_ms = m_clock.now_ms();
        transition.series = checkpoint;
        transition.money_management = mm;
        transition.expected_series_revision = m_checkpoint.series.state_revision;
        transition.expected_money_management_revision = m_mm.state_revision;
        transition.logical_currency = m_config.base_stake.currency;
        for (const auto& binding : roster.reservation_bindings) {
            auto reservation = read(m_storage.reservation(binding.reservation_id));
            const auto revision = reservation.state_revision++;
            reservation.state = ts::RiskReservationState::SETTLED;
            if (binding.trade_id == transition.settlement.settlement.trade_id) {
                transition.reservation = reservation;
                transition.expected_reservation_revision = revision;
            } else {
                transition.group_reservation_bindings.push_back({binding.trade_id, reservation, revision});
            }
        }
        {
            auto unit = m_storage.begin();
            const auto completed = unit.apply_group_completion_atomic(transition);
            if (!completed.succeeded()) return completed;
            require(unit.commit());
        }
        reload();
        return {ts::StorageResultKind::APPLIED, {}};
    }

private:
    static std::uint64_t allocate(ts::TradingSystemStorage::UnitOfWork& unit, const char* name) {
        const auto id = unit.next_id(name);
        if (id.kind != ts::StorageResultKind::APPLIED) throw std::runtime_error(id.message);
        return id.value;
    }

    static bool add_units(ts::MoneyAmount& total, const ts::MoneyAmount& amount) {
        if (amount.units <= 0 || total.units >
            (std::numeric_limits<std::int64_t>::max)() - amount.units) return false;
        total.units += amount.units;
        return true;
    }

    void validate_config() const {
        if (!m_config.base_stake.valid_metadata() || m_config.base_stake.units <= 0 ||
            m_config.targets.empty() || m_config.targets.size() > 16 ||
            (m_config.loss_step && m_config.loss_multiplier <= 1)) {
            throw std::invalid_argument("invalid synthetic configuration");
        }
        std::set<std::uint64_t> accounts;
        for (const auto& target : m_config.targets) {
            if (!target.account_id || !target.risk_scope_id ||
                !accounts.insert(target.account_id).second || !m_config.accounts.count(target.risk_scope_id)) {
                throw std::invalid_argument("invalid or duplicated target");
            }
        }
    }

    nlohmann::json configuration() const {
        nlohmann::json targets = nlohmann::json::array();
        for (const auto& target : m_config.targets) targets.push_back({target.account_id, target.risk_scope_id});
        return {{"units", m_config.base_stake.units}, {"precision", m_config.base_stake.precision},
                {"currency", static_cast<int>(m_config.base_stake.currency)},
                {"loss_step", m_config.loss_step}, {"multiplier", m_config.loss_multiplier},
                {"targets", targets}};
    }

    void reload() {
        auto checkpoint = read(m_storage.series(1));
        auto mm = read(m_storage.money_management("synthetic.mm"));
        if (mm.state_type != "synthetic.capped-loss.v1" ||
            mm.payload.at("codec_version") != 1 || mm.payload.at("configuration") != configuration() ||
            !mm.payload.at("after_loss").is_boolean()) {
            throw std::runtime_error("unsupported or incompatible synthetic MM state");
        }
        ts::ExecutionGroupAggregator group;
        if (checkpoint.current_group_id) {
            const auto roster = read(m_storage.roster(checkpoint.current_group_id));
            std::vector<std::uint64_t> ids;
            for (const auto& member : roster.members) ids.push_back(member.trade_id);
            group = ts::ExecutionGroupAggregator(roster.intent_id, {roster.group_id, roster.expected_members},
                ids, m_config.base_stake.currency, m_config.base_stake.precision);
        }
        auto series = std::make_unique<ts::SeriesProgression>(checkpoint.series);
        if (series->hydrate(snapshot(checkpoint), std::move(group), checkpoint.receipts,
                {checkpoint.used_intents.begin(), checkpoint.used_intents.end()},
                {checkpoint.used_intent_keys.begin(), checkpoint.used_intent_keys.end()}) !=
                    ts::ProgressionResult::APPLIED) {
            throw std::runtime_error("cannot hydrate canonical series owner");
        }
        reload_risk();
        m_checkpoint = std::move(checkpoint);
        m_mm = std::move(mm);
        m_series = std::move(series);
    }

    void reload_risk() {
        const auto durable = m_storage.reservations();
        if (!durable.succeeded()) throw std::runtime_error(durable.message);
        std::map<std::uint64_t, std::unique_ptr<ts::RiskAdmission>> owners;
        std::set<std::uint64_t> scopes;
        for (const auto& target : m_config.targets) scopes.insert(target.risk_scope_id);
        for (const auto scope : scopes) {
            ts::RiskAdmissionConfig config;
            config.scope = {ts::RiskScopeKind::ACCOUNT, scope};
            auto owner = std::make_unique<ts::RiskAdmission>(config);
            std::map<ts::IntentId, ts::RiskReservation> by_intent;
            std::uint64_t maximum_id = 0;
            for (const auto& reservation : durable.values) {
                if (reservation.risk_scope_id != scope) continue;
                maximum_id = (std::max)(maximum_id, reservation.reservation_id);
                auto token = reservation.to_risk_reservation();
                token.idempotency_key = read(m_storage.intent(token.intent_id)).idempotency_key;
                const auto found = by_intent.find(token.intent_id);
                if (found == by_intent.end()) by_intent.emplace(token.intent_id, token);
                else {
                    if (found->second.state != token.state ||
                        found->second.account_revision != token.account_revision ||
                        !add_units(found->second.stake, token.stake) ||
                        !add_units(found->second.potential_risk, token.potential_risk)) {
                        throw std::runtime_error("inconsistent shared-scope reservation projection");
                    }
                    found->second.reservation_id = (std::min)(found->second.reservation_id, token.reservation_id);
                }
            }
            std::vector<ts::RiskReservation> tokens;
            for (const auto& entry : by_intent) tokens.push_back(entry.second);
            const auto account = m_config.accounts.at(scope);
            if (tokens.empty()) {
                if (account.valid() && owner->update_snapshot(account).kind != ts::RiskAdmissionKind::APPLIED) {
                    throw std::runtime_error("invalid account snapshot");
                }
            } else if (maximum_id == (std::numeric_limits<std::uint64_t>::max)() ||
                owner->hydrate(account, tokens, maximum_id + 1).kind != ts::RiskAdmissionKind::APPLIED) {
                throw std::runtime_error("cannot hydrate canonical risk owner");
            }
            owners.emplace(scope, std::move(owner));
        }
        m_risk = std::move(owners);
    }

    SyntheticConfig m_config;
    ts::FakeExecutionClock m_clock;
    ts::ExecutionRecoveryManager m_manager;
    ts::TradingSystemStorage m_storage;
    ts::SimulatedExecutionAdapter m_broker;
    ts::SeriesCheckpoint m_checkpoint;
    ts::MoneyManagementStateRecord m_mm;
    std::unique_ptr<ts::SeriesProgression> m_series;
    std::map<std::uint64_t, std::unique_ptr<ts::RiskAdmission>> m_risk;
};

} // namespace optionx_examples

#endif // OPTIONX_EXAMPLE_SYNTHETIC_HOST_HPP_INCLUDED
