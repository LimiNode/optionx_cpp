#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_TRADE_MANAGER_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_TRADE_MANAGER_HPP_INCLUDED

/// \file TradeManager.hpp
/// \brief Implements trade execution, trade status tracking, and balance updates.

namespace optionx::platforms::intrade_bar {

    /// \class TradeManager
    /// \brief Manages trade execution, trade status updates, and account balance tracking.
    ///
    /// This component is responsible for handling trade requests, monitoring trade status,
    /// and ensuring the account balance is correctly updated after each trade.
    class TradeManager final : public components::BaseComponent {
    public:
        using trade_result_check_callback_t = BaseTradingPlatform::trade_result_check_callback_t;
        using trade_history_callback_t = BaseTradingPlatform::trade_history_callback_t;
        using lifecycle_token_t = events::trade_lifecycle_token_t;

        /// \brief Constructs the trade manager.
        /// \param platform Reference to the trading platform.
        /// \param request_manager Reference to the request manager handling HTTP requests.
        /// \param account_info Shared pointer to account information structure.
        explicit TradeManager(
                BaseTradingPlatform& platform,
                RequestManager& request_manager,
                std::shared_ptr<BaseAccountInfoData> account_info)
                : BaseComponent(platform.event_bus()),
                  m_request_manager(request_manager),
                  m_account_info(std::move(account_info))  {
            subscribe<events::TradeRequestEvent>();
            subscribe<events::TradeStatusEvent>();
            subscribe<events::OpenTradesEvent>();
            platform.register_component(this);
        }

        /// \brief Default destructor.
        virtual ~TradeManager() = default;

        /// \brief Processes incoming events and dispatches them to appropriate handlers.
        /// \param event The received event.
        void on_event(const utils::Event* const event) override;

        /// \brief Executes periodic tasks related to trade management.
        void process() override;

        /// \brief Shuts down the trade manager and cleans up resources.
        void shutdown() override;

        /// \brief Requests closed trade history for the currently selected account.
        /// \param request History range and timestamp field.
        /// \param callback Callback receiving parsed trade history or an error.
        /// \return True if the history request was accepted for processing.
        bool fetch_trade_history(
                const TradeHistoryRequest& request,
                trade_history_callback_t callback);
        /// \brief Requests the final result for a previously opened Intrade Bar trade.
        /// \param query Broker-side trade identity and retry settings.
        /// \param result Partially filled result object to update.
        /// \param callback Callback receiving the updated result.
        /// \return True if the check was accepted or completed with a validation result.
        bool fetch_trade_result(
                TradeResultQuery query,
                std::unique_ptr<TradeResult> result,
                trade_result_check_callback_t callback);

    private:
        RequestManager&    m_request_manager; ///< Reference to the request manager.
        utils::TaskManager m_task_manager;    ///< Task manager for handling asynchronous tasks.
        std::shared_ptr<BaseAccountInfoData> m_account_info; ///< Shared pointer to account information.

        /// \brief Handles a trade request event.
        /// \param event The trade request event.
        void handle_event(const events::TradeRequestEvent& event);

        /// \brief Handles a trade status update event.
        /// \param event The trade status event.
        void handle_event(const events::TradeStatusEvent& event);

        /// \brief Handles an OpenTradesEvent.
        /// \param event The OpenTradesEvent containing the number of open trades.
        void handle_event(const events::OpenTradesEvent& event);

        /// \brief Processes trade status based on retrieved price and profit.
        /// \param success Indicates whether the trade check was successful.
        /// \param price The closing price of the trade.
        /// \param profit The profit or loss from the trade.
        /// \param balance The updated account balance after the trade.
        /// \param request The original trade request.
        /// \param result The trade result containing updated trade details.
        void process_trade_status(
                bool success,
                double price,
                double profit,
                double balance,
                std::shared_ptr<TradeRequest> request,
                std::shared_ptr<TradeResult> result,
                const lifecycle_token_t& lifecycle_token);

        /// \brief Returns whether a transaction may still be updated asynchronously.
        /// \param lifecycle_token Token propagated from the transaction queue.
        /// \return True for active tokens and legacy events without a token.
        static bool async_operations_active(
                const lifecycle_token_t& lifecycle_token) noexcept {
            return !lifecycle_token ||
                lifecycle_token->load(std::memory_order_acquire);
        }

