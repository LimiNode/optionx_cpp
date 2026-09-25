#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_BALANCE_MANAGER_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_BALANCE_MANAGER_HPP_INCLUDED

/// \file BalanceManager.hpp
/// \brief Defines the BalanceManager class responsible for handling balance updates and account-related events.

namespace optionx::platforms::intrade_bar {

    /// \class BalanceManager
    /// \brief Manages balance updates and account-related events.
    ///
    /// This class is responsible for retrieving balance information, monitoring account activity,
    /// and reacting to trade events to ensure account consistency. It subscribes to various events
    /// related to trading activity and account status changes.
    class BalanceManager final : public components::BaseComponent {
    public:

        /// \brief Constructs the BalanceManager.
        /// \param platform Reference to the trading platform.
        /// \param request_manager Reference to the request manager for making HTTP requests.
        /// \param account_info Shared pointer to the account information data.
        explicit BalanceManager(
            BaseTradingPlatform& platform,
            RequestManager& request_manager,
            std::shared_ptr<BaseAccountInfoData> account_info)
            : BaseComponent(platform.event_bus()), m_request_manager(request_manager),
              m_account_info(std::move(account_info))  {
            subscribe<events::ConnectRequestEvent>();
            subscribe<events::DisconnectRequestEvent>();
            subscribe<events::AuthDataEvent>();
            subscribe<events::BalanceRequestEvent>();
            subscribe<events::TradeRequestEvent>();
            subscribe<events::AccountInfoUpdateEvent>();
            m_request_time = m_last_trades_time =
                time_shield::ms_to_sec(OPTIONX_TIMESTAMP_MS);
            platform.register_component(this);
        }

        /// \brief Default destructor.
        virtual ~BalanceManager() = default;

        /// \brief Processes incoming events and dispatches them to the appropriate handlers.
        /// \param event The received event.
        void on_event(const utils::Event* const event) override;

        /// \brief Processes periodic balance updates and account-related tasks.
        void process() override;

        /// \brief Shuts down all active balance-related operations.
        void shutdown() override;

    private:
        RequestManager&    m_request_manager; ///< Reference to the request manager.
        utils::TaskManager m_task_manager;    ///< Task manager for handling async tasks.
        std::shared_ptr<BaseAccountInfoData> m_account_info; ///< Shared pointer to account information.
        int64_t m_last_trades_time;           ///< Timestamp of the last trade activity.
        int64_t m_request_time;               ///< Timestamp of the last balance request.
        int64_t m_balance_check_period_ms = time_shield::MS_PER_15_MIN; ///< Connected balance polling period.
        int64_t m_disconnected_domain_retry_period_ms = time_shield::MS_PER_15_SEC; ///< Disconnected host/domain recovery period.
        bool m_has_balance_update = false;    ///< Flag indicating if a balance update is in progress.
        bool m_check_host_in_progress = false;
        std::uint32_t m_consecutive_host_check_failures = 0; ///< Consecutive failed host checks while connected.
        static constexpr std::uint32_t kHostCheckFailureThreshold = 2;
        std::uint64_t m_balance_request_generation = 0; ///< Monotonic balance request generation.
        std::uint64_t m_active_balance_request_generation = 0; ///< Currently active balance request generation.
        std::uint64_t m_host_request_generation = 0; ///< Monotonic host/domain request generation.
        std::uint64_t m_active_host_request_generation = 0; ///< Currently active host/domain request generation.

        /// \brief Starts a balance request generation.
        /// \param reason Human-readable trigger for diagnostics.
        /// \return Generation token to capture in the request callback.
        std::uint64_t begin_balance_request(const char* reason);

        /// \brief Completes an active balance request if its generation is current.
        /// \param generation Generation token captured when the request was sent.
        /// \param reason Human-readable callback context for diagnostics.
        /// \return True if the callback may apply its result.
        bool finish_balance_request(
            std::uint64_t generation,
            const char* reason);

        /// \brief Starts a host/domain request generation.
        /// \param reason Human-readable trigger for diagnostics.
        /// \return Generation token to capture in request callbacks.
        std::uint64_t begin_host_request(const char* reason);

        /// \brief Checks whether a host/domain callback belongs to the active chain.
        /// \param generation Generation token captured when the request was sent.
        /// \param reason Human-readable callback context for diagnostics.
        /// \return True if the callback may continue the host/domain chain.
        bool is_host_request_current(
            std::uint64_t generation,
            const char* reason) const;

        /// \brief Completes an active host/domain request if its generation is current.
        /// \param generation Generation token captured when the request was sent.
        /// \param reason Human-readable callback context for diagnostics.
        /// \return True if the callback may apply its result.
        bool finish_host_request(
            std::uint64_t generation,
            const char* reason);

