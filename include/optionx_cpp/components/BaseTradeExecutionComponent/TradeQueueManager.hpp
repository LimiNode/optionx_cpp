#pragma once
#ifndef OPTIONX_HEADER_COMPONENTS_BASE_TRADE_EXECUTION_COMPONENT_TRADE_QUEUE_MANAGER_HPP_INCLUDED
#define OPTIONX_HEADER_COMPONENTS_BASE_TRADE_EXECUTION_COMPONENT_TRADE_QUEUE_MANAGER_HPP_INCLUDED

/// \file TradeQueueManager.hpp
/// \brief Manages the trade request queue, processes transactions, and handles trade events.

namespace optionx::components {

    /// \class TradeQueueManager
    /// \brief Manages the queue of trade transactions and processes them accordingly.
    ///
    /// ### Subscribed events:
    /// - `PriceUpdateEvent`: Updates trade states based on market price movements.
    /// - `DisconnectRequestEvent`: Handles connection loss and finalizes active trades.
    /// - `OpenTradesSnapshotEvent`: Synchronizes broker-side active trades when the local queue is idle.
    ///
    /// ### Emitted events:
    /// - `TradeRequestEvent`: Notifies when a trade request is sent.
    /// - `TradeStatusEvent`: Reports trade state changes.
    /// - `OpenTradesEvent`: Notifies about open trade count updates.
    ///
    /// ### Threading contract:
    /// - `add_trade()` is the only supported external enqueue entry point, but
    ///   it synchronizes only final insertion into the pending queue. The caller
    ///   must ensure the trade ID provider, account info access, and preprocess
    ///   callback are safe from the calling thread.
    /// - `process()`, `finalize_all_trades()`, and event handlers are platform
    ///   event-loop owned. Local open-trade counters, broker snapshot counters,
    ///   and active transaction lists must not be driven concurrently from
    ///   another thread.
    class TradeQueueManager : public utils::EventMediator {
    public:
        using transaction_t = std::shared_ptr<events::TradeTransactionEvent>;
        using trade_result_callback_t = std::function<void(std::unique_ptr<TradeRequest>, std::unique_ptr<TradeResult>)>;
        using trade_id_provider_t = std::function<std::uint32_t()>;

        /// \brief Constructs a TradeQueueManager instance.
        /// \param bus Event bus for event-based communication.
        /// \param account_info Account information provider.
        /// \param trade_state_manager Manages trade state transitions.
        /// \param require_open_trades_snapshot Start with trade admission blocked
        ///        until an authoritative broker snapshot is accepted.
        explicit TradeQueueManager(
                utils::EventBus& bus,
                AccountInfoProvider& account_info,
                TradeStateManager& trade_state_manager,
                bool require_open_trades_snapshot = false)
            : EventMediator(bus), m_account_info(account_info),
              m_trade_state_manager(trade_state_manager),
              m_require_open_trades_snapshot(require_open_trades_snapshot),
              m_open_trades_snapshot_ready(!require_open_trades_snapshot) {
            subscribe<events::PriceUpdateEvent>();
            subscribe<events::DisconnectRequestEvent>();
            subscribe<events::OpenTradesSnapshotEvent>();
            subscribe<events::OpenTradesSnapshotAvailabilityEvent>();
        }

        /// \brief Virtual destructor.
        virtual ~TradeQueueManager() = default;

        /// \brief Handles an event notification received as a raw pointer.
        /// \param event The received event.
        void on_event(const utils::Event* const event) override {
            if (const auto* msg = dynamic_cast<const events::PriceUpdateEvent*>(event)) {
                handle_event(*msg);
            } else
            if (const auto* msg = dynamic_cast<const events::DisconnectRequestEvent*>(event)) {
                handle_event(*msg);
            } else
            if (const auto* msg = dynamic_cast<const events::OpenTradesSnapshotEvent*>(event)) {
                handle_event(*msg);
            } else
            if (const auto* msg = dynamic_cast<const events::OpenTradesSnapshotAvailabilityEvent*>(event)) {
                handle_event(*msg);
            }
        };

