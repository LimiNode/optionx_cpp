#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_IMARKET_DATA_SUBSCRIBER_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_IMARKET_DATA_SUBSCRIBER_HPP_INCLUDED

/// \file IMarketDataSubscriber.hpp
/// \brief Defines the object interface used by market-data fan-out helpers.

namespace optionx::market_data {

    /// \class IMarketDataSubscriber
    /// \brief Receives routed market-data batches and stream status updates.
    /// \details This interface is intended for bots, chart services, strategy
    ///          services, and time-series builders that prefer one subscriber
    ///          object over assigning global provider callbacks directly. The
    ///          interface only receives events; subscription creation and
    ///          lifetime are owned by router/hub objects.
    class IMarketDataSubscriber {
    public:
        /// \brief Virtual destructor for subscriber implementations.
        virtual ~IMarketDataSubscriber() = default;

        /// \brief Receives a live tick-data batch.
        /// \param batch Tick batch routed from a provider.
        virtual void on_tick_data(const TickDataBatch& batch) {
            (void)batch;
        }

        /// \brief Receives a live bar-data batch.
        /// \param batch Bar batch routed from a provider.
        virtual void on_bar_data(const BarDataBatch& batch) {
            (void)batch;
        }

        /// \brief Receives a live market-data stream status update.
        /// \param update Stream status routed from a provider.
        virtual void on_market_data_status(const MarketDataStatusUpdate& update) {
            (void)update;
        }

        /// \brief Receives history prefill or gap-recovery progress for a route.
        /// \param update Route-scoped continuity status and range information.
        virtual void on_market_data_continuity(
                const MarketDataContinuityUpdate& update) {
            (void)update;
        }
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_IMARKET_DATA_SUBSCRIBER_HPP_INCLUDED
