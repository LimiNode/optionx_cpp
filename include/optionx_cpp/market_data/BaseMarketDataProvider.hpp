#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_BASE_MARKET_DATA_PROVIDER_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_BASE_MARKET_DATA_PROVIDER_HPP_INCLUDED

/// \file BaseMarketDataProvider.hpp
/// \brief Declares the public market-data provider role.

namespace optionx::market_data {

    /// \class BaseMarketDataProvider
    /// \brief Role interface for endpoints that provide live or historical market data.
    class BaseMarketDataProvider {
    public:
        /// \brief Callback that receives live bar batches from a provider.
        using bars_callback_t = std::function<void(std::unique_ptr<BarDataBatch>)>;

        /// \brief Callback that receives live tick batches from a provider.
        using ticks_callback_t = std::function<void(std::unique_ptr<TickDataBatch>)>;

        /// \brief Callback that receives market-data stream status updates.
        using status_callback_t = std::function<void(MarketDataStatusUpdate)>;

        /// \brief Callback that receives historical bars or a typed failure.
        using bar_history_callback_t = std::function<void(BarHistoryResult)>;

        /// \brief Callback that receives subscribe/unsubscribe acceptance results.
        /// \details A successful subscribe result means the provider accepted
        ///          desired state and returned a handle. Live transport
        ///          readiness is reported through status_callback_t.
        using subscription_callback_t = std::function<void(MarketDataSubscriptionResult)>;

        /// \brief Callback that receives subscription batch acceptance results.
        /// \details Stream-level CONNECTED/READY/DISCONNECTED updates are
        ///          delivered through status_callback_t.
        using subscription_batch_callback_t = std::function<void(MarketDataSubscriptionBatchResult)>;

        /// \brief Constructs a market-data provider and assigns a runtime instance ID.
        BaseMarketDataProvider() : m_provider_id(next_provider_instance_id()) {}

        /// \brief Copying is disabled because subscription handles are bound to provider identity.
        BaseMarketDataProvider(const BaseMarketDataProvider&) = delete;
        /// \brief Copy assignment is disabled because it would duplicate provider identity.
        BaseMarketDataProvider& operator=(const BaseMarketDataProvider&) = delete;
        /// \brief Moving is disabled because existing handles must not change owner identity.
        BaseMarketDataProvider(BaseMarketDataProvider&&) = delete;
        /// \brief Move assignment is disabled because existing handles must not change owner identity.
        BaseMarketDataProvider& operator=(BaseMarketDataProvider&&) = delete;

        /// \brief Virtual destructor for polymorphic provider implementations.
        virtual ~BaseMarketDataProvider() = default;

        /// \brief Returns the runtime provider instance ID used to bind subscription handles.
        ProviderInstanceId provider_id() const noexcept {
            return m_provider_id;
        }

        /// \brief Returns a reference to the live bar-data callback.
        /// \details Providers may coalesce queued updates and invoke this callback
        ///          from their lifecycle `process()`/platform loop, not directly
        ///          from a low-level event-bus drain. Live bar streams may emit
        ///          multiple `INCOMPLETE` snapshots for the same bar key before a
        ///          later `FINALIZED` snapshot arrives.
        /// \return Mutable callback reference, or a null callback if live bars are unsupported.
        virtual bars_callback_t& on_bar_data() {
            return m_bar_data_callback;
        }

        /// \brief Returns a reference to the live tick-data callback.
        /// \details Providers may coalesce queued updates and invoke this callback
        ///          from their lifecycle `process()`/platform loop, not directly
        ///          from a low-level event-bus drain.
        /// \return Mutable callback reference, or a null callback if live ticks are unsupported.
        virtual ticks_callback_t& on_tick_data() {
            return m_tick_data_callback;
        }

        /// \brief Returns a reference to the market-data stream status callback.
        /// \details Status updates describe stream-level lifecycle events keyed by
        ///          symbol/type/timeframe/transport. They are not per-subscription
        ///          callbacks and providers are not required to replay cached
        ///          READY status to late subscribers.
        /// \return Mutable callback reference, or a null callback if status updates are unsupported.
        virtual status_callback_t& on_market_data_status() {
            return m_status_callback;
        }

