#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_SUBSCRIPTION_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_SUBSCRIPTION_HPP_INCLUDED

/// \file MarketDataSubscription.hpp
/// \brief Defines market-data subscription request, handle, and result DTOs.

#include "MarketDataContinuityOptions.hpp"

namespace optionx::market_data {

    /// \brief Runtime identifier of a market-data provider instance.
    /// \details The value is process-local and is used only to reject handles
    ///          passed to the wrong provider object.
    using ProviderInstanceId = std::uint64_t;

    /// \brief Runtime identifier assigned by a market-data provider.
    /// \details A subscription ID is meaningful only together with its
    ///          ProviderInstanceId.
    using SubscriptionId = std::uint64_t;

    /// \brief Invalid market-data provider instance identifier.
    inline constexpr ProviderInstanceId kInvalidProviderInstanceId = 0;

    /// \brief Invalid market-data subscription identifier.
    inline constexpr SubscriptionId kInvalidSubscriptionId = 0;

    /// \struct TickSubscriptionRequest
    /// \brief Request for a live tick market-data stream.
    struct TickSubscriptionRequest {
        std::string symbol; ///< Broker/provider symbol.
        MarketDataTransport transport = MarketDataTransport::AUTO; ///< Preferred transport.

        /// \brief Default constructor.
        TickSubscriptionRequest() = default;

        /// \brief Constructs a tick-stream request.
        /// \param symbol Broker/provider symbol.
        /// \param transport Preferred transport for live data.
        TickSubscriptionRequest(
                std::string symbol,
                MarketDataTransport transport = MarketDataTransport::AUTO)
                : symbol(std::move(symbol)),
                  transport(transport) {}

        /// \brief Returns true when the request describes a valid live tick stream.
        /// \return True when symbol is not empty.
        bool valid() const {
            return !symbol.empty();
        }
    };

    /// \struct BarSubscriptionRequest
    /// \brief Request for a live bar market-data stream.
    struct BarSubscriptionRequest {
        std::string symbol; ///< Broker/provider symbol.
        BarTimeframe timeframe = 0; ///< Bar timeframe in seconds; values <= 0 are invalid.
        BarPriceSource price_source = BarPriceSource::MID; ///< Price source for bars.
        MarketDataTransport transport = MarketDataTransport::AUTO; ///< Preferred transport.
        MarketDataContinuityOptions continuity; ///< Optional history prefill and gap recovery.

        /// \brief Default constructor.
        BarSubscriptionRequest() = default;

        /// \brief Constructs a bar-stream request.
        /// \param symbol Broker/provider symbol.
        /// \param timeframe Bar timeframe in seconds.
        /// \param price_source Price stream used to build OHLC values.
        /// \param transport Preferred transport for live data.
        /// \param continuity Optional history prefill and gap recovery policy.
        BarSubscriptionRequest(
                std::string symbol,
                BarTimeframe timeframe,
                BarPriceSource price_source = BarPriceSource::MID,
                MarketDataTransport transport = MarketDataTransport::AUTO,
                MarketDataContinuityOptions continuity = {})
                : symbol(std::move(symbol)),
                  timeframe(timeframe),
                  price_source(price_source),
                  transport(transport),
                  continuity(continuity) {}

        /// \brief Returns true when the request describes a valid live bar stream.
        /// \return True when symbol is not empty and timeframe is positive.
        bool valid() const {
            return !symbol.empty() && timeframe > 0 && continuity.valid();
        }
    };

    /// \struct MarketDataSubscriptionHandle
    /// \brief Opaque handle returned for an accepted live market-data subscription.
    struct MarketDataSubscriptionHandle {
        ProviderInstanceId provider_id = kInvalidProviderInstanceId; ///< Provider instance that owns the handle.
        SubscriptionId id = kInvalidSubscriptionId; ///< Provider-assigned subscription ID.
        std::string symbol; ///< Subscribed symbol.
        MarketDataType stream_type = MarketDataType::UNKNOWN; ///< Stream type.
        BarTimeframe timeframe = 0; ///< Bar timeframe in seconds, or 0 for ticks.
        BarPriceSource price_source = BarPriceSource::MID; ///< Price source for bar streams.
        MarketDataTransport transport = MarketDataTransport::AUTO; ///< Transport chosen or requested.

