#pragma once
#ifndef OPTIONX_HEADER_DATA_EVENTS_TRADE_TRANSACTION_EVENT_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_EVENTS_TRADE_TRANSACTION_EVENT_HPP_INCLUDED

#include <atomic>
#include <memory>

/// \file TradeTransactionEvent.hpp
/// \brief Defines the TradeTransactionEvent class used for handling trade transaction events.

namespace optionx::events {

    using trade_lifecycle_token_t = std::shared_ptr<std::atomic_bool>;

    /// \class TradeTransactionEvent
    /// \brief Represents an event containing information about a trade transaction.
    class TradeTransactionEvent : public utils::Event {
    public:
        std::shared_ptr<TradeRequest> request;  ///< Shared pointer to the trade request details.
        std::shared_ptr<TradeResult>  result;   ///< Shared pointer to the trade result details.

        /// \brief Constructs a TradeTransactionEvent with a trade request and API type.
        /// \param trade_request A unique pointer to the trade request that initializes the event.
        /// \param platform_type
        TradeTransactionEvent(std::unique_ptr<TradeRequest> &trade_request, PlatformType platform_type) {
            if (trade_request && trade_request->trade_id == 0) {
                trade_request->trade_id = utils::make_trade_id();
            }
            result = trade_request->create_trade_result_shared();
            result->place_date = OPTIONX_TIMESTAMP_MS;
            result->platform_type = platform_type;
            request = std::shared_ptr<TradeRequest>(trade_request.release());
        }

        /// \brief Constructs a TradeTransactionEvent with a trade request and trade result.
        /// \param trade_request A unique pointer to the trade request to initialize the event.
        /// \param trade_result A unique pointer to the trade result to initialize the event.
        TradeTransactionEvent(
                std::unique_ptr<TradeRequest> &trade_request,
                std::unique_ptr<TradeResult> &trade_result) {
            request = std::shared_ptr<TradeRequest>(trade_request.release());
            result = std::shared_ptr<TradeResult>(trade_result.release());
        }

        /// \brief Default virtual destructor.
        virtual ~TradeTransactionEvent() = default;
        
        std::type_index type() const override {
            return typeid(TradeTransactionEvent);
        }

        const char* name() const override {
            return "TradeTransactionEvent";
        }

        /// \brief Returns the token shared by asynchronous operations for this transaction.
        /// \return Shared lifecycle token, active until the transaction is finalized.
        const trade_lifecycle_token_t& async_lifecycle_token() const noexcept {
            return m_async_lifecycle_token;
        }

        /// \brief Reports whether asynchronous work may still update this transaction.
        /// \return False after queue finalization invalidates the transaction.
        bool async_operations_active() const noexcept {
            return m_async_lifecycle_token &&
                m_async_lifecycle_token->load(std::memory_order_acquire);
        }

        /// \brief Invalidates asynchronous work associated with this transaction.
        void cancel_async_operations() noexcept {
            if (m_async_lifecycle_token) {
                m_async_lifecycle_token->store(false, std::memory_order_release);
            }
        }

    private:
        trade_lifecycle_token_t m_async_lifecycle_token =
            std::make_shared<std::atomic_bool>(true);
    };

} // namespace optionx::events

#endif // OPTIONX_HEADER_DATA_EVENTS_TRADE_TRANSACTION_EVENT_HPP_INCLUDED