        /// \brief Requests a live tick stream subscription.
        /// \param request Tick subscription parameters.
        /// \param callback Callback receiving desired-subscription acceptance or failure.
        /// \return True if the provider accepted the operation for processing; false otherwise.
        virtual bool subscribe_ticks(
                TickSubscriptionRequest request,
                subscription_callback_t callback) {
            MarketDataSubscriptionBatch batch;
            batch.subscribe_ticks(std::move(request));
            return apply_subscriptions(
                std::move(batch),
                [callback = std::move(callback)](MarketDataSubscriptionBatchResult result) mutable {
                    dispatch_single_subscription_result(std::move(callback), std::move(result));
                });
        }

        /// \brief Requests a live bar stream subscription.
        /// \param request Bar subscription parameters.
        /// \param callback Callback receiving desired-subscription acceptance or failure.
        /// \return True if the provider accepted the operation for processing; false otherwise.
        virtual bool subscribe_bars(
                BarSubscriptionRequest request,
                subscription_callback_t callback) {
            MarketDataSubscriptionBatch batch;
            batch.subscribe_bars(std::move(request));
            return apply_subscriptions(
                std::move(batch),
                [callback = std::move(callback)](MarketDataSubscriptionBatchResult result) mutable {
                    dispatch_single_subscription_result(std::move(callback), std::move(result));
                });
        }

        /// \brief Stops a live market-data subscription.
        /// \param subscription Subscription handle returned by a successful subscribe call.
        /// \param callback Callback receiving unsubscribe status.
        /// \return True if the provider accepted the operation for processing; false otherwise.
        virtual bool unsubscribe(
                MarketDataSubscriptionHandle subscription,
                subscription_callback_t callback) {
            MarketDataSubscriptionBatch batch;
            batch.unsubscribe(std::move(subscription));
            return apply_subscriptions(
                std::move(batch),
                [callback = std::move(callback)](MarketDataSubscriptionBatchResult result) mutable {
                    dispatch_single_subscription_result(std::move(callback), std::move(result));
                });
        }

        /// \brief Applies a batch of live market-data subscription changes.
        /// \param batch Subscription changes to validate and apply as one operation.
        /// \param callback Callback receiving batch status and per-operation results.
        /// \return True if the provider accepted the operation for processing; false otherwise.
        virtual bool apply_subscriptions(
                MarketDataSubscriptionBatch batch,
                subscription_batch_callback_t callback) {
            if (batch.empty()) {
                dispatch_subscription_batch_result(
                    std::move(callback),
                    MarketDataSubscriptionBatchResult::failed(
                        MarketDataSubscriptionStatus::INVALID_REQUEST,
                        "Market-data subscription batch is empty."));
                return false;
            }

            std::vector<MarketDataSubscriptionResult> results;
            results.reserve(batch.changes.size());
            for (auto& change : batch.changes) {
                switch (change.action) {
                case MarketDataSubscriptionAction::SUBSCRIBE_TICKS:
                    if (!change.tick_request.valid()) {
                        results.push_back(MarketDataSubscriptionResult::failed(
                            std::move(change.tick_request),
                            MarketDataSubscriptionStatus::INVALID_REQUEST,
                            "Invalid tick subscription request."));
                    } else {
                        results.push_back(MarketDataSubscriptionResult::failed(
                            std::move(change.tick_request),
                            MarketDataSubscriptionStatus::UNSUPPORTED,
                            "Tick subscriptions are not supported by this provider."));
                    }
                    break;
                case MarketDataSubscriptionAction::SUBSCRIBE_BARS:
                    if (!change.bar_request.valid()) {
                        results.push_back(MarketDataSubscriptionResult::failed(
                            std::move(change.bar_request),
                            MarketDataSubscriptionStatus::INVALID_REQUEST,
                            "Invalid bar subscription request."));
                    } else {
                        results.push_back(MarketDataSubscriptionResult::failed(
                            std::move(change.bar_request),
                            MarketDataSubscriptionStatus::UNSUPPORTED,
                            "Bar subscriptions are not supported by this provider."));
                    }
                    break;
                case MarketDataSubscriptionAction::UNSUBSCRIBE:
                    if (!change.subscription.valid()) {
                        results.push_back(MarketDataSubscriptionResult::failed(
                            std::move(change.subscription),
                            MarketDataSubscriptionStatus::INVALID_REQUEST,
                            "Invalid market-data subscription handle."));
                    } else if (change.subscription.provider_id != provider_id()) {
                        results.push_back(MarketDataSubscriptionResult::failed(
                            std::move(change.subscription),
                            MarketDataSubscriptionStatus::WRONG_PROVIDER,
                            "Market-data subscription handle belongs to another provider."));
                    } else {
                        results.push_back(MarketDataSubscriptionResult::failed(
                            std::move(change.subscription),
                            MarketDataSubscriptionStatus::UNSUPPORTED,
                            "Market-data subscriptions are not supported by this provider."));
                    }
                    break;
                case MarketDataSubscriptionAction::UNSUBSCRIBE_ALL:
                    results.push_back(MarketDataSubscriptionResult::failed(
                        MarketDataSubscriptionHandle{},
                        MarketDataSubscriptionStatus::UNSUPPORTED,
                        "Market-data subscriptions are not supported by this provider."));
                    break;
                }
            }

            dispatch_subscription_batch_result(
                std::move(callback),
                MarketDataSubscriptionBatchResult::failed(
                    results.empty() ? MarketDataSubscriptionStatus::FAILED : results.front().status,
                    results.empty() ? "Market-data subscription batch failed." : results.front().error_message,
                    std::move(results)));
            return false;
        }