        /// \brief Returns true if the handle can identify a provider subscription.
        /// \return True when both provider and subscription IDs are non-zero.
        bool valid() const {
            return provider_id != kInvalidProviderInstanceId &&
                   id != kInvalidSubscriptionId;
        }

        /// \brief Builds a tick handle from a request and provider-assigned IDs.
        /// \param provider_id Runtime provider instance that owns the handle.
        /// \param id Provider-assigned subscription ID.
        /// \param request Original tick subscription request.
        static MarketDataSubscriptionHandle from_tick_request(
                ProviderInstanceId provider_id,
                SubscriptionId id,
                const TickSubscriptionRequest& request) {
            MarketDataSubscriptionHandle handle;
            handle.provider_id = provider_id;
            handle.id = id;
            handle.symbol = request.symbol;
            handle.stream_type = MarketDataType::TICKS;
            handle.timeframe = 0;
            handle.transport = request.transport;
            return handle;
        }

        /// \brief Builds a bar handle from a request and provider-assigned IDs.
        /// \param provider_id Runtime provider instance that owns the handle.
        /// \param id Provider-assigned subscription ID.
        /// \param request Original bar subscription request.
        static MarketDataSubscriptionHandle from_bar_request(
                ProviderInstanceId provider_id,
                SubscriptionId id,
                const BarSubscriptionRequest& request) {
            MarketDataSubscriptionHandle handle;
            handle.provider_id = provider_id;
            handle.id = id;
            handle.symbol = request.symbol;
            handle.stream_type = MarketDataType::BARS;
            handle.timeframe = request.timeframe;
            handle.price_source = request.price_source;
            handle.transport = request.transport;
            return handle;
        }
    };

    /// \struct MarketDataSubscriptionResult
    /// \brief Typed result for market-data subscribe and unsubscribe operations.
    struct MarketDataSubscriptionResult {
        MarketDataSubscriptionStatus status = MarketDataSubscriptionStatus::UNKNOWN; ///< Operation status.
        std::string error_message; ///< Failure details, if any.
        MarketDataSubscriptionHandle subscription; ///< Related subscription handle.

        /// \brief Returns true when the operation succeeded.
        /// \details The status field is the source of truth; there is no
        ///          separate mutable success flag.
        bool success() const noexcept {
            return status == MarketDataSubscriptionStatus::APPLIED ||
                   status == MarketDataSubscriptionStatus::SUBSCRIBED ||
                   status == MarketDataSubscriptionStatus::UNSUBSCRIBED;
        }

        /// \brief Allows result objects to be used in boolean contexts.
        explicit operator bool() const noexcept {
            return success();
        }

        /// \brief Creates a successful desired-subscription acceptance result.
        /// \details Stream transport readiness is reported separately through
        ///          MarketDataStatusUpdate with status READY.
        /// \param handle Accepted subscription handle.
        static MarketDataSubscriptionResult subscribed(MarketDataSubscriptionHandle handle) {
            if (!handle.valid()) {
                return failed(
                    std::move(handle),
                    MarketDataSubscriptionStatus::INVALID_REQUEST,
                    "Cannot subscribe with an invalid market-data subscription handle.");
            }

            MarketDataSubscriptionResult result;
            result.status = MarketDataSubscriptionStatus::SUBSCRIBED;
            result.subscription = std::move(handle);
            return result;
        }

        /// \brief Creates a successful unsubscribe result.
        /// \param handle Stopped subscription handle.
        static MarketDataSubscriptionResult unsubscribed(MarketDataSubscriptionHandle handle) {
            if (!handle.valid()) {
                return failed(
                    std::move(handle),
                    MarketDataSubscriptionStatus::INVALID_REQUEST,
                    "Cannot unsubscribe with an invalid market-data subscription handle.");
            }

            MarketDataSubscriptionResult result;
            result.status = MarketDataSubscriptionStatus::UNSUBSCRIBED;
            result.subscription = std::move(handle);
            return result;
        }

