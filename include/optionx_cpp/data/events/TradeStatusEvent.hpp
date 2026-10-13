#pragma once
#ifndef OPTIONX_HEADER_DATA_EVENTS_TRADE_STATUS_EVENT_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_EVENTS_TRADE_STATUS_EVENT_HPP_INCLUDED

/// \file TradeStatusEvent.hpp
/// \brief Defines the TradeStatusEvent class for handling trade status checks.

namespace optionx::events {

    /// \class TradeStatusEvent
    /// \brief Event to check the status of a trade.
    class TradeStatusEvent : public utils::Event {
    public:
        std::shared_ptr<TradeRequest> request;  ///< Shared pointer to the trade request details.
        std::shared_ptr<TradeResult>  result;    ///< Shared pointer to the trade result details.
        trade_lifecycle_token_t lifecycle_token; ///< Token fencing late asynchronous callbacks.

        /// \brief Constructor initializing the trade request and result.
        /// \param trade_request Shared pointer to the trade request.
        /// \param trade_result Shared pointer to the trade result.
        /// \param token Optional transaction lifecycle token used to fence late callbacks.
        explicit TradeStatusEvent(
            std::shared_ptr<TradeRequest> trade_request,
            std::shared_ptr<TradeResult>  trade_result,
            trade_lifecycle_token_t token = nullptr)
            : request(std::move(trade_request)),
              result(std::move(trade_result)),
              lifecycle_token(std::move(token)) {}

        /// \brief Default virtual destructor.
        virtual ~TradeStatusEvent() = default;
        
        std::type_index type() const override {
            return typeid(TradeStatusEvent);
        }

        const char* name() const override {
            return "TradeStatusEvent";
        }
    };

} // namespace optionx::events

#endif // OPTIONX_HEADER_DATA_EVENTS_TRADE_STATUS_EVENT_HPP_INCLUDED
