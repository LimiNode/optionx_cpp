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
    INVALID_RECEIPT,
    TOO_LARGE
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
    using Sink = std::function<ObservationIngress(const TradeResult&, const std::string&)>;
    static constexpr std::size_t max_receipt_bytes = 256;
    static constexpr std::size_t max_broker_hash_bytes = 256;
    static constexpr std::size_t max_diagnostic_bytes = 4096;

    PlatformObservationHandoff(TradingSystemStorage& storage,
                               ExecutionRecoveryManager& manager,
                               std::size_t capacity, std::size_t byte_budget = 1024 * 1024)
        : m_storage(storage), m_manager(manager),
          m_state(std::make_shared<State>(capacity, byte_budget)) {}

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
            m_state->capacity == 0 || m_state->capacity > 4096 ||
            m_state->byte_budget == 0 || m_state->byte_budget > 64 * 1024 * 1024) {
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
            if (nlohmann::json(persisted) != nlohmann::json(operation)) return false;
        } catch (...) {
            return false;
        }
        const auto existing = m_operations.find(operation.operation_id);
        if (existing == m_operations.end()) {
            if (operation.state_revision != 0) return false;
        } else {
            const auto& prior = existing->second;
            if (nlohmann::json(prior) == nlohmann::json(operation)) return true;
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
        const auto sealed = std::make_shared<const platform_receipts::Binding>(std::move(binding));
        return [weak, sealed](const TradeResult& result, const std::string& receipt_id) {
            const auto state = weak.lock();
            if (!state) return ObservationIngress::CLOSED;
            const bool too_large = receipt_id.size() > max_receipt_bytes ||
                result.option_hash.size() > max_broker_hash_bytes ||
                result.error_desc.size() > max_diagnostic_bytes;
            const auto bytes = sizeof(Entry) + receipt_id.size() +
                result.option_hash.size() + result.error_desc.size();
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->accepting || state->closed || state->blocked) return ObservationIngress::CLOSED;
                if (receipt_id.empty() || too_large) {
                    state->blocked = true;
                    return too_large ? ObservationIngress::TOO_LARGE : ObservationIngress::INVALID_RECEIPT;
                }
                if (state->queue.size() + state->copying >= state->capacity || bytes > state->byte_budget - state->bytes) {
                    state->blocked = true;
                    return ObservationIngress::QUEUE_FULL;
                }
                ++state->copying;
                state->bytes += bytes;
            }
            // Bound the DTO first, then copy outside the queue mutex. The sealed
            // binding is shared instead of copying its JSON for every callback.
            try {
                auto entry = std::make_shared<const Entry>(sealed, result, receipt_id, bytes);
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->accepting || state->closed || state->blocked) {
                    --state->copying;
                    state->bytes -= bytes;
                    return ObservationIngress::CLOSED;
                }
                state->queue.push_back(std::move(entry));
                --state->copying;
            } catch (...) {
                std::lock_guard<std::mutex> lock(state->mutex);
                --state->copying;
                state->bytes -= bytes;
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
        std::size_t budget;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            budget = m_state->queue.size();
        }
        while (budget-- > 0) {
            std::unique_lock<std::mutex> lock(m_state->mutex);
            if (m_state->queue.empty()) {
                output.blocked = m_state->blocked;
                return output;
            }
            const auto entry = m_state->queue.front();
            lock.unlock();

            const auto operation = m_operations.find(entry->binding->operation_id());
            if (operation == m_operations.end()) {
                output.kind = StorageResultKind::INVALID_STATE;
                output.message = "operation is not tracked by execution owner";
            } else {
                auto applied = m_manager.apply_platform_receipt(m_storage, *entry->binding,
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
            m_state->bytes -= entry->bytes;
            m_state->queue.pop_front();
            ++output.processed;
            lock.unlock();
        }
        std::lock_guard<std::mutex> lock(m_state->mutex);
        output.blocked = m_state->blocked;
        return output;
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
        Entry(std::shared_ptr<const platform_receipts::Binding> sealed,
              const TradeResult& value, const std::string& receipt, std::size_t size)
            : binding(std::move(sealed)), result(value), receipt_id(receipt), bytes(size) {}
        std::shared_ptr<const platform_receipts::Binding> binding;
        TradeResult result;
        std::string receipt_id;
        std::size_t bytes;
    };
    struct State {
        State(std::size_t count, std::size_t budget) : capacity(count), byte_budget(budget) {}
        std::mutex mutex;
        std::deque<std::shared_ptr<const Entry>> queue;
        std::size_t capacity;
        std::size_t byte_budget;
        std::size_t bytes = 0;
        std::size_t copying = 0;
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