        /// \brief Creates a failure result for a tick request.
        /// \param request Rejected request.
        /// \param status Failure status; accidental success statuses are normalized.
        /// \param message Diagnostic message for the caller.
        static MarketDataSubscriptionResult failed(
                TickSubscriptionRequest request,
                MarketDataSubscriptionStatus status,
                std::string message) {
            MarketDataSubscriptionResult result;
            result.status = failure_status_or_failed(status);
            result.error_message = std::move(message);
            result.subscription = MarketDataSubscriptionHandle::from_tick_request(
                kInvalidProviderInstanceId,
                kInvalidSubscriptionId,
                request);
            return result;
        }

        /// \brief Creates a failure result for a bar request.
        /// \param request Rejected request.
        /// \param status Failure status; accidental success statuses are normalized.
        /// \param message Diagnostic message for the caller.
        static MarketDataSubscriptionResult failed(
                BarSubscriptionRequest request,
                MarketDataSubscriptionStatus status,
                std::string message) {
            MarketDataSubscriptionResult result;
            result.status = failure_status_or_failed(status);
            result.error_message = std::move(message);
            result.subscription = MarketDataSubscriptionHandle::from_bar_request(
                kInvalidProviderInstanceId,
                kInvalidSubscriptionId,
                request);
            return result;
        }

        /// \brief Creates a failure result for an existing handle.
        /// \param handle Subscription handle related to the failure.
        /// \param status Failure status; accidental success statuses are normalized.
        /// \param message Diagnostic message for the caller.
        static MarketDataSubscriptionResult failed(
                MarketDataSubscriptionHandle handle,
                MarketDataSubscriptionStatus status,
                std::string message) {
            MarketDataSubscriptionResult result;
            result.status = failure_status_or_failed(status);
            result.error_message = std::move(message);
            result.subscription = std::move(handle);
            return result;
        }

        /// \brief Converts accidental success statuses to a failure status.
        /// \return FAILED when status is a success value; otherwise returns status.
        static MarketDataSubscriptionStatus failure_status_or_failed(
                MarketDataSubscriptionStatus status) noexcept {
            if (status == MarketDataSubscriptionStatus::APPLIED ||
                status == MarketDataSubscriptionStatus::SUBSCRIBED ||
                status == MarketDataSubscriptionStatus::UNSUBSCRIBED) {
                return MarketDataSubscriptionStatus::FAILED;
            }
            return status;
        }
    };

    /// \enum MarketDataSubscriptionAction
    /// \brief Operation stored inside a subscription batch.
    enum class MarketDataSubscriptionAction {
        SUBSCRIBE_TICKS, ///< Start a live tick stream.
        SUBSCRIBE_BARS,  ///< Start a live bar stream.
        UNSUBSCRIBE,     ///< Stop one live stream.
        UNSUBSCRIBE_ALL  ///< Stop all streams owned by the provider.
    };

    /// \struct MarketDataSubscriptionChange
    /// \brief One operation inside a market-data subscription batch.
    struct MarketDataSubscriptionChange {
        MarketDataSubscriptionAction action = MarketDataSubscriptionAction::SUBSCRIBE_TICKS; ///< Operation kind.
        TickSubscriptionRequest tick_request; ///< Tick request for SUBSCRIBE_TICKS.
        BarSubscriptionRequest bar_request; ///< Bar request for SUBSCRIBE_BARS.
        MarketDataSubscriptionHandle subscription; ///< Handle for unsubscribe operations.
    };

    /// \struct MarketDataSubscriptionBatch
    /// \brief A set of live market-data subscription changes to apply atomically.
    struct MarketDataSubscriptionBatch {
        std::vector<MarketDataSubscriptionChange> changes; ///< Ordered subscription changes.

