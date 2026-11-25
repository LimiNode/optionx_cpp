#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_PLATFORM_OBSERVATION_HANDOFF_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_PLATFORM_OBSERVATION_HANDOFF_HPP_INCLUDED

/// \file PlatformObservationHandoff.hpp
/// \brief Bounded callback-to-owner handoff for durable platform receipts.

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace optionx::trading_system::detail {

enum class ObservationIngress {
    ACCEPTED,
    CLOSED,
    QUEUE_FULL,
    INVALID_RECEIPT
};

struct ObservationDrainResult {
    std::size_t processed = 0;
    StorageResultKind kind = StorageResultKind::APPLIED;
    std::string message;
    bool blocked = false;
};

/// \class PlatformObservationHandoff
/// \brief Owns one manual-pump execution inbox; callbacks never touch storage.
/// \details The host drives drain() from one execution-owner TaskManager loop,
/// after broker calls return. Storage, manager, and their clock outlive this
/// object. A callback captures only a weak inbox and a sealed binding, never
/// the owner or its storage. Destruction closes ingress without DB work;
/// unresolved durable risk remains for restart reconciliation.
class PlatformObservationHandoff {
public:
    using Sink = std::function<ObservationIngress(TradeResult, std::string)>;

    PlatformObservationHandoff(TradingSystemStorage& storage,
                               ExecutionRecoveryManager& manager,
                               std::size_t capacity)
        : m_storage(storage), m_manager(manager), m_state(std::make_shared<State>(capacity)) {}

    PlatformObservationHandoff(const PlatformObservationHandoff&) = delete;
    PlatformObservationHandoff& operator=(const PlatformObservationHandoff&) = delete;

    ~PlatformObservationHandoff() { close_ingress(); }

    /// \brief Validates journal revision chains once before accepting callbacks.
    /// \return Recovery status; failed startup never opens admission.
    RecoveryResult start() {
        if (m_ready.load(std::memory_order_acquire)) {
            return {RecoveryResultKind::STORAGE_ERROR, {}, {}, "execution owner already started"};
        }
        if (m_owner_thread != std::thread::id{} && m_owner_thread != std::this_thread::get_id()) {
            return {RecoveryResultKind::STORAGE_ERROR, {}, {}, "execution owner thread changed"};
        }
        m_owner_thread = std::this_thread::get_id();
        auto recovered = ExecutionRecoveryManager::recover(m_storage);
        if (!recovered.succeeded()) return recovered;
        m_operations = recovered.operations;
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->closed || m_state->blocked ||
            m_state->capacity == 0 || m_state->capacity > 4096) {
            return {RecoveryResultKind::STORAGE_ERROR, {}, {}, "observation ingress is closed"};
        }
        m_state->accepting = true;
        m_ready.store(true, std::memory_order_release);
        return recovered;
    }

    /// \brief Tracks a newly durable operation without a full journal scan.
    /// \return False if the supplied revision is not the persisted operation fact.
    bool track_durable_operation(const ExecutionOperationRecord& operation) {
        if (!owner_thread() || !ready()) return false;
        const auto fact = m_storage.journal_by_key(ExecutionRecoveryManager::operation_key(operation));
        if (!fact.found() || fact.value->event_type != "EXECUTION_OPERATION") return false;
        try {
            const auto persisted = fact.value->payload.get<ExecutionOperationRecord>();
            if (persisted.operation_id != operation.operation_id ||
                persisted.state_revision != operation.state_revision ||
                persisted.trade_id != operation.trade_id ||
                persisted.account_id != operation.account_id ||
                persisted.group_id != operation.group_id) return false;
        } catch (...) {
            return false;
        }
        const auto existing = m_operations.find(operation.operation_id);
        if (existing == m_operations.end()) {
            if (operation.state_revision != 0) return false;
        } else {
            const auto& prior = existing->second;
            if (operation.state_revision <= prior.state_revision ||
                operation.state_revision - prior.state_revision != 1 ||
                prior.account_id != operation.account_id ||
                prior.trade_id != operation.trade_id ||
                prior.group_id != operation.group_id ||
                prior.risk_scope_id != operation.risk_scope_id ||
                prior.intent_id != operation.intent_id) return false;
        }
        m_operations[operation.operation_id] = operation;
        return true;
    }

    /// \brief Creates a generation-bound callback sink for one sealed operation.
    /// \param binding Owner-issued immutable operation/account binding.
    /// \return A callback safe to invoke after handoff destruction.
    Sink make_sink(platform_receipts::Binding binding) const {
        const std::weak_ptr<State> weak = m_state;
        return [weak, binding = std::move(binding)](TradeResult result, std::string receipt_id) mutable {
            const auto state = weak.lock();
            if (!state) return ObservationIngress::CLOSED;
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->accepting || state->closed || state->blocked) return ObservationIngress::CLOSED;
            if (receipt_id.empty()) {
                state->blocked = true;
                return ObservationIngress::INVALID_RECEIPT;
            }
            if (state->queue.size() >= state->capacity) {
                state->blocked = true;
                return ObservationIngress::QUEUE_FULL;
            }
            try {
                state->queue.push_back({binding, std::move(result), std::move(receipt_id)});
            } catch (...) {
                state->blocked = true;
                return ObservationIngress::QUEUE_FULL;
            }
            return ObservationIngress::ACCEPTED;
        };
    }

    /// \brief Commits queued observations on the execution owner thread only.
    /// \return Processing count and first durable error; failed item remains queued.
    ObservationDrainResult drain() {
        ObservationDrainResult output;
        if (!owner_thread() || !m_ready.load(std::memory_order_acquire)) {
            output.kind = StorageResultKind::INVALID_STATE;
            output.message = "execution owner is not ready";
            output.blocked = true;
            return output;
        }
        while (true) {
            std::unique_lock<std::mutex> lock(m_state->mutex);
            if (m_state->queue.empty()) {
                output.blocked = m_state->blocked;
                return output;
            }
            std::optional<Entry> entry;
            try {
                entry.emplace(m_state->queue.front());
            } catch (...) {
                m_state->blocked = true;
                output.kind = StorageResultKind::STORAGE_ERROR;
                output.message = "cannot copy queued platform observation";
                output.blocked = true;
                return output;
            }
            lock.unlock();

            const auto operation = m_operations.find(entry->binding.operation_id());
            if (operation == m_operations.end()) {
                output.kind = StorageResultKind::INVALID_STATE;
                output.message = "operation is not tracked by execution owner";
            } else {
                auto applied = m_manager.apply_platform_receipt(m_storage, entry->binding,
                    entry->result, entry->receipt_id, operation->second.state_revision);
                output.kind = applied.kind;
                output.message = applied.message;
                if (applied.succeeded()) {
                    if (applied.operation) operation->second = *applied.operation;
                }
            }
            if (output.kind != StorageResultKind::APPLIED &&
                output.kind != StorageResultKind::DUPLICATE) {
                lock.lock();
                m_state->blocked = true;
                output.blocked = true;
                return output;
            }
            lock.lock();
            m_state->queue.pop_front();
            ++output.processed;
        }
    }

    /// \brief Stops ingress before draining accepted observations.
    ObservationDrainResult shutdown_and_drain() {
        close_ingress();
        return drain();
    }

    /// \brief Closes ingress; safe from any thread and never performs storage work.
    void close_ingress() noexcept {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->accepting = false;
        m_state->closed = true;
    }

    /// \brief Whether new progression remains permitted by this handoff.
    bool ready() const noexcept {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_ready.load(std::memory_order_acquire) &&
               !m_state->blocked && !m_state->closed;
    }

private:
    struct Entry {
        platform_receipts::Binding binding;
        TradeResult result;
        std::string receipt_id;
    };
    struct State {
        explicit State(std::size_t count) : capacity(count) {}
        std::mutex mutex;
        std::deque<Entry> queue;
        std::size_t capacity;
        bool accepting = false;
        bool blocked = false;
        bool closed = false;
    };

    bool owner_thread() const noexcept { return m_owner_thread == std::this_thread::get_id(); }

    TradingSystemStorage& m_storage;
    ExecutionRecoveryManager& m_manager;
    std::shared_ptr<State> m_state;
    std::map<std::string, ExecutionOperationRecord> m_operations;
    std::thread::id m_owner_thread;
    std::atomic_bool m_ready{false};
};

} // namespace optionx::trading_system::detail

#endif // OPTIONX_HEADER_TRADING_SYSTEM_DETAIL_PLATFORM_OBSERVATION_HANDOFF_HPP_INCLUDED