        /// \brief Invalidates pending balance and host/domain callbacks.
        /// \param reason Human-readable trigger for diagnostics.
        void invalidate_async_requests(const char* reason);

         /// \brief Initiates a balance update request.
        void handle_balance_update();

        /// \brief Processes a successful balance update.
        /// \param balance The updated balance value.
        /// \param currency The account currency type.
        /// \param account_info Shared pointer to the account information.
        void process_balance_success(
            double balance,
            CurrencyType currency,
            std::shared_ptr<AccountInfoData>& account_info);

        /// \brief Handles a failed balance update.
        /// \param account_info Shared pointer to the account information.
        void process_balance_failure(
            std::shared_ptr<AccountInfoData>& account_info);

        /// \brief Handles an event triggered when a connection request is received.
        /// \param event The connection request event.
        void handle_event(const events::ConnectRequestEvent& event);

        /// \brief Handles an event triggered when a disconnection request is received.
        /// \param event The disconnection request event.
        void handle_event(const events::DisconnectRequestEvent& event);

        /// \brief Handles an event triggered when a balance request is received.
        /// \param event The balance request event.
        void handle_event(const events::BalanceRequestEvent& event);

        /// \brief Handles updated auth/settings data.
        /// \param event The auth data event.
        void handle_event(const events::AuthDataEvent& event);

        /// \brief Handles an event triggered when a trade request is received.
        /// \param event The trade request event.
        void handle_event(const events::TradeRequestEvent& event);

        /// \brief Handles an account information update event.
        /// \param event The account info update event.
        void handle_event(const events::AccountInfoUpdateEvent& event);

        /// \brief Handles account connection events.
        void handle_connected();

        /// \brief Handles account disconnection events.
        void handle_disconnected();

        /// \brief Retrieves the account information as an `AccountInfoData` instance.
        /// \return A shared pointer to `AccountInfoData`.
        std::shared_ptr<AccountInfoData> get_account_info();
    };

    inline void BalanceManager::on_event(const utils::Event* const event) {
        if (const auto* msg = dynamic_cast<const events::ConnectRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::DisconnectRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::AuthDataEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::TradeRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::BalanceRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::AccountInfoUpdateEvent*>(event)) {
            handle_event(*msg);
        }
    };

    inline void BalanceManager::process() {
        m_task_manager.process();
    }

    inline void BalanceManager::shutdown() {
        invalidate_async_requests("shutdown");
        m_task_manager.shutdown();
    }

    inline std::uint64_t BalanceManager::begin_balance_request(const char* reason) {
        m_has_balance_update = true;
        const std::uint64_t generation = ++m_balance_request_generation;
        m_active_balance_request_generation = generation;
        LOGIT_DEBUG(
            "Intrade Bar balance: balance request started. reason=",
            reason,
            ", generation=",
            generation);
        return generation;
    }

    inline bool BalanceManager::finish_balance_request(
            std::uint64_t generation,
            const char* reason) {
        if (generation != m_active_balance_request_generation) {
            LOGIT_DEBUG(
                "Intrade Bar balance: stale balance callback ignored. reason=",
                reason,
                ", callback_generation=",
                generation,
                ", active_generation=",
                m_active_balance_request_generation);
            return false;
        }

        m_active_balance_request_generation = 0;
        m_has_balance_update = false;
        return true;
    }

    inline std::uint64_t BalanceManager::begin_host_request(const char* reason) {
        m_check_host_in_progress = true;
        const std::uint64_t generation = ++m_host_request_generation;
        m_active_host_request_generation = generation;
        LOGIT_DEBUG(
            "Intrade Bar balance: host/domain request started. reason=",
            reason,
            ", generation=",
            generation);
        return generation;
    }

    inline bool BalanceManager::is_host_request_current(
            std::uint64_t generation,
            const char* reason) const {
        const bool current = generation == m_active_host_request_generation;
        if (!current) {
            LOGIT_DEBUG(
                "Intrade Bar balance: stale host/domain callback ignored. reason=",
                reason,
                ", callback_generation=",
                generation,
                ", active_generation=",
                m_active_host_request_generation);
        }
        return current;
    }

    inline bool BalanceManager::finish_host_request(
            std::uint64_t generation,
            const char* reason) {
        if (!is_host_request_current(generation, reason)) {
            return false;
        }

        m_active_host_request_generation = 0;
        m_check_host_in_progress = false;
        return true;
    }

    inline void BalanceManager::invalidate_async_requests(const char* reason) {
        ++m_balance_request_generation;
        ++m_host_request_generation;
        m_active_balance_request_generation = 0;
        m_active_host_request_generation = 0;
        m_has_balance_update = false;
        m_check_host_in_progress = false;
        LOGIT_DEBUG(
            "Intrade Bar balance: async callbacks invalidated. reason=",
            reason,
            ", balance_generation=",
            m_balance_request_generation,
            ", host_generation=",
            m_host_request_generation);
    }