        /// \brief Adds a live tick subscription request.
        MarketDataSubscriptionBatch& subscribe_ticks(TickSubscriptionRequest request) {
            MarketDataSubscriptionChange change;
            change.action = MarketDataSubscriptionAction::SUBSCRIBE_TICKS;
            change.tick_request = std::move(request);
            changes.push_back(std::move(change));
            return *this;
        }

        /// \brief Adds a live bar subscription request.
        MarketDataSubscriptionBatch& subscribe_bars(BarSubscriptionRequest request) {
            MarketDataSubscriptionChange change;
            change.action = MarketDataSubscriptionAction::SUBSCRIBE_BARS;
            change.bar_request = std::move(request);
            changes.push_back(std::move(change));
            return *this;
        }

        /// \brief Adds one unsubscribe operation.
        MarketDataSubscriptionBatch& unsubscribe(MarketDataSubscriptionHandle subscription) {
            MarketDataSubscriptionChange change;
            change.action = MarketDataSubscriptionAction::UNSUBSCRIBE;
            change.subscription = std::move(subscription);
            changes.push_back(std::move(change));
            return *this;
        }

        /// \brief Adds an operation that stops all active subscriptions.
        MarketDataSubscriptionBatch& unsubscribe_all() {
            MarketDataSubscriptionChange change;
            change.action = MarketDataSubscriptionAction::UNSUBSCRIBE_ALL;
            changes.push_back(std::move(change));
            return *this;
        }

        /// \brief Returns true when there are no changes.
        [[nodiscard]] bool empty() const noexcept {
            return changes.empty();
        }
    };

    /// \struct MarketDataSubscriptionBatchResult
    /// \brief Result of applying a market-data subscription batch.
    struct MarketDataSubscriptionBatchResult {
        MarketDataSubscriptionStatus status = MarketDataSubscriptionStatus::UNKNOWN; ///< Batch status.
        std::string error_message; ///< Batch-level failure details.
        std::vector<MarketDataSubscriptionResult> results; ///< Per-operation results.

        /// \brief Returns true when the batch was applied successfully.
        [[nodiscard]] bool success() const noexcept {
            return status == MarketDataSubscriptionStatus::APPLIED;
        }

        /// \brief Allows batch results to be used in boolean contexts.
        explicit operator bool() const noexcept {
            return success();
        }

        /// \brief Creates a successful batch result.
        static MarketDataSubscriptionBatchResult applied(
                std::vector<MarketDataSubscriptionResult> results) {
            MarketDataSubscriptionBatchResult result;
            result.status = MarketDataSubscriptionStatus::APPLIED;
            result.results = std::move(results);
            return result;
        }

        /// \brief Creates a failed batch result.
        static MarketDataSubscriptionBatchResult failed(
                MarketDataSubscriptionStatus status,
                std::string message,
                std::vector<MarketDataSubscriptionResult> results = {}) {
            MarketDataSubscriptionBatchResult result;
            result.status = MarketDataSubscriptionResult::failure_status_or_failed(status);
            result.error_message = std::move(message);
            result.results = std::move(results);
            return result;
        }
    };

    /// \struct MarketDataStatusUpdate
    /// \brief Status update for a live market-data stream.
    struct MarketDataStatusUpdate {
        ProviderInstanceId provider_id = kInvalidProviderInstanceId; ///< Provider that owns the stream.
        MarketDataSubscriptionHandle subscription; ///< Related subscription, if the provider can identify one.
        MarketDataType type = MarketDataType::UNKNOWN; ///< Stream payload type.
        std::string symbol; ///< Stream symbol.
        BarTimeframe timeframe = 0; ///< Bar timeframe in seconds, or 0 for ticks.
        MarketDataTransport transport = MarketDataTransport::AUTO; ///< Stream transport.
        MarketDataStreamStatus status = MarketDataStreamStatus::UNKNOWN; ///< Stream status.
        std::string message; ///< Optional status details.
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_SUBSCRIPTION_HPP_INCLUDED