        /// \brief Retrieves account information as an `AccountInfoData` instance.
        /// \return A shared pointer to `AccountInfoData` containing account details.
        std::shared_ptr<AccountInfoData> get_account_info();
    };

    inline void TradeManager::on_event(const utils::Event* const event) {
        if (const auto* msg = dynamic_cast<const events::TradeRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::TradeStatusEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::OpenTradesEvent*>(event)) {
            handle_event(*msg);
        }
    };

    inline void TradeManager::process() {
        m_task_manager.process();
    }

    inline void TradeManager::shutdown() {
        m_task_manager.shutdown();
    }

    inline bool TradeManager::fetch_trade_history(
            const TradeHistoryRequest& request,
            trade_history_callback_t callback) {
        if (!callback || !request.has_valid_range()) return false;

        const AccountType account_type = get_account_info()->account_type;
        if (account_type == AccountType::UNKNOWN) {
            callback(TradeHistoryResult::fail("Current account type is unknown."));
            return true;
        }

        m_request_manager.request_trade_history_result(
            request,
            account_type,
            [callback = std::move(callback)](TradeHistoryApiResult history_result) mutable {
                if (!callback) return;
                if (!history_result) {
                    callback(TradeHistoryResult::fail(
                        std::move(history_result.error_message),
                        history_result.status_code));
                    return;
                }
                callback(TradeHistoryResult::ok(
                    std::move(history_result.value.records),
                    history_result.status_code));
            });
        return true;
    }
    inline bool TradeManager::fetch_trade_result(
            TradeResultQuery query,
            std::unique_ptr<TradeResult> result,
            trade_result_check_callback_t callback) {
        if (!result || !callback) return false;

        auto shared_result = std::shared_ptr<TradeResult>(std::move(result));
        if (query.trade_id == 0) query.trade_id = shared_result->trade_id;
        if (query.option_id == 0) query.option_id = shared_result->option_id;
        if (query.option_hash.empty()) query.option_hash = shared_result->option_hash;

        if (shared_result->trade_id == 0) shared_result->trade_id = query.trade_id;
        if (shared_result->option_id == 0) shared_result->option_id = query.option_id;
        if (shared_result->option_hash.empty()) shared_result->option_hash = query.option_hash;
        shared_result->platform_type = PlatformType::INTRADE_BAR;

        auto callback_ptr = std::make_shared<trade_result_check_callback_t>(std::move(callback));
        auto complete = [shared_result, callback_ptr]() {
            if (*callback_ptr) {
                (*callback_ptr)(shared_result->clone_unique());
            }
        };

        if (!query.has_broker_identity()) {
            shared_result->trade_state = shared_result->live_state = TradeState::CHECK_ERROR;
            shared_result->error_code = TradeErrorCode::INVALID_REQUEST;
            shared_result->error_desc = "Broker trade identity is missing.";
            complete();
            return true;
        }

        if (query.option_id <= 0) {
            shared_result->trade_state = shared_result->live_state = TradeState::CHECK_ERROR;
            shared_result->error_code = TradeErrorCode::INVALID_REQUEST;
            shared_result->error_desc = "Intrade Bar trade result check requires numeric option_id.";
            complete();
            return true;
        }

        const int retry_attempts = query.retry_attempts < 0 ? 0 : query.retry_attempts;
        LOGIT_INFO("Intrade Bar trade: fetching result by option_id=", query.option_id);
        m_request_manager.request_trade_check_result(
            query.option_id,
            retry_attempts,
            [this, shared_result, callback_ptr](TradeCheckResult check_result) {
                if (!check_result) {
                    auto account_info = get_account_info();
                    shared_result->trade_state = shared_result->live_state = TradeState::CHECK_ERROR;
                    shared_result->error_code = TradeErrorCode::PARSING_ERROR;
                    if (check_result.status_code == 451) {
                        shared_result->error_desc = "Trade result blocked (HTTP 451 - Unavailable For Legal Reasons).";
                    } else {
                        shared_result->error_desc = check_result.error_message.empty()
                            ? "Failed to retrieve trade result."
                            : check_result.error_message;
                    }
                    if (check_result.status_code == 451 && account_info->connect) {
                        LOGIT_0ERROR();
                        account_info->connect = false;
                        using Status = events::AccountInfoUpdateEvent::Status;
                        notify(events::AccountInfoUpdateEvent(account_info, Status::DISCONNECTED, "HTTP 451 - Unavailable For Legal Reasons."));
                    }
                    if (*callback_ptr) {
                        (*callback_ptr)(shared_result->clone_unique());
                    }
                    return;
                }

                if (!apply_trade_check_info_to_result(check_result.value, *shared_result)) {
                    if (*callback_ptr) {
                        (*callback_ptr)(shared_result->clone_unique());
                    }
                    return;
                }

                m_request_manager.request_balance(
                    [this, shared_result, callback_ptr](
                            bool success,
                            double balance,
                            CurrencyType currency) {
                        auto account_info = get_account_info();
                        if (success) {
                            using Status = events::AccountInfoUpdateEvent::Status;
                            const double previous_balance = account_info->balance;
                            shared_result->set_balance(balance);
                            if (shared_result->currency == CurrencyType::UNKNOWN) {
                                shared_result->currency = currency;
                            }

                            if (!account_info->connect) {
                                account_info->connect = true;
                                notify(events::AccountInfoUpdateEvent(account_info, Status::CONNECTED));
                            }

                            const double balance_tolerance = 0.01;
                            if (std::abs(previous_balance - balance) > balance_tolerance) {
                                account_info->balance = balance;
                                notify(events::AccountInfoUpdateEvent(account_info, Status::BALANCE_UPDATED));
                            }
                        } else if (!shared_result->has_balance()) {
                            shared_result->set_balance(account_info->balance);
                        }
                        if (!shared_result->has_close_balance() &&
                            shared_result->has_open_balance() &&
                            optionx::is_result_state(shared_result->trade_state)) {
                            shared_result->set_close_balance(
                                shared_result->open_balance + shared_result->profit);
                        }

                        if (*callback_ptr) {
                            (*callback_ptr)(shared_result->clone_unique());
                        }
                    });
            });
        return true;
    }