        /// \brief Adds a trade request to the processing queue.
        /// \tparam PreprocessFunctor Type of the preprocessing functor.
        /// \param request Unique pointer to a trade request.
        /// \param platform_type The platform type associated with the request.
        /// \param preprocess Functor to preprocess the trade request and result.
        /// \return True if the trade request was successfully added; false otherwise.
        template<typename PreprocessFunction>
        bool add_trade(
            std::unique_ptr<TradeRequest> request,
            PlatformType platform_type,
            PreprocessFunction preprocess);

        /// \brief Sets a callback for trade result events.
        /// \param callback Function to handle trade result events.
        void set_trade_result_callback(trade_result_callback_t callback);

        /// \brief
        /// \return
        trade_result_callback_t& on_trade_result() {
            return m_trade_result_callback;
        }

        /// \brief Returns provider used to initialize empty TradeRequest::trade_id values.
        /// \return Mutable provider callback. Return 0 from the provider to use the local fallback generator.
        trade_id_provider_t& on_trade_id() {
            return m_trade_id_provider;
        }

        /// \brief Processes all pending, closing, and finalizing transactions.
        virtual void process() {
            process_snapshot_open_trades();
            process_pending_transactions();
            process_closing_transactions();
            process_finalizing_transactions();
        }

         /// \brief Finalizes all active and pending trades.
        void finalize_all_trades();

    protected:

        /// \brief Retrieves and removes the next transaction from the pending queue.
        /// \return The next transaction to process, or nullptr if none available.
        transaction_t pop_next_transaction();

        /// \brief Handles all canceled transactions.
        void handle_canceled_transactions(std::list<transaction_t>& canceled_transactions);

        /// \brief Handles an error when closing a transaction.
        void handle_closing_error(
                const transaction_t& transaction,
                int64_t timestamp);

        /// \brief Processes all pending transactions.
        void process_pending_transactions();

        /// \brief Processes transactions that need to be closed.
        void process_closing_transactions();

        /// \brief Processes transactions that need to be finalized.
        void process_finalizing_transactions();

        /// \brief Handles an error while closing a trade.
        /// \param transaction The transaction that encountered an error.
        /// \param timestamp Current timestamp.
        void clean_expired_transactions(
                int64_t current_time_ms,
                std::list<transaction_t>& canceled_transactions);

        /// \brief Increments the open trades counter and emits an OpenTradesEvent.
        void increment_open_trades(
                const std::shared_ptr<TradeRequest>& request,
                const std::shared_ptr<TradeResult>& result);

        /// \brief Decrements the open trades counter and emits an OpenTradesEvent.
        void decrement_open_trades(
                const std::shared_ptr<TradeRequest>& request,
                const std::shared_ptr<TradeResult>& result);

        /// \brief Updates broker-snapshot open trades whose planned close time has passed.
        void process_snapshot_open_trades();

    private:
        AccountInfoProvider&     m_account_info;        ///< Reference to account information provider.
        TradeStateManager&       m_trade_state_manager; ///< Manages trade states and transitions.
        mutable std::mutex       m_trade_result_mutex;  ///< Mutex for the trade result callback.
        trade_result_callback_t  m_trade_result_callback; ///< Callback for handling trade results.
        trade_id_provider_t      m_trade_id_provider;
        std::mutex               m_pending_mutex;      ///< Mutex for pending transactions list.
        std::list<transaction_t> m_pending_transactions; ///< List of pending transactions.
        std::list<transaction_t> m_open_transactions;  ///< List of open transactions.
        std::chrono::steady_clock::time_point m_last_order_time; ///< Last processed order timestamp.
        bool                     m_has_sent_order = false; ///< True after the first broker order request is emitted.
        // TradeQueueManager is owned by the platform event loop. m_pending_mutex
        // protects external enqueueing; open/snapshot counters are event-loop state.
        int64_t                  m_local_open_trades = 0; ///< Number of locally tracked open trades.
        int64_t                  m_snapshot_open_trades = 0; ///< Number of active trades loaded from a broker snapshot.
        int64_t                  m_snapshot_unknown_close_trades = 0; ///< Snapshot trades without a known close time.
        bool                     m_require_open_trades_snapshot = false; ///< True when connected admission requires an authoritative snapshot.
        bool                     m_open_trades_snapshot_ready = true; ///< True only after an authoritative snapshot is accepted.
        bool                     m_snapshot_refresh_requested = false; ///< True after emitting a refresh request for current snapshot state.
        std::vector<int64_t>     m_snapshot_close_due_times_ms; ///< Close timestamps with the configured safety buffer.
        bool                     m_has_trade_storm_balance = false; ///< True when a local trade burst baseline is active.
        double                   m_trade_storm_base_balance = 0.0; ///< Trusted balance captured before a local trade burst.
        double                   m_trade_storm_realized_profit = 0.0; ///< Realized profit within the active local trade burst.
        int64_t                  m_last_trade_activity_ms = 0; ///< Last local trade open/finalize timestamp.
        static constexpr int64_t kTradeStormIdleMs = time_shield::MS_PER_15_SEC; ///< Quiet period before trusting a fresh balance.

