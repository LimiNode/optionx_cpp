#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_OFFLINE_PLATFORM_OWNER_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_OFFLINE_PLATFORM_OWNER_HPP_INCLUDED

/// \file OfflinePlatformOwner.hpp
/// \brief Offline lifecycle adapter using the existing execution and completion owners.

#include <set>

namespace optionx::trading_system::detail {

/// \class OfflinePlatformOwner
/// \brief Single-owner manual lifecycle integration; not a live TradingSystem facade.
/// \details All methods except platform callbacks belong to the start() thread.
/// Storage, manager, and the supplied idempotent completion owner must outlive
/// this adapter. Endpoint metadata is an attestation by the trusted host.
class OfflinePlatformOwner {
public:
    struct Endpoint {
        std::shared_ptr<platforms::BaseTradingPlatform> platform;
        AccountIdentityRecord account;
    };
    using Complete = std::function<StorageResult(ExecutionGroupId)>;

    OfflinePlatformOwner(TradingSystemStorage& storage, ExecutionRecoveryManager& manager,
                         std::vector<Endpoint> endpoints, Complete complete,
                         std::size_t capacity = 64, std::size_t bytes = 1024 * 1024)
        : m_storage(storage), m_manager(manager), m_endpoints(std::move(endpoints)),
          m_complete(std::move(complete)), m_inbox(storage, manager, capacity, bytes),
          m_control(std::make_shared<Control>()) {}

    OfflinePlatformOwner(const OfflinePlatformOwner&) = delete;
    OfflinePlatformOwner& operator=(const OfflinePlatformOwner&) = delete;

    ~OfflinePlatformOwner() {
        // Explicit shutdown() is the durable drain; destruction only cancels,
        // joins, and revokes callbacks, leaving lost facts for reconciliation.
        stop_platforms();
        m_inbox.close_ingress();
        m_control->active.store(false);
        m_tasks.shutdown();
    }

    StorageResult start() {
        if (m_started || !m_complete) return invalid("owner already started or missing completion contract");
        m_thread = std::this_thread::get_id();
        std::set<std::uint64_t> accounts;
        std::set<const platforms::BaseTradingPlatform*> instances;
        for (const auto& endpoint : m_endpoints) {
            const auto stored = m_storage.account(endpoint.account.account_id);
            if (!endpoint.platform || !stored.found() || !endpoint.account.active ||
                !stored.value->active || !accounts.insert(endpoint.account.account_id).second ||
                !instances.insert(endpoint.platform.get()).second ||
                endpoint.platform->platform_type() != PlatformType::INTRADE_BAR ||
                endpoint.account.platform_type != "IntradeBar" ||
                platform_receipts::account_identity(endpoint.account) !=
                    platform_receipts::account_identity(*stored.value)) {
                return invalid("endpoint identity mismatch");
            }
        }
        const auto recovered = m_inbox.start();
        if (!recovered.succeeded()) return invalid(recovered.diagnostic);
        {
            auto unit = m_storage.begin();
            const auto generation = unit.next_id("offline-platform-generation");
            if (generation.kind != StorageResultKind::APPLIED) return invalid(generation.message);
            const auto committed = unit.commit();
            if (!committed.succeeded()) return committed;
            m_control->generation = generation.value;
        }
        for (const auto& entry : recovered.operations) m_groups.insert(entry.second.group_id);
        m_control->active.store(true);
        m_started = true;
        for (const auto& endpoint : m_endpoints) endpoint.platform->run(false);
        return {StorageResultKind::APPLIED, {}};
    }