    inline void TradeManager::handle_event(const events::TradeRequestEvent& event) {
        auto request = event.request;
        auto result  = event.result;
        LOGIT_INFO(
            "Intrade Bar trade: opening request. symbol=",
            request ? request->symbol : std::string(),
            ", amount=",
            request ? request->amount : 0.0);

        m_request_manager.request_execute_trade(
                request, [this, request, result] (
                    bool success,
                    long status_code,
                    int64_t option_id,
                    int64_t open_date,
                    double open_price,
                    const std::string& error_desc) {
            const int64_t timestamp = OPTIONX_TIMESTAMP_MS;
            auto account_info  = get_account_info();
            set_zero_spread_for_symbol(result->spread, request->symbol);
            if (!success) {
                LOGIT_WARN(
                    "Intrade Bar trade: open failed. status=",
                    status_code,
                    ", error=",
                    error_desc);
                result->trade_state = result->live_state = TradeState::OPEN_ERROR;
                result->error_code = TradeErrorCode::PARSING_ERROR;
                result->delay = timestamp - result->send_date;
                result->ping = result->delay / 2;
                result->open_date = timestamp;
                result->close_date = request->option_type == OptionType::SPRINT
                 ? timestamp + time_shield::sec_to_ms(request->duration)
                 : time_shield::sec_to_ms(request->expiry_time);
                if (status_code == 451) {
                    result->error_desc = "Trade request blocked (HTTP 451 - Unavailable For Legal Reasons).";
                } else {
                    result->error_desc = error_desc;
                }
                // If HTTP 451 and account is still connected, disconnect and notify.
                if (status_code == 451 && account_info->connect) {
                    LOGIT_0ERROR();
                    account_info->connect = false;
                    using Status = events::AccountInfoUpdateEvent::Status;
                    notify(events::AccountInfoUpdateEvent(account_info, Status::DISCONNECTED, "HTTP 451 - Unavailable For Legal Reasons."));
                }
                return;
            }

            result->option_id = option_id;
            LOGIT_INFO(
                "Intrade Bar trade: open accepted. option_id=",
                option_id,
                ", open_price=",
                open_price);
            result->open_date = open_date;
            result->close_date = request->option_type == OptionType::SPRINT
             ? open_date + time_shield::sec_to_ms(request->duration)
             : time_shield::sec_to_ms(request->expiry_time);
            result->delay = timestamp > result->open_date ?
                timestamp - result->open_date :
                timestamp - result->send_date;
            result->ping = result->delay / 2;
            result->open_price = result->close_price = open_price;
            result->live_state = TradeState::STANDOFF;
            result->error_code = TradeErrorCode::SUCCESS;
            result->payout     = account_info->get_for_trade<double>(AccountInfoType::PAYOUT, request, time_shield::ms_to_sec(result->open_date));

            m_request_manager.request_balance(
                    [this, request, result](
                    bool success,
                    double balance,
                    CurrencyType currency) {
                auto account_info = get_account_info();
                if (success) {
                    result->trade_state = TradeState::OPEN_SUCCESS;

                    using Status = events::AccountInfoUpdateEvent::Status;
                    const double previous_balance = account_info->balance;
                    result->set_balance(balance);
                    if (!result->has_open_balance()) {
                        result->set_open_balance(previous_balance);
                    }

                    if (!account_info->connect) {
                        account_info->connect = true;
                        notify(events::AccountInfoUpdateEvent(account_info, Status::CONNECTED));
                    }

                    const double balance_tolerance = 0.01;
                    if (std::abs(previous_balance - balance) > balance_tolerance) {
                        account_info->balance = balance;
                        notify(events::AccountInfoUpdateEvent(account_info, Status::BALANCE_UPDATED));
                    }
                } else {
                    result->set_balance(account_info->balance);
                    if (!result->has_open_balance()) {
                        result->set_open_balance(account_info->balance);
                    }
                    result->trade_state = TradeState::OPEN_SUCCESS;
                }
            });
        });
    }