    // Handles balance updates.
    inline void BalanceManager::handle_balance_update() {
        if (m_has_balance_update) return;
        const std::uint64_t generation =
            begin_balance_request("balance-update");
        LOGIT_INFO("Intrade Bar balance: requesting balance snapshot.");
        m_request_manager.request_balance([this, generation](
                bool success,
                double balance,
                CurrencyType currency) {
            if (!finish_balance_request(generation, "balance-update")) {
                return;
            }
            auto account_info = get_account_info();
            if (!account_info) {
                LOGIT_ERROR("Failed to get account info.");
                return;
            }

            if (success) {
                LOGIT_INFO(
                    "Intrade Bar balance: snapshot received. currency=",
                    to_str(currency),
                    ", balance=",
                    balance);
                process_balance_success(balance, currency, account_info);
            } else {
                LOGIT_WARN("Intrade Bar balance: snapshot request failed.");
                process_balance_failure(account_info);
            }
        });
    }

    // Processes successful balance updates.
    inline void BalanceManager::process_balance_success(
            double balance,
            CurrencyType currency,
            std::shared_ptr<AccountInfoData>& account_info) {
        using Status = events::AccountInfoUpdateEvent::Status;
        const double previous_balance = account_info->balance;

        if (!account_info->connect) {
            account_info->connect = true;
            notify(events::AccountInfoUpdateEvent(account_info, Status::CONNECTED));
        }

        int64_t elapsed = time_shield::ms_to_sec(OPTIONX_TIMESTAMP_MS) - m_last_trades_time;

        const double balance_tolerance = 0.01;
        const int64_t max_elapsed = 3 * time_shield::SEC_PER_MIN;
        if ((account_info->currency != currency) ||
            (std::abs(previous_balance - balance) > balance_tolerance &&
            elapsed >= max_elapsed)) {
            notify(events::RestartAuthEvent());
        } else
        if (std::abs(previous_balance - balance) > balance_tolerance) {
            account_info->balance = balance;
            notify(events::AccountInfoUpdateEvent(account_info, Status::BALANCE_UPDATED));
        }
    }

    // Processes balance update failure.
    inline void BalanceManager::process_balance_failure(
            std::shared_ptr<AccountInfoData>& account_info) {
        if (account_info->connect) {
            account_info->connect = false;
            using Status = events::AccountInfoUpdateEvent::Status;
            const std::string error_text("Failed to retrieve balance.");
            LOGIT_ERROR(error_text);
            notify(events::AccountInfoUpdateEvent(account_info, Status::DISCONNECTED, error_text));
        }
    }

    inline void BalanceManager::handle_event(const events::BalanceRequestEvent& event) {
        m_last_trades_time = time_shield::ms_to_sec(OPTIONX_TIMESTAMP_MS);
        handle_balance_update();
    }

    inline void BalanceManager::handle_event(const events::AuthDataEvent& event) {
        if (auto auth_data = std::dynamic_pointer_cast<AuthData>(event.auth_data)) {
            invalidate_async_requests("auth-data");
            if (auth_data->balance_check_period_ms > 0) {
                m_balance_check_period_ms = auth_data->balance_check_period_ms;
                LOGIT_INFO(
                    "Intrade Bar balance: configured balance check period ms=",
                    m_balance_check_period_ms);
            }
            if (auth_data->disconnected_domain_retry_period_ms > 0) {
                m_disconnected_domain_retry_period_ms =
                    auth_data->disconnected_domain_retry_period_ms;
                LOGIT_INFO(
                    "Intrade Bar balance: configured disconnected domain retry period ms=",
                    m_disconnected_domain_retry_period_ms);
            }
        }
    }

    inline void BalanceManager::handle_event(const events::TradeRequestEvent& event) {
        m_last_trades_time = time_shield::ms_to_sec(OPTIONX_TIMESTAMP_MS);
        auto request = event.request;
        //auto result  = event.result;
        if (request->option_type == OptionType::SPRINT) {
            m_last_trades_time += request->duration;
        } else {
            if (request->duration > 0) {
                m_last_trades_time += request->duration;
            } else {
                m_last_trades_time = request->expiry_time;
            }
        }
        const int64_t max_elapsed = 3 * time_shield::SEC_PER_MIN;
        m_last_trades_time += max_elapsed;
    }

    inline void BalanceManager::handle_event(const events::ConnectRequestEvent& event) {
        LOGIT_TRACE0();
        invalidate_async_requests("connect-request");
        m_task_manager.shutdown();
    }

