#pragma once
#ifndef OPTIONX_HEADER_COMPONENTS_BASE_TRADE_EXECUTION_COMPONENT_HPP_INCLUDED
#define OPTIONX_HEADER_COMPONENTS_BASE_TRADE_EXECUTION_COMPONENT_HPP_INCLUDED

/// \file BaseTradeExecutionComponent.hpp
/// \brief Defines the BaseTradeExecutionComponent class for managing trade requests and processing transactions.

#include "BaseTradeExecutionComponent/AccountInfoProvider.hpp"
#include "BaseTradeExecutionComponent/TradeStateManager.hpp"
#include "BaseTradeExecutionComponent/TradeQueueManager.hpp"

namespace optionx::components {

    /// \class BaseTradeExecutionComponent
    /// \brief Central component responsible for managing trade requests and processing transactions.
    ///
    /// This class acts as a high-level manager that handles trade requests, validates them, and processes pending orders.
    /// It delegates state management to `TradeStateManager` and queue management to `TradeQueueManager`.
    ///
    /// ### Subscribed Events:
    /// - `PriceUpdateEvent` - Updates trade states based on market prices.
    /// - `DisconnectRequestEvent` - Handles connection loss and forces trade finalization.
    /// - `OpenTradesSnapshotEvent` - Synchronizes broker-side active trades when the local queue is idle.
    /// - `OpenTradesSnapshotAvailabilityEvent` - Gates connected-account trade admission until broker state is authoritative.
    ///
    /// ### Emitted Events:
    /// - `TradeTransactionEvent` - Notifies about trade request updates.
    /// - `TradeRequestEvent` - Sent when a trade request is accepted for processing.
    /// - `TradeStatusEvent` - Updates listeners on trade state changes.
    /// - `OpenTradesEvent` - Notifies about changes in the number of open trades.
    /// - `OpenTradesSnapshotRefreshRequestEvent` - Requests a broker active-trades snapshot refresh.
    class BaseTradeExecutionComponent : public BaseComponent {
    public:
        using trade_result_callback_t = std::function<void(std::unique_ptr<TradeRequest>, std::unique_ptr<TradeResult>)>;
        using trade_id_provider_t = std::function<std::uint32_t()>;

        /// \brief Constructs a `BaseTradeExecutionComponent` instance.
        /// \param bus Reference to the `EventBus` for subscribing to and emitting events.
        /// \param account_info Shared pointer to the `BaseAccountInfoData` interface for retrieving account-related information.
        /// \param require_open_trades_snapshot Start with trade admission blocked
        ///        until an authoritative broker snapshot is accepted.
        explicit BaseTradeExecutionComponent(
                utils::EventBus& bus,
                std::shared_ptr<BaseAccountInfoData> account_info,
                bool require_open_trades_snapshot = false)
            : BaseComponent(bus), m_account_info(std::move(account_info)),
              m_trade_state_manager(m_account_info),
              m_trade_queue(
                  bus,
                  m_account_info,
                  m_trade_state_manager,
                  require_open_trades_snapshot) {
        }

        /// \brief Default virtual destructor.
        virtual ~BaseTradeExecutionComponent() = default;

        /// \brief Handles an event notification received as a raw pointer.
        /// \param event The received event.
        void on_event(const utils::Event* const event) override {};

        /// \brief Sets a callback for trade result events (open, close, or errors).
        /// \param callback Function to handle trade result events.
        void set_trade_result_callback(trade_result_callback_t callback) {
            m_trade_queue.set_trade_result_callback(callback);
        }

        /// \brief Returns the trade result callback slot.
        /// \return Mutable callback reference invoked for trade state updates.
        trade_result_callback_t& on_trade_result() {
            return m_trade_queue.on_trade_result();
        }

        /// \brief Returns provider used to reserve persistent trade IDs before execution.
        /// \return Mutable provider callback. Leave empty to use the process-local fallback generator.
        trade_id_provider_t& on_trade_id() {
            return m_trade_queue.on_trade_id();
        }

        /// \brief Validates and places a trade request into the pending queue.
        /// \param request Unique pointer to a trade request.
        /// \return True if the request passes validation and is added to the queue; false otherwise.
        bool place_trade(std::unique_ptr<TradeRequest> request) {
            return m_trade_queue.add_trade(
                    std::move(request),
                    platform_type(),
                    [this](
                    std::unique_ptr<TradeRequest> &trade_request,
                    std::unique_ptr<TradeResult> &trade_result) {
                return preprocess_trade_request(trade_request, trade_result);
            });
        }

        /// \brief Initializes the component.
        void initialize() override {};

        /// \brief Processes all pending and active trades.
        void process() override {
            m_trade_queue.process();
        }

        /// \brief Finalizes all active and pending trades before shutdown.
        void shutdown() override {
            m_trade_queue.finalize_all_trades();
        }

    protected:
        AccountInfoProvider m_account_info; ///< Manages access to account-related data.
        TradeStateManager   m_trade_state_manager; ///< Handles trade state transitions and validation.
        TradeQueueManager   m_trade_queue; ///< Manages the queue of pending and active trade transactions.

        /// \brief Preprocesses a trade request before placing it into the queue.
        /// \param trade_request Unique pointer to a trade request.
        /// \param trade_result Unique pointer to a trade result.
        /// \return True if the request is valid after preprocessing; false otherwise.
        virtual bool preprocess_trade_request(
                std::unique_ptr<TradeRequest> &trade_request,
                std::unique_ptr<TradeResult> &trade_result) {
            // Default implementation; can be overridden by derived classes.
            return true;
        }

        /// \brief Returns the platform type associated with this trade manager.
        /// \return The `PlatformType` of the trading component.
        virtual PlatformType platform_type() const = 0;
    };

} // namespace optionx::components

#endif // OPTIONX_HEADER_COMPONENTS_BASE_TRADE_EXECUTION_COMPONENT_HPP_INCLUDED