    inline void TradeManager::handle_event(const events::TradeStatusEvent& event) {
        auto request = event.request;
        auto result  = event.result;
        auto lifecycle_token = event.lifecycle_token;
        if (!request || !result) {
            LOGIT_ERROR("TradeStatusEvent received with null request or result.");
            return;
        }
        if (!async_operations_active(lifecycle_token)) return;

        LOGIT_INFO("Intrade Bar trade: checking result. option_id=", result->option_id);
        LOGIT_0TRACE();
        // Give the broker a short settlement window before the first check.
        // Retries are rate-limited to about one request every two seconds.
        const int64_t delay_ms = 1500;
        m_task_manager.add_delayed_task(
                "event(TradeStatusEvent)-1500ms",
                delay_ms, 
                [this, request, result, lifecycle_token](
                    std::shared_ptr<utils::Task> task) {
            LOGIT_0TRACE();
            if (!async_operations_active(lifecycle_token)) return;
            if (task->is_shutdown()) {
                LOGIT_INFO("Task was shut down unexpectedly for option ID: ", result->option_id);
                auto account_info = get_account_info();
                result->payout = account_info->get_for_trade<double>(
                    AccountInfoType::PAYOUT,
                    request,
                    time_shield::ms_to_sec(result->open_date));
                result->profit =
                    result->live_state == TradeState::STANDOFF ? 0 :
                    (result->live_state == TradeState::WIN ?
                     result->payout * result->amount : -result->amount);
                result->trade_state = result->live_state = TradeState::CHECK_ERROR;
                result->error_code = TradeErrorCode::CLIENT_FORCED_CLOSE;
                result->error_desc = to_str(TradeErrorCode::CLIENT_FORCED_CLOSE);
                return;
            }
            // Fifteen retries after the initial request (up to sixteen total
            // requests) normally span about thirty seconds. Because the
            // broker-wide limit is shared by up to five closing trades, one
            // trade may receive a slot roughly every ten seconds. The queue
            // response timeout therefore allows about 150 seconds of polling
            // plus margin for delayed broker settlement.
            const int retry_attempts = 15;
            m_request_manager.request_trade_check(
                    result->option_id,
                    retry_attempts,
                    [this, request, result, lifecycle_token](
                    bool success,
                    long status_code,
                    double price,
                    double profit) {
                if (!async_operations_active(lifecycle_token)) return;
                if (!success) {
                    LOGIT_ERROR("Failed to retrieve trade result for option ID: ", result->option_id);
                    auto account_info = get_account_info();
                    result->payout = account_info->get_for_trade<double>(
                        AccountInfoType::PAYOUT,
                        request,
                        time_shield::ms_to_sec(result->open_date));
                    result->profit =
                        result->live_state == TradeState::STANDOFF ? 0 :
                        (result->live_state == TradeState::WIN ?
                         result->payout * result->amount : -result->amount);
                    result->trade_state = result->live_state = TradeState::CHECK_ERROR;
                    result->error_code = TradeErrorCode::PARSING_ERROR;
                    if (status_code == 451) {
                        result->error_desc = "Trade result blocked (HTTP 451 - Unavailable For Legal Reasons).";
                    } else {
                        result->error_desc = "Failed to retrieve trade result.";
                    }
                    // If HTTP 451 and account is still connected, disconnect and notify.
                    if (status_code == 451 && account_info->connect) {
                        LOGIT_0ERROR();
                        account_info->connect = false;
                        using Status = events::AccountInfoUpdateEvent::Status;
                        notify(events::AccountInfoUpdateEvent(account_info, Status::DISCONNECTED, "HTTP 451 - Unavailable For Legal Reasons."));
                    }
                    return;
                }
                LOGIT_0TRACE();
                if (!async_operations_active(lifecycle_token)) return;
                m_request_manager.request_balance(
                        [this, request, result, lifecycle_token, price, profit](
                        bool success,
                        double balance,
                        CurrencyType currency) {
                    if (!async_operations_active(lifecycle_token)) return;
                    process_trade_status(
                        success,
                        price,
                        profit,
                        balance,
                        request,
                        result,
                        lifecycle_token);
                });
            });
        });
    }