        /// \brief Stops all live market-data subscriptions owned by this provider.
        /// \param callback Callback receiving batch status.
        /// \return True if the provider accepted the operation for processing; false otherwise.
        virtual bool unsubscribe_all(subscription_batch_callback_t callback) {
            MarketDataSubscriptionBatch batch;
            batch.unsubscribe_all();
            return apply_subscriptions(std::move(batch), std::move(callback));
        }

    protected:
        /// \brief Delivers a subscription operation result when a callback was supplied.
        /// \param callback Optional callback supplied by the caller.
        /// \param result Typed operation result to deliver.
        static void dispatch_subscription_result(
                subscription_callback_t callback,
                MarketDataSubscriptionResult result) {
            if (callback) {
                callback(std::move(result));
            }
        }

        /// \brief Delivers a subscription batch result when a callback was supplied.
        static void dispatch_subscription_batch_result(
                subscription_batch_callback_t callback,
                MarketDataSubscriptionBatchResult result) {
            if (callback) {
                callback(std::move(result));
            }
        }

        /// \brief Delivers the first per-operation result from a batch callback wrapper.
        static void dispatch_single_subscription_result(
                subscription_callback_t callback,
                MarketDataSubscriptionBatchResult result) {
            if (!callback) return;
            if (!result.results.empty()) {
                callback(std::move(result.results.front()));
                return;
            }

            MarketDataSubscriptionResult single;
            single.status = MarketDataSubscriptionResult::failure_status_or_failed(result.status);
            single.error_message = std::move(result.error_message);
            callback(std::move(single));
        }

    public:
        /// \brief Requests historical bar data for a specified time range.
        /// \param request Historical bar-data request parameters.
        /// \param callback Callback function to receive bars or a failure reason.
        /// \return True if the request was accepted for processing; false otherwise.
        virtual bool fetch_bar_history(
                const BarHistoryRequest& request,
                bar_history_callback_t callback) {
            (void)request;
            (void)callback;
            return false;
        }

    private:
        ProviderInstanceId m_provider_id = kInvalidProviderInstanceId; ///< Runtime provider instance ID.
        bars_callback_t m_bar_data_callback; ///< Per-instance default live bar callback slot.
        ticks_callback_t m_tick_data_callback; ///< Per-instance default live tick callback slot.
        status_callback_t m_status_callback; ///< Per-instance default stream status callback slot.

        /// \brief Returns the next non-zero provider instance ID.
        static ProviderInstanceId next_provider_instance_id() {
            static std::atomic<ProviderInstanceId> counter{1};
            const auto id = counter.fetch_add(1, std::memory_order_relaxed);
            return id == kInvalidProviderInstanceId ? counter.fetch_add(1, std::memory_order_relaxed) : id;
        }
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_BASE_MARKET_DATA_PROVIDER_HPP_INCLUDED