        /// \brief Dispatches a trade event notification.
        /// \param transaction The trade transaction event to be dispatched.
        void dispatch_trade_event(const transaction_t& transaction);

        /// \brief Returns the effective open trade count visible to account info providers.
        /// \return Sum of local and broker-snapshot open trades.
        int64_t current_open_trades() const;

        /// \brief Emits the effective open trade count.
        void emit_open_trades(
                const std::shared_ptr<TradeRequest>& request,
                const std::shared_ptr<TradeResult>& result);

        /// \brief Clears broker-snapshot open trade state.
        /// \return True if the effective counter changed.
        bool clear_snapshot_open_trades();

        /// \brief Requests a broker snapshot refresh once for the current snapshot state.
        /// \param reason Human-readable refresh reason.
        void request_snapshot_refresh(const char* reason);

        /// \brief Returns the projected equity-like balance before opening the next local trade.
        double next_open_balance(
                int64_t timestamp_ms,
                double account_balance);

        /// \brief Adds realized result profit to the current local trade burst.
        void add_realized_profit_to_storm(const std::shared_ptr<TradeResult>& result);

        /// \brief Returns true if a trade state has a monetary result.
        static bool is_result_state_for_balance(TradeState state) noexcept;

        /// \brief Adds a close buffer to a close timestamp without overflowing.
        /// \param close_time_ms Close timestamp in milliseconds.
        /// \param close_buffer_ms Non-negative close buffer in milliseconds.
        /// \return Saturated close timestamp with the buffer applied.
        int64_t add_snapshot_close_buffer(
                int64_t close_time_ms,
                int64_t close_buffer_ms) const;

        /// \brief Handles incoming price updates.
        /// \param event The received price update event.
        void handle_event(const events::PriceUpdateEvent& event);

        /// \brief Handles incoming disconnect requests.
        /// \param event The received disconnect request event.
        void handle_event(const events::DisconnectRequestEvent& event);

        /// \brief Handles a broker-side active-trades snapshot.
        /// \param event Snapshot event to apply when the local queue is idle.
        void handle_event(const events::OpenTradesSnapshotEvent& event);

        /// \brief Handles broker snapshot availability changes.
        /// \param event Availability state emitted by the platform snapshot manager.
        void handle_event(const events::OpenTradesSnapshotAvailabilityEvent& event);
    };

    template<typename PreprocessFunction>
    inline bool TradeQueueManager::add_trade(
            std::unique_ptr<TradeRequest> request,
            PlatformType platform_type,
            PreprocessFunction preprocess) {
        LOGIT_0TRACE();
        if (!request) return false;
        if (request->account_type == AccountType::UNKNOWN) {
            request->account_type = m_account_info.get_info<AccountType>(AccountInfoType::ACCOUNT_TYPE);
        }
        if (request->currency == CurrencyType::UNKNOWN) {
            request->currency = m_account_info.get_info<CurrencyType>(AccountInfoType::CURRENCY);
        }
        if (request->trade_id == 0 && m_trade_id_provider) {
            request->trade_id = m_trade_id_provider();
        }
        if (request->trade_id == 0) {
            request->trade_id = utils::make_trade_id();
        }
        auto result = request->create_trade_result_unique();
        result->place_date = OPTIONX_TIMESTAMP_MS;
        result->platform_type = platform_type;
        if (!preprocess(request, result)) return false;

        LOGIT_0TRACE();
        auto trade_event = std::make_shared<events::TradeTransactionEvent>(request, result);
        std::lock_guard<std::mutex> lock(m_pending_mutex);
        m_pending_transactions.push_back(std::move(trade_event));
        return true;
    }