    /// \brief Commits submission-started before posting the external command.
    /// \details Even scheduler rejection/cancellation retains the durable
    /// uncertain boundary. No automatic resend or invented OPEN_ERROR follows.
    StorageResult submit(ExecutionOperationRecord operation, const platform_receipts::Binding& binding) try {
        if (!ready() || operation.state != ExecutionOperationState::PREPARED ||
            operation.operation_id != binding.operation_id()) return invalid("submission is not eligible");
        auto endpoint = find_endpoint(binding);
        if (!endpoint) return invalid("endpoint does not match sealed account");
        // A fresh capability/revision check precedes the durable boundary.
        const auto restored = m_manager.restore_platform_binding(m_storage, operation.operation_id);
        if (!restored.succeeded() || restored.binding->document() != binding.document() ||
            restored.binding->event_id() != binding.event_id()) return invalid("stale platform binding");
        const auto account = m_storage.account(operation.account_id);
        if (!account.found() || !account.value->active || !m_inbox.track_durable_operation(operation)) {
            return invalid("account inactive or operation revision stale");
        }
        auto request = std::make_shared<std::unique_ptr<TradeRequest>>(
            std::make_unique<TradeRequest>(binding.request()));
        const auto sink = callback(binding);
        (*request)->add_callback([sink](auto, std::unique_ptr<TradeResult> result) {
            if (result) sink(*result);
        });
        const auto marked = m_manager.mark_submission_started(m_storage, operation);
        if (!marked.succeeded()) return marked;
        ++operation.state_revision;
        operation.state = ExecutionOperationState::SUBMISSION_STARTED;
        // mark_submission_started sets the durable observation timestamp.
        const auto event = m_storage.journal_by_key(ExecutionRecoveryManager::operation_key(operation));
        if (!event.found() || !m_inbox.track_durable_operation(event.value->payload.get<ExecutionOperationRecord>())) {
            block(); return invalid("cannot track submission revision");
        }
        m_groups.insert(operation.group_id);
        const auto weak = std::weak_ptr<platforms::BaseTradingPlatform>(endpoint->platform);
        return post(*endpoint, [weak, request] {
            const auto platform = weak.lock();
            return platform && platform->place_trade(std::move(*request));
        });
    } catch (...) {
        block(); return invalid("submission preparation failed; reconcile durable facts");
    }

    /// \brief Reconciles a confirmed numeric broker ID, never reopens a trade.
    StorageResult query(const platform_receipts::Binding& binding, std::uint64_t revision) {
        if (!ready()) return invalid("owner is not ready");
        auto endpoint = find_endpoint(binding);
        if (!endpoint) return invalid("query endpoint mismatch");
        QueryApi api(*this, *endpoint);
        return m_manager.request_known_result(m_storage, binding, revision, api,
            endpoint->account, callback(binding));
    }

    /// \brief Processes platform tasks, then durable receipts and ready logical groups.
    StorageResult process() try {
        if (!on_owner() || !m_started || m_stopped) return invalid("wrong owner lifecycle");
        for (const auto& endpoint : m_endpoints) endpoint.platform->process();
        const auto result = std::make_shared<StorageResult>(StorageResult{StorageResultKind::APPLIED, {}});
        if (!m_tasks.add_single_task([this, result](auto task) {
            if (!task->is_shutdown()) *result = apply();
        })) { block(); return invalid("owner scheduling failed"); }
        m_tasks.process();
        return *result;
    } catch (...) {
        block(); return invalid("owner processing failed; reconcile durable facts");
    }

    StorageResult shutdown() {
        if (!on_owner() || !m_started) return invalid("shutdown requires the owner thread");
        if (m_stopped) return {StorageResultKind::DUPLICATE, {}};
        m_stopped = true;
        stop_platforms(); // Drain/cancel accepted platform tasks while ingress is alive.
        const auto drained = m_inbox.shutdown_and_drain();
        m_control->active.store(false);
        m_tasks.shutdown();
        if (drained.blocked || !success(drained.kind) || m_control->blocked.load()) {
            block(); return invalid("shutdown needs reconciliation");
        }
        return complete_ready();
    }

    bool ready() const {
        return on_owner() && m_started && !m_stopped &&
            !m_control->blocked.load() && m_inbox.ready();
    }

private:
    struct Control {
        std::atomic_bool active{false};
        std::atomic_bool blocked{false};
        std::atomic<std::uint64_t> sequence{0};
        std::uint64_t generation = 0;
    };
    struct QueryApi : platforms::BaseTradingApi {
        OfflinePlatformOwner& owner;
        Endpoint& endpoint;
        QueryApi(OfflinePlatformOwner& value, Endpoint& target) : owner(value), endpoint(target) {}
        bool fetch_trade_result(TradeResultQuery query, std::unique_ptr<TradeResult> result,
                                trade_result_check_callback_t callback) override {
            const auto weak = std::weak_ptr<platforms::BaseTradingPlatform>(endpoint.platform);
            auto seed = std::make_shared<std::unique_ptr<TradeResult>>(std::move(result));
            return owner.post(endpoint, [weak, query, seed, callback] {
                const auto platform = weak.lock();
                return platform && platform->fetch_trade_result(query, std::move(*seed), callback);
            }).succeeded();
        }
    };

