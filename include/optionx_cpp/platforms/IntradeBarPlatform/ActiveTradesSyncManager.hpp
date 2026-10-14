#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_ACTIVE_TRADES_SYNC_MANAGER_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_ACTIVE_TRADES_SYNC_MANAGER_HPP_INCLUDED

/// \file ActiveTradesSyncManager.hpp
/// \brief Defines Intrade Bar active trade snapshot synchronization.

namespace optionx::platforms::intrade_bar {

    /// \class ActiveTradesSyncManager
    /// \brief Requests broker active-trades snapshots and publishes them to the trade queue.
    class ActiveTradesSyncManager final : public components::BaseComponent {
    public:

        /// \brief Constructs the active trades synchronization manager.
        /// \param platform Reference to the trading platform.
        /// \param request_manager Reference to the request manager for HTTP requests.
        /// \param account_info Shared pointer to account information data.
        explicit ActiveTradesSyncManager(
            BaseTradingPlatform& platform,
            RequestManager& request_manager,
            std::shared_ptr<BaseAccountInfoData> account_info)
            : BaseComponent(platform.event_bus()),
              m_request_manager(request_manager),
              m_account_info(std::move(account_info)) {
            subscribe<events::AuthDataEvent>();
            subscribe<events::RestartAuthEvent>();
            subscribe<events::DisconnectRequestEvent>();
            subscribe<events::AccountInfoUpdateEvent>();
            subscribe<events::OpenTradesSnapshotRefreshRequestEvent>();
            platform.register_component(this);
        }

        /// \brief Default destructor.
        virtual ~ActiveTradesSyncManager() = default;

        /// \brief Dispatches subscribed events.
        /// \param event Incoming event.
        void on_event(const utils::Event* const event) override;

        /// \brief Processes delayed refresh tasks.
        void process() override;

        /// \brief Shuts down delayed refresh tasks and invalidates active callbacks.
        void shutdown() override;

    private:
        struct AccountContext {
            int64_t user_id = 0;
            AccountType account_type = AccountType::UNKNOWN;
            CurrencyType currency = CurrencyType::UNKNOWN;
            std::uint64_t generation = 0;
        };

        RequestManager& m_request_manager; ///< Reference to the request manager.
        utils::TaskManager m_task_manager; ///< Task manager for delayed refresh requests.
        std::shared_ptr<BaseAccountInfoData> m_account_info; ///< Shared pointer to account information.
        int64_t m_active_trades_close_buffer_ms = time_shield::MS_PER_SEC; ///< Safety delay after broker close time.
        int64_t m_active_trades_sync_period_ms = time_shield::MS_PER_15_SEC; ///< Delayed refresh period for uncertain snapshots.
        bool m_sync_in_progress = false; ///< True while a snapshot request is running.
        bool m_refresh_scheduled = false; ///< True while a delayed refresh task is pending.
        std::uint64_t m_sync_generation = 0; ///< Monotonic request generation for stale callback filtering.
        std::uint64_t m_active_sync_generation = 0; ///< Generation of the currently active snapshot request.
        std::uint64_t m_account_context_generation = 0; ///< Changes whenever broker account identity changes.

        /// \brief Starts a broker active-trades snapshot request.
        /// \param reason Human-readable trigger reason for logs.
        void request_sync(std::string reason);

        /// \brief Schedules a delayed broker active-trades snapshot request.
        /// \param reason Human-readable trigger reason for logs.
        void schedule_sync(std::string reason);

        /// \brief Resets active sync request state and invalidates pending callbacks.
        void reset_sync_state();

        /// \brief Invalidates sync state because account identity or connection context changed.
        /// \param reason Human-readable reason for diagnostics.
        void invalidate_account_context(const char* reason);

        /// \brief Captures current account identity used by an outgoing snapshot request.
        /// \param account_info Current Intrade Bar account data.
        /// \return Account context token for stale callback filtering.
        AccountContext capture_account_context(
            const std::shared_ptr<AccountInfoData>& account_info) const;

        /// \brief Checks that a callback still belongs to the current account context.
        /// \param expected Context captured when the request was sent.
        /// \param current Current Intrade Bar account data.
        /// \return True if the callback may update the trade queue.
        bool is_account_context_current(
            const AccountContext& expected,
            const std::shared_ptr<AccountInfoData>& current) const;

        /// \brief Handles updated auth/settings data.
        /// \param event Auth data event.
        void handle_event(const events::AuthDataEvent& event);

        /// \brief Handles restart-auth signals as suspicious-account-state triggers.
        /// \param event Restart-auth event.
        void handle_event(const events::RestartAuthEvent& event);

        /// \brief Handles disconnect requests.
        /// \param event Disconnect request event.
        void handle_event(const events::DisconnectRequestEvent& event);