    inline void BalanceManager::handle_event(const events::DisconnectRequestEvent& event) {
        LOGIT_TRACE0();
        invalidate_async_requests("disconnect-request");
        m_task_manager.shutdown();
    }

    inline void BalanceManager::handle_event(const events::AccountInfoUpdateEvent& event) {
        using Status = events::AccountInfoUpdateEvent::Status;
        if (event.status == Status::CONNECTED) {
            handle_connected();
        } else
        if (event.status == Status::DISCONNECTED) {
            handle_disconnected();
        }
    }

    /// \brief Handles account connection event.
    inline void BalanceManager::handle_connected() {
        LOGIT_TRACE0();

        m_task_manager.shutdown();
        invalidate_async_requests("connected");
        m_consecutive_host_check_failures = 0;
        
        LOGIT_INFO(
            "Intrade Bar balance: starting connected balance polling. period_ms=",
            m_balance_check_period_ms);

        m_task_manager.add_periodic_task(
                "connected-balance-check",
                m_balance_check_period_ms,
                [this](std::shared_ptr<utils::Task> task){
            if (task->is_shutdown()) return;
            LOGIT_TRACE0();
            handle_balance_update();
        });
        
        m_task_manager.add_periodic_task(
                "connected-15sec",
                time_shield::MS_PER_15_SEC,
                [this](std::shared_ptr<utils::Task> task){
            if (task->is_shutdown()) return;
            if (m_check_host_in_progress) return;
            const std::uint64_t generation =
                begin_host_request("connected-host-check");
            
            LOGIT_TRACE0();
            m_request_manager.request_check_current_host_available([this, generation](
                    bool success) {
                LOGIT_TRACE0();
                if (!finish_host_request(generation, "connected-host-check")) {
                    return;
                }
                if (success) {
                    if (m_consecutive_host_check_failures != 0) {
                        LOGIT_INFO(
                            "Intrade Bar balance: current host check recovered after failures=",
                            m_consecutive_host_check_failures);
                    }
                    m_consecutive_host_check_failures = 0;
                    return;
                }

                ++m_consecutive_host_check_failures;
                LOGIT_WARN(
                    "Intrade Bar balance: current host check failed. consecutive_failures=",
                    m_consecutive_host_check_failures,
                    "; disconnect_threshold=",
                    kHostCheckFailureThreshold);
                if (m_consecutive_host_check_failures < kHostCheckFailureThreshold) {
                    return;
                }

                auto account_info = get_account_info();
                if (account_info->connect) {
                    account_info->connect = false;
                    using Status = events::AccountInfoUpdateEvent::Status;
                    const std::string error_text("Ping to current host failed.");
                    LOGIT_ERROR(error_text);
                    notify(events::AccountInfoUpdateEvent(account_info, Status::DISCONNECTED, error_text));
                }
            });
        });
    }

    /// \brief Handles account disconnection event.
    inline void BalanceManager::handle_disconnected() {
        LOGIT_TRACE0();

        m_task_manager.shutdown();
        invalidate_async_requests("disconnected");
        m_consecutive_host_check_failures = 0;
        LOGIT_INFO(
            "Intrade Bar balance: starting disconnected host recovery. period_ms=",
            m_disconnected_domain_retry_period_ms);
        m_task_manager.add_periodic_task(
                "disconnected-domain-retry",
                m_disconnected_domain_retry_period_ms,
                [this](std::shared_ptr<utils::Task> task) {
            LOGIT_TRACE0();
            if (task->is_shutdown()) return;
            if (m_check_host_in_progress) return;
            const std::uint64_t generation =
                begin_host_request("disconnected-domain-retry");
            
            LOGIT_TRACE0();
            m_request_manager.request_check_current_host_available([this, generation](
                    bool success) {
                if (!is_host_request_current(generation, "disconnected-host-check")) {
                    return;
                }
                if (success) {
                    if (!finish_host_request(generation, "disconnected-host-check")) {
                        return;
                    }
                    handle_balance_update();
                    return;
                }
                
                m_request_manager.request_find_working_domain(
                        [this, generation](bool success, std::string& host) {
                    if (!finish_host_request(generation, "disconnected-domain-find")) {
                        return;
                    }
                    if (!success) {
                        return;
                    }

                    notify(events::AutoDomainSelectedEvent(
                        success,
                        host));
                                
                    handle_balance_update();
                });
            });
        });
    }

    inline std::shared_ptr<AccountInfoData> BalanceManager::get_account_info() {
        if (auto account_info = std::dynamic_pointer_cast<AccountInfoData>(m_account_info)) {
            return account_info;
        }
        LOGIT_FATAL("Failed to cast IAccountInfoData to AccountInfoData");
        throw std::runtime_error("Invalid account information type");
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_BALANCE_MANAGER_HPP_INCLUDED