    static StorageResult invalid(std::string message) { return {StorageResultKind::INVALID_STATE, std::move(message)}; }
    static bool success(StorageResultKind kind) {
        return kind == StorageResultKind::APPLIED || kind == StorageResultKind::DUPLICATE;
    }
    bool on_owner() const { return m_thread == std::this_thread::get_id(); }
    void block() { m_control->blocked.store(true); }
    void stop_platforms() noexcept {
        for (const auto& endpoint : m_endpoints) if (endpoint.platform) endpoint.platform->shutdown();
    }
    Endpoint* find_endpoint(const platform_receipts::Binding& binding) {
        for (auto& endpoint : m_endpoints) {
            const auto stored = m_storage.account(endpoint.account.account_id);
            if (stored.found() && platform_receipts::account_identity(*stored.value) ==
                platform_receipts::account_identity(endpoint.account) &&
                platform_receipts::account_identity(endpoint.account) ==
                platform_receipts::account_identity(detail::account_from_json(binding.document().at("account")))) return &endpoint;
        }
        return nullptr;
    }
    std::function<void(const TradeResult&)> callback(const platform_receipts::Binding& binding) {
        const auto sink = m_inbox.make_sink(binding);
        const auto weak = std::weak_ptr<Control>(m_control);
        const auto generation = m_control->generation;
        return [weak, generation, sink](const TradeResult& result) {
            const auto control = weak.lock();
            if (!control || !control->active.load() || generation != control->generation) return;
            try {
                const auto receipt = "generation:" + std::to_string(generation) + ":receipt:" +
                    std::to_string(control->sequence.fetch_add(1));
                if (sink(result, receipt) != ObservationIngress::ACCEPTED) control->blocked.store(true);
            } catch (...) { control->blocked.store(true); }
        };
    }
    StorageResult post(Endpoint& endpoint, std::function<bool()> work) {
        const auto weak = std::weak_ptr<Control>(m_control);
        const auto generation = m_control->generation;
        try {
            const bool accepted = endpoint.platform->post_task([weak, generation, work] {
                const auto control = weak.lock();
                if (!control || !control->active.load() || control->generation != generation ||
                    control->blocked.load()) return;
                try { if (!work()) control->blocked.store(true); }
                catch (...) { control->blocked.store(true); }
            }, [weak] { if (const auto control = weak.lock()) control->blocked.store(true); });
            if (accepted) return {StorageResultKind::APPLIED, {}};
        } catch (...) {}
        block();
        return invalid("platform scheduling failed; durable risk retained");
    }
    StorageResult apply() {
        const auto drained = m_inbox.drain();
        if (drained.blocked || !success(drained.kind) || m_control->blocked.load()) {
            block(); return invalid("observation handoff needs reconciliation");
        }
        return complete_ready();
    }
    StorageResult complete_ready() {
        for (auto group = m_groups.begin(); group != m_groups.end();) {
            const auto roster = m_storage.roster(*group);
            const auto records = m_storage.settlements_for_group(*group);
            if (!roster.found() || !records.succeeded()) { block(); return invalid("cannot read group facts"); }
            std::set<std::uint64_t> terminal;
            for (const auto& record : records.values) if (record.settlement.terminal()) terminal.insert(record.settlement.trade_id);
            bool ready = terminal.size() == roster.value->expected_members;
            for (const auto& member : roster.value->members) ready = ready && terminal.count(member.trade_id) != 0;
            if (!ready) { ++group; continue; }
            const auto completed = m_complete(*group);
            if (!completed.succeeded()) { block(); return completed; }
            group = m_groups.erase(group);
        }
        return {StorageResultKind::APPLIED, {}};
    }

    TradingSystemStorage& m_storage;
    ExecutionRecoveryManager& m_manager;
    std::vector<Endpoint> m_endpoints;
    Complete m_complete;
    PlatformObservationHandoff m_inbox;
    std::shared_ptr<Control> m_control;
    utils::TaskManager m_tasks;
    std::set<ExecutionGroupId> m_groups;
    std::thread::id m_thread;
    bool m_started = false;
    bool m_stopped = false;
};

} // namespace optionx::trading_system::detail
#endif // OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_OFFLINE_PLATFORM_OWNER_HPP_INCLUDED