    inline void TradeManager::handle_event(const events::OpenTradesEvent& event) {
        using Status = events::AccountInfoUpdateEvent::Status;
        auto account_info = get_account_info();
        account_info->open_trades = event.open_trades;
        notify(events::AccountInfoUpdateEvent(account_info, Status::OPEN_TRADES_CHANGED));
    }

    inline void TradeManager::process_trade_status(
            bool success,
            double price,
            double profit,
            double balance,
            std::shared_ptr<TradeRequest> request,
            std::shared_ptr<TradeResult> result,
            const lifecycle_token_t& lifecycle_token) {
        if (!request || !result || !async_operations_active(lifecycle_token)) return;
        auto account_info = get_account_info();
        set_zero_spread_for_symbol(result->spread, request->symbol);
        result->set_balance(success ? balance : account_info->balance);
        if (!apply_trade_check_info_to_result(TradeCheckInfo{price, profit}, *result)) {
            return;
        }
        if (result->has_open_balance()) {
            result->set_close_balance(result->open_balance + result->profit);
        }

        if (result->trade_state == TradeState::STANDOFF ||
            result->trade_state == TradeState::LOSS) {
            result->payout = account_info->get_for_trade<double>(
                AccountInfoType::PAYOUT,
                request,
                time_shield::ms_to_sec(result->open_date));
        }

        if (success) {
            using Status = events::AccountInfoUpdateEvent::Status;
            const double balance_tolerance = 0.01;
            const double previous_balance = account_info->balance;

            if (!account_info->connect) {
                account_info->connect = true;
                notify(events::AccountInfoUpdateEvent(account_info, Status::CONNECTED));
            }

            if (std::abs(previous_balance - balance) > balance_tolerance) {
                account_info->balance = balance;
                notify(events::AccountInfoUpdateEvent(account_info, Status::BALANCE_UPDATED));
            }
        }
    }

    inline std::shared_ptr<AccountInfoData> TradeManager::get_account_info() {
        if (auto account_info = std::dynamic_pointer_cast<AccountInfoData>(m_account_info)) {
            return account_info;
        }
        LOGIT_FATAL("Failed to cast IAccountInfoData to AccountInfoData");
        throw std::runtime_error("Invalid account information type");
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_TRADE_MANAGER_HPP_INCLUDED