        /// \brief Handles account info state changes.
        /// \param event Account info update event.
        void handle_event(const events::AccountInfoUpdateEvent& event);

        /// \brief Handles snapshot refresh requests from the trade queue.
        /// \param event Refresh request event.
        void handle_event(const events::OpenTradesSnapshotRefreshRequestEvent& event);

        /// \brief Retrieves the account information as an AccountInfoData instance.
        /// \return A shared pointer to AccountInfoData.
        std::shared_ptr<AccountInfoData> get_account_info();
    };

    inline void ActiveTradesSyncManager::on_event(const utils::Event* const event) {
        if (const auto* msg = dynamic_cast<const events::AuthDataEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::RestartAuthEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::DisconnectRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::AccountInfoUpdateEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::OpenTradesSnapshotRefreshRequestEvent*>(event)) {
            handle_event(*msg);
        }
    }

    inline void ActiveTradesSyncManager::process() {
        m_task_manager.process();
    }

    inline void ActiveTradesSyncManager::shutdown() {
        m_task_manager.shutdown();
        m_refresh_scheduled = false;
        reset_sync_state();
    }

    inline void ActiveTradesSyncManager::request_sync(std::string reason) {
        if (m_sync_in_progress) {
            LOGIT_DEBUG(
                "Intrade Bar active trades sync: snapshot already in progress. reason=",
                reason);
            schedule_sync(std::move(reason));
            return;
        }

        const auto account_info = get_account_info();
        if (!account_info->connect) {
            LOGIT_DEBUG(
                "Intrade Bar active trades sync: skipped for disconnected account. reason=",
                reason);
            return;
        }

        m_sync_in_progress = true;
        m_refresh_scheduled = false;
        notify(events::OpenTradesSnapshotAvailabilityEvent(false, reason));
        const std::uint64_t generation = ++m_sync_generation;
        m_active_sync_generation = generation;
        const AccountContext account_context = capture_account_context(account_info);
        LOGIT_INFO(
            "Intrade Bar active trades sync: requesting broker snapshot. reason=",
            reason);

        m_request_manager.request_active_trades_snapshot_result(
            [this, reason, generation, account_context](ActiveTradesSnapshotResult result) {
                if (generation != m_active_sync_generation) return;
                m_active_sync_generation = 0;
                m_sync_in_progress = false;
                const auto account_info = get_account_info();
                if (!account_info->connect) {
                    LOGIT_DEBUG(
                        "Intrade Bar active trades sync: stale snapshot ignored for disconnected account. reason=",
                        reason);
                    return;
                }
                if (!is_account_context_current(account_context, account_info)) {
                    LOGIT_DEBUG(
                        "Intrade Bar active trades sync: stale snapshot ignored because account context changed. reason=",
                        reason,
                        ", expected_user_id=",
                        account_context.user_id,
                        ", current_user_id=",
                        account_info->user_id,
                        ", expected_account_type=",
                        to_str(account_context.account_type),
                        ", current_account_type=",
                        to_str(account_info->account_type),
                        ", expected_currency=",
                        to_str(account_context.currency),
                        ", current_currency=",
                        to_str(account_info->currency));
                    return;
                }
                if (!result) {
                    LOGIT_WARN(
                        "Intrade Bar active trades sync: snapshot request failed. reason=",
                        reason,
                        ", status_code=",
                        result.status_code,
                        ", error=",
                        result.error_message);
                    schedule_sync("snapshot-failed");
                    return;
                }

                std::vector<int64_t> close_times_ms;
                close_times_ms.reserve(result.value.trades.size());
                for (const auto& trade : result.value.trades) {
                    if (trade.close_time_ms > 0) {
                        close_times_ms.push_back(trade.close_time_ms);
                    }
                }

                LOGIT_INFO(
                    "Intrade Bar active trades sync: snapshot received. reason=",
                    reason,
                    ", active_trades=",
                    result.value.trades.size(),
                    ", close_times=",
                    close_times_ms.size(),
                    ", close_buffer_ms=",
                    m_active_trades_close_buffer_ms);

                notify(events::OpenTradesSnapshotEvent(
                    static_cast<int64_t>(result.value.trades.size()),
                    std::move(close_times_ms),
                    m_active_trades_close_buffer_ms));
            });
    }

    inline void ActiveTradesSyncManager::reset_sync_state() {
        m_sync_in_progress = false;
        m_active_sync_generation = 0;
        ++m_sync_generation;
    }

    inline void ActiveTradesSyncManager::invalidate_account_context(const char* reason) {
        ++m_account_context_generation;
        m_task_manager.shutdown();
        m_refresh_scheduled = false;
        reset_sync_state();
        notify(events::OpenTradesSnapshotAvailabilityEvent(
            false,
            reason ? reason : "account-context-invalidated"));
        LOGIT_DEBUG(
            "Intrade Bar active trades sync: account context invalidated. reason=",
            reason,
            ", generation=",
            m_account_context_generation);
    }