    inline void TradeQueueManager::set_trade_result_callback(trade_result_callback_t callback) {
        std::lock_guard<std::mutex> lock(m_trade_result_mutex);
        m_trade_result_callback = std::move(callback);
    }

    inline TradeQueueManager::transaction_t TradeQueueManager::pop_next_transaction() {
        // Preserve the normal disconnected-account error path. The snapshot
        // fence applies only while a connected account lacks authoritative
        // broker open-trade state.
        if (m_require_open_trades_snapshot &&
            !m_open_trades_snapshot_ready &&
            m_account_info.get_info<bool>(AccountInfoType::CONNECTION_STATUS)) {
            return nullptr;
        }

        const int64_t order_interval_ms = std::max<int64_t>(
            0,
            m_account_info.get_info<int64_t>(AccountInfoType::ORDER_INTERVAL_MS));
        auto now = std::chrono::steady_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_order_time);

        if (!m_has_sent_order || elapsed_time.count() >= order_interval_ms) {
            auto it = m_pending_transactions.begin();
            auto transaction = *it;
            auto& request = transaction->request;
            int64_t open_trades = m_account_info.get_for_trade<int64_t>(AccountInfoType::OPEN_TRADES, request);
            if (open_trades < m_account_info.get_for_trade<int64_t>(AccountInfoType::MAX_TRADES, request)) {
                m_pending_transactions.erase(it);
                return transaction;
            }
        }
        return nullptr;
    }

    inline void TradeQueueManager::handle_canceled_transactions(std::list<transaction_t>& calceled_transactions) {
        const int64_t timestamp = OPTIONX_TIMESTAMP_MS;
        for (const auto &transaction : calceled_transactions) {
            transaction->cancel_async_operations();
            m_trade_state_manager.finalize_transaction_with_error(
                transaction,
                TradeErrorCode::LONG_QUEUE_WAIT,
                TradeState::OPEN_ERROR,
                timestamp);
        }
    }

    inline void TradeQueueManager::clean_expired_transactions(
            int64_t current_time_ms,
            std::list<transaction_t>& calceled_transactions) {
        const int64_t timeout_ms = time_shield::sec_to_ms(m_account_info.get_info<int64_t>(AccountInfoType::ORDER_QUEUE_TIMEOUT));
        auto it = m_pending_transactions.begin();
        while (it != m_pending_transactions.end()) {
            auto& transaction = *it;
            const int64_t delay_ms = current_time_ms - transaction->result->place_date;
            if (delay_ms >= timeout_ms) {
                calceled_transactions.push_back(std::move(transaction));
                it = m_pending_transactions.erase(it);
            } else {
                ++it;
            }
        }
    }

    inline void TradeQueueManager::handle_closing_error(
            const transaction_t& transaction,
            int64_t timestamp) {
        transaction->cancel_async_operations();
        auto& result = transaction->result;
        auto& request = transaction->request;
        // A delayed broker result is recoverable during normal operation. Do
        // not classify it as fatal: fatal logging makes the console runner
        // show an intrusive error prompt for a trade-level timeout.
        LOGIT_ERROR(
            "Trade closing failed. option_id=",
            result ? result->option_id : 0,
            ", error=",
            result ? result->error_desc : std::string());
        decrement_open_trades(request, result);
        m_trade_state_manager.finalize_transaction_with_error(
            transaction,
            result->error_code,
            TradeState::CHECK_ERROR,
            timestamp,
            result->error_desc);
        dispatch_trade_event(transaction);
    }

    inline void TradeQueueManager::process_pending_transactions() {
        std::unique_lock<std::mutex> lock(m_pending_mutex);
        if (m_pending_transactions.empty()) return;

        std::list<transaction_t> calceled_transactions;
        const int64_t timestamp = OPTIONX_TIMESTAMP_MS;

        clean_expired_transactions(timestamp, calceled_transactions);

        for (;;) {
            if (m_pending_transactions.empty()) break;
            std::shared_ptr<events::TradeTransactionEvent> transaction = pop_next_transaction();
            if (!transaction) break;
            lock.unlock();

            auto &request = transaction->request;
            auto &result  = transaction->result;
            result->error_code = m_trade_state_manager.validate_request(request);
            if (result->error_code == TradeErrorCode::SUCCESS) {
                LOGIT_0TRACE();
                result->trade_state = result->live_state = TradeState::WAITING_OPEN;
                result->send_date   = OPTIONX_TIMESTAMP_MS;
                const double account_balance =
                    m_account_info.get_for_trade<double>(AccountInfoType::BALANCE, request);
                result->set_balance(account_balance);
                result->set_open_balance(next_open_balance(result->send_date, account_balance));
                result->payout      = m_account_info.get_for_trade<double>(AccountInfoType::PAYOUT, request, time_shield::ms_to_sec(result->send_date));

                m_last_order_time = std::chrono::steady_clock::now();
                m_has_sent_order = true;
                increment_open_trades(request, result);
                dispatch_trade_event(transaction);

                events::TradeRequestEvent trade_request_event(request, result);
                notify(trade_request_event);

                m_open_transactions.push_back(std::move(transaction));
            } else {
                LOGIT_0TRACE();
                m_trade_state_manager.finalize_transaction_with_error(transaction, result->error_code, TradeState::OPEN_ERROR, timestamp);
                dispatch_trade_event(transaction);
            }

            handle_canceled_transactions(calceled_transactions);
            return;
        }
        lock.unlock();
        handle_canceled_transactions(calceled_transactions);
    }

    inline void TradeQueueManager::process_closing_transactions() {
        if (m_open_transactions.empty()) return;
        const int64_t timestamp = OPTIONX_TIMESTAMP_MS;
        auto it = m_open_transactions.begin();
        while (it != m_open_transactions.end()) {
            auto& transaction = *it;
            auto& request = transaction->request;
            auto& result  = transaction->result;

            if (result->trade_state == TradeState::OPEN_SUCCESS) {
                LOGIT_0TRACE();
                dispatch_trade_event(transaction);
                result->trade_state = result->live_state = TradeState::IN_PROGRESS;
                ++it;
                continue;
            }

            // Skip transactions not in states for closure or waiting
            if (!m_trade_state_manager.is_closable_state(result->trade_state)) {
                ++it;
                continue;
            }

            // Calculate close date based on the option type
            const int64_t close_date = m_trade_state_manager.calculate_close_date(result, request);

            // Handle invalid close date
            if (close_date == 0) {
                LOGIT_0TRACE();
                result->error_code = request->option_type == OptionType::SPRINT ?
                    TradeErrorCode::INVALID_DURATION : TradeErrorCode::INVALID_EXPIRY_TIME;
                handle_closing_error(transaction, timestamp);
                it = m_open_transactions.erase(it);
                continue;
            }

            // If it's not time to close the option yet, skip
            if (timestamp < close_date) {
                ++it;
                continue;
            }

            // If the response timeout has been exceeded, finalize with an error
            if (timestamp > (close_date + m_account_info.get_response_timeout())) {
                LOGIT_0TRACE();
                result->error_code = TradeErrorCode::LONG_RESPONSE_WAIT;
                handle_closing_error(transaction, timestamp);
                it = m_open_transactions.erase(it);
                continue;
            }

            // Transition the state to WAITING_CLOSE and notify listeners
            if (m_trade_state_manager.is_transition_to_waiting_close(result->trade_state)) {
                LOGIT_0TRACE();
                result->trade_state = result->live_state = TradeState::WAITING_CLOSE;
                dispatch_trade_event(transaction);
                events::TradeStatusEvent trade_status_event(
                    request,
                    result,
                    transaction->async_lifecycle_token());
                notify(trade_status_event);
            }

            ++it;
        }
    }

    inline void TradeQueueManager::process_finalizing_transactions() {
        auto it = m_open_transactions.begin();
        while (it != m_open_transactions.end()) {
            auto& transaction = *it;
            auto& request = transaction->request;
            auto& result  = transaction->result;

            // Process transactions in terminal states
            if (m_trade_state_manager.is_terminal_state(result->trade_state)) {
                LOGIT_0TRACE();
                transaction->cancel_async_operations();
                decrement_open_trades(request, result);
                dispatch_trade_event(transaction);
                it = m_open_transactions.erase(it);
            } else {
                ++it;
            }
        }
    }

    inline void TradeQueueManager::finalize_all_trades() {
        LOGIT_0TRACE();

        std::list<transaction_t> pending_transactions;
        std::unique_lock<std::mutex> lock(m_pending_mutex);
        if (!m_pending_transactions.empty()) {
            pending_transactions.swap(m_pending_transactions);
        }
        lock.unlock();

        int64_t timestamp = OPTIONX_TIMESTAMP_MS;
        for (auto& transaction : pending_transactions) {
            transaction->cancel_async_operations();
            m_trade_state_manager.finalize_transaction_with_error(transaction, TradeErrorCode::CLIENT_FORCED_CLOSE, TradeState::OPEN_ERROR, timestamp);
            dispatch_trade_event(transaction);
        }

        for (auto& transaction : m_open_transactions) {
            transaction->cancel_async_operations();
            m_trade_state_manager.finalize_transaction_with_error(transaction, TradeErrorCode::CLIENT_FORCED_CLOSE, TradeState::CHECK_ERROR, timestamp);
            decrement_open_trades(transaction->request, transaction->result);
            dispatch_trade_event(transaction);
        }
        m_open_transactions.clear();

        if (clear_snapshot_open_trades()) {
            emit_open_trades(nullptr, nullptr);
        }

        m_has_sent_order = false;
        m_has_trade_storm_balance = false;
        m_trade_storm_base_balance = 0.0;
        m_trade_storm_realized_profit = 0.0;
        m_last_trade_activity_ms = timestamp;
    }

    inline void TradeQueueManager::increment_open_trades(
        const std::shared_ptr<TradeRequest>& request,
        const std::shared_ptr<TradeResult>& result) {
        m_local_open_trades++;
        m_last_trade_activity_ms = OPTIONX_TIMESTAMP_MS;
        emit_open_trades(request, result);
    }

    inline void TradeQueueManager::decrement_open_trades(
        const std::shared_ptr<TradeRequest>& request,
        const std::shared_ptr<TradeResult>& result) {
        if (m_local_open_trades > 0) {
            add_realized_profit_to_storm(result);
            m_local_open_trades--;
            m_last_trade_activity_ms = OPTIONX_TIMESTAMP_MS;
            emit_open_trades(request, result);
        }
    }

    inline void TradeQueueManager::process_snapshot_open_trades() {
        if (m_snapshot_open_trades <= 0) return;
        if (m_snapshot_close_due_times_ms.empty()) {
            if (m_snapshot_unknown_close_trades > 0) {
                request_snapshot_refresh("missing-close-times");
            }
            return;
        }

        const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
        const auto first_active = std::upper_bound(
            m_snapshot_close_due_times_ms.begin(),
            m_snapshot_close_due_times_ms.end(),
            now_ms);
        const int64_t closed_count = static_cast<int64_t>(
            std::distance(m_snapshot_close_due_times_ms.begin(), first_active));

        if (closed_count <= 0) return;

        m_snapshot_close_due_times_ms.erase(
            m_snapshot_close_due_times_ms.begin(),
            first_active);

        if (closed_count >= m_snapshot_open_trades) {
            m_snapshot_open_trades = 0;
        } else {
            m_snapshot_open_trades -= closed_count;
        }
        if (m_snapshot_unknown_close_trades > m_snapshot_open_trades) {
            m_snapshot_unknown_close_trades = m_snapshot_open_trades;
        }
        emit_open_trades(nullptr, nullptr);

        if (m_snapshot_open_trades > 0 &&
            m_snapshot_unknown_close_trades > 0 &&
            m_snapshot_close_due_times_ms.empty()) {
            request_snapshot_refresh("unknown-close-times");
        }
    }

    inline int64_t TradeQueueManager::current_open_trades() const {
        return m_local_open_trades + m_snapshot_open_trades;
    }

    inline void TradeQueueManager::emit_open_trades(
            const std::shared_ptr<TradeRequest>& request,
            const std::shared_ptr<TradeResult>& result) {
        events::OpenTradesEvent event(current_open_trades(), request, result);
        notify(event);
    }

    inline bool TradeQueueManager::clear_snapshot_open_trades() {
        const bool changed =
            m_snapshot_open_trades != 0 ||
            m_snapshot_unknown_close_trades != 0 ||
            !m_snapshot_close_due_times_ms.empty();
        m_snapshot_open_trades = 0;
        m_snapshot_unknown_close_trades = 0;
        m_snapshot_refresh_requested = false;
        m_snapshot_close_due_times_ms.clear();
        return changed;
    }

    inline void TradeQueueManager::request_snapshot_refresh(const char* reason) {
        if (m_snapshot_refresh_requested) return;
        m_snapshot_refresh_requested = true;
        notify(events::OpenTradesSnapshotRefreshRequestEvent(reason ? reason : ""));
    }

    inline double TradeQueueManager::next_open_balance(
            int64_t timestamp_ms,
            double account_balance) {
        const bool local_queue_idle =
            m_local_open_trades <= 0 && m_open_transactions.empty();
        const bool idle_long_enough =
            m_last_trade_activity_ms <= 0 ||
            timestamp_ms - m_last_trade_activity_ms >= kTradeStormIdleMs;

        if (!m_has_trade_storm_balance ||
            (local_queue_idle && idle_long_enough)) {
            m_has_trade_storm_balance = true;
            m_trade_storm_base_balance = account_balance;
            m_trade_storm_realized_profit = 0.0;
        }

        return m_trade_storm_base_balance + m_trade_storm_realized_profit;
    }

    inline void TradeQueueManager::add_realized_profit_to_storm(
            const std::shared_ptr<TradeResult>& result) {
        if (!result || !is_result_state_for_balance(result->trade_state)) {
            return;
        }
        m_trade_storm_realized_profit += result->profit;
    }

    inline bool TradeQueueManager::is_result_state_for_balance(TradeState state) noexcept {
        return state == TradeState::WIN ||
               state == TradeState::LOSS ||
               state == TradeState::STANDOFF ||
               state == TradeState::REFUND;
    }

    inline int64_t TradeQueueManager::add_snapshot_close_buffer(
            int64_t close_time_ms,
            int64_t close_buffer_ms) const {
        if (close_buffer_ms <= 0) return close_time_ms;
        const int64_t max_ms = std::numeric_limits<int64_t>::max();
        if (close_time_ms > max_ms - close_buffer_ms) return max_ms;
        return close_time_ms + close_buffer_ms;
    }

    inline void TradeQueueManager::dispatch_trade_event(const transaction_t& transaction) {
        auto &request = transaction->request;
        auto &result  = transaction->result;

        notify(transaction.get());
        request->dispatch_callbacks(request, result);

        std::lock_guard<std::mutex> lock(m_trade_result_mutex);
        if (m_trade_result_callback) {
            m_trade_result_callback(
                std::move(request->clone_unique()),
                std::move(result->clone_unique()));
        }
    }

    inline void TradeQueueManager::handle_event(const events::PriceUpdateEvent& event) {
        for (auto& transaction : m_open_transactions) {
            auto& request = transaction->request;
            auto& result = transaction->result;

            if (result->trade_state != TradeState::OPEN_SUCCESS &&
                result->trade_state != TradeState::IN_PROGRESS) continue;

            if (result->trade_state == TradeState::OPEN_SUCCESS) {
                LOGIT_0TRACE();
                dispatch_trade_event(transaction);
                result->live_state  = TradeState::IN_PROGRESS;
                result->trade_state = TradeState::IN_PROGRESS;
            }

            const auto* quote_batch = event.find_tick_batch_by_symbol(request->symbol);
            if (!quote_batch || quote_batch->items.empty()) continue;

            const auto& quote = quote_batch->items.back();
            if (!quote.has_flag(MarketDataFlags::INITIALIZED)) continue;

            const double close_price = quote.mid_price(quote_batch->price_digits);
            result->close_price = close_price;
            result->live_state = m_trade_state_manager.determine_trade_state(result, request, close_price);
            dispatch_trade_event(transaction);
        }
    }

    inline void TradeQueueManager::handle_event(const events::DisconnectRequestEvent& event) {
        if (m_require_open_trades_snapshot) {
            m_open_trades_snapshot_ready = false;
        }
        finalize_all_trades();
    }

    inline void TradeQueueManager::handle_event(const events::OpenTradesSnapshotEvent& event) {
        std::unique_lock<std::mutex> lock(m_pending_mutex);
        m_snapshot_refresh_requested = false;
        const bool pending_blocks_snapshot =
            m_open_trades_snapshot_ready && !m_pending_transactions.empty();
        if (pending_blocks_snapshot ||
            !m_open_transactions.empty() ||
            m_local_open_trades > 0) {
            LOGIT_DEBUG(
                "Open trades snapshot skipped because local trade queue is active. snapshot_open_trades=",
                event.open_trades,
                ", local_open_trades=",
                m_local_open_trades);
            lock.unlock();
            request_snapshot_refresh("local-queue-active");
            return;
        }

        m_open_trades_snapshot_ready = true;
        m_snapshot_open_trades = std::max<int64_t>(0, event.open_trades);
        m_snapshot_unknown_close_trades = 0;
        m_snapshot_close_due_times_ms.clear();
        const int64_t close_buffer_ms = std::max<int64_t>(0, event.close_buffer_ms);
        if (m_snapshot_open_trades > 0) {
            for (const int64_t close_time_ms : event.close_times_ms) {
                if (close_time_ms <= 0) continue;
                m_snapshot_close_due_times_ms.push_back(
                    add_snapshot_close_buffer(close_time_ms, close_buffer_ms));
            }
        }
        std::sort(
            m_snapshot_close_due_times_ms.begin(),
            m_snapshot_close_due_times_ms.end());
        if (static_cast<int64_t>(m_snapshot_close_due_times_ms.size()) >
            m_snapshot_open_trades) {
            m_snapshot_close_due_times_ms.resize(
                static_cast<std::size_t>(m_snapshot_open_trades));
        }
        m_snapshot_unknown_close_trades = std::max<int64_t>(
            0,
            m_snapshot_open_trades -
                static_cast<int64_t>(m_snapshot_close_due_times_ms.size()));
        lock.unlock();

        LOGIT_INFO(
            "Open trades snapshot applied. snapshot_open_trades=",
            m_snapshot_open_trades,
            ", unknown_close_times=",
            m_snapshot_unknown_close_trades,
            ", close_times=",
            m_snapshot_close_due_times_ms.size());
        emit_open_trades(nullptr, nullptr);

        if (m_snapshot_unknown_close_trades > 0) {
            request_snapshot_refresh("unknown-close-times");
        }
    }

    inline void TradeQueueManager::handle_event(
            const events::OpenTradesSnapshotAvailabilityEvent& event) {
        if (!m_require_open_trades_snapshot) return;

        m_open_trades_snapshot_ready = event.ready;
        if (event.ready) return;

        // Availability=false means that the current snapshot is unknown, not
        // that the broker confirmed zero active trades. Keep the authoritative
        // public count unchanged until a replacement snapshot is accepted.
        clear_snapshot_open_trades();
    }

} // namespace optionx::components

#endif // OPTIONX_HEADER_COMPONENTS_BASE_TRADE_EXECUTION_COMPONENT_TRADE_QUEUE_MANAGER_HPP_INCLUDED