    inline ActiveTradesSyncManager::AccountContext
    ActiveTradesSyncManager::capture_account_context(
            const std::shared_ptr<AccountInfoData>& account_info) const {
        AccountContext context;
        context.user_id = account_info->user_id;
        context.account_type = account_info->account_type;
        context.currency = account_info->currency;
        context.generation = m_account_context_generation;
        return context;
    }

    inline bool ActiveTradesSyncManager::is_account_context_current(
            const AccountContext& expected,
            const std::shared_ptr<AccountInfoData>& current) const {
        return expected.generation == m_account_context_generation &&
            expected.user_id == current->user_id &&
            expected.account_type == current->account_type &&
            expected.currency == current->currency;
    }

    inline void ActiveTradesSyncManager::schedule_sync(std::string reason) {
        if (m_refresh_scheduled) {
            LOGIT_DEBUG(
                "Intrade Bar active trades sync: refresh already scheduled. reason=",
                reason);
            return;
        }

        const auto account_info = get_account_info();
        if (!account_info->connect) {
            LOGIT_DEBUG(
                "Intrade Bar active trades sync: refresh skipped for disconnected account. reason=",
                reason);
            return;
        }

        LOGIT_INFO(
            "Intrade Bar active trades sync: scheduling broker snapshot refresh. reason=",
            reason,
            ", delay_ms=",
            m_active_trades_sync_period_ms);
        const std::string log_reason = reason;
        m_refresh_scheduled = m_task_manager.add_delayed_task(
            "active-trades-sync-refresh",
            m_active_trades_sync_period_ms,
            [this, reason = std::move(reason)](std::shared_ptr<utils::Task> task) mutable {
                m_refresh_scheduled = false;
                if (task->is_shutdown()) return;
                request_sync(std::move(reason));
            });
        if (!m_refresh_scheduled) {
            LOGIT_WARN(
                "Intrade Bar active trades sync: failed to schedule broker snapshot refresh. reason=",
                log_reason);
        }
    }

    inline void ActiveTradesSyncManager::handle_event(const events::AuthDataEvent& event) {
        if (auto auth_data = std::dynamic_pointer_cast<AuthData>(event.auth_data)) {
            m_active_trades_close_buffer_ms = auth_data->active_trades_close_buffer_ms;
            m_active_trades_sync_period_ms = auth_data->active_trades_sync_period_ms;
            const auto account_info = get_account_info();
            if (account_info->connect &&
                ((auth_data->account_type != AccountType::UNKNOWN &&
                  auth_data->account_type != account_info->account_type) ||
                 (auth_data->currency != CurrencyType::UNKNOWN &&
                  auth_data->currency != account_info->currency))) {
                invalidate_account_context("auth-data-account-context-change");
            }
            LOGIT_INFO(
                "Intrade Bar active trades sync: configured close buffer ms=",
                m_active_trades_close_buffer_ms,
                ", sync_period_ms=",
                m_active_trades_sync_period_ms);
        }
    }

    inline void ActiveTradesSyncManager::handle_event(const events::RestartAuthEvent& event) {
        request_sync("restart-auth");
    }

    inline void ActiveTradesSyncManager::handle_event(const events::DisconnectRequestEvent& event) {
        invalidate_account_context("disconnect-request");
    }

    inline void ActiveTradesSyncManager::handle_event(const events::AccountInfoUpdateEvent& event) {
        using Status = events::AccountInfoUpdateEvent::Status;
        if (event.status == Status::CONNECTED) {
            request_sync("connected");
        } else
        if (event.status == Status::ACCOUNT_TYPE_CHANGED ||
            event.status == Status::CURRENCY_CHANGED) {
            invalidate_account_context("account-settings-changed");
            request_sync("account-settings-changed");
        } else
        if (event.status == Status::DISCONNECTED ||
            event.status == Status::FAILED_TO_CONNECT) {
            invalidate_account_context("account-disconnected");
        }
    }

    inline void ActiveTradesSyncManager::handle_event(
            const events::OpenTradesSnapshotRefreshRequestEvent& event) {
        schedule_sync(event.reason.empty() ? "refresh-request" : event.reason);
    }

    inline std::shared_ptr<AccountInfoData> ActiveTradesSyncManager::get_account_info() {
        if (auto account_info = std::dynamic_pointer_cast<AccountInfoData>(m_account_info)) {
            return account_info;
        }
        LOGIT_FATAL("Failed to cast IAccountInfoData to AccountInfoData");
        throw std::runtime_error("Invalid account information type");
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_ACTIVE_TRADES_SYNC_MANAGER_HPP_INCLUDED
