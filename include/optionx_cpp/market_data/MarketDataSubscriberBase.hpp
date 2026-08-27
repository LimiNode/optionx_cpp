#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_SUBSCRIBER_BASE_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_SUBSCRIBER_BASE_HPP_INCLUDED

/// \file MarketDataSubscriberBase.hpp
/// \brief Defines a convenience base for self-subscribing market-data consumers.

namespace optionx::market_data {

    /// \class MarketDataSubscriberBase
    /// \brief Stores Router subscription handles for a subscriber implementation.
    /// \details Derive bots, charts, or time-series services from this class when
    ///          they should create subscriptions from their own methods. The base
    ///          keeps move-only Router handles alive and releases them during
    ///          explicit unsubscribe or destruction.
    ///
    ///          Instances must be owned by std::shared_ptr before calling the
    ///          protected subscribe methods. Do not add another
    ///          enable_shared_from_this base to the derived class. The referenced
    ///          MarketDataRouter must outlive this subscriber while subscribe
    ///          methods can still be called. Provider completion and cached
    ///          status replay may run synchronously before a subscribe helper
    ///          returns; callbacks should use the concrete subscription carried
    ///          by the event instead of assuming the returned Router ID was
    ///          already stored in a derived field.
    class MarketDataSubscriberBase
            : public IMarketDataSubscriber,
              public std::enable_shared_from_this<MarketDataSubscriberBase> {
    public:
        using subscription_callback_t = MarketDataRouter::subscription_callback_t; ///< Operation callback.

        /// \brief Binds this subscriber helper to a Router.
        /// \param router Router that creates and owns provider routes.
        explicit MarketDataSubscriberBase(MarketDataRouter& router) noexcept
                : m_router(&router) {}

        /// \brief Copy construction is disabled because handles have unique ownership.
        MarketDataSubscriberBase(const MarketDataSubscriberBase&) = delete;
        /// \brief Copy assignment is disabled because handles have unique ownership.
        MarketDataSubscriberBase& operator=(const MarketDataSubscriberBase&) = delete;
        /// \brief Moving is disabled because Router stores a weak pointer to this object.
        MarketDataSubscriberBase(MarketDataSubscriberBase&&) = delete;
        /// \brief Move assignment is disabled because Router stores a weak pointer to this object.
        MarketDataSubscriberBase& operator=(MarketDataSubscriberBase&&) = delete;

        /// \brief Releases all subscriptions owned by this subscriber.
        ~MarketDataSubscriberBase() override {
            try {
                unsubscribe_all();
            } catch (...) {
            }
        }

    protected:
        /// \brief Creates and stores a tick subscription for this subscriber.
        /// \param provider Provider that owns the physical subscription.
        /// \param request Tick stream request.
        /// \param callback Optional provider subscription result callback.
        /// \return Stable Router ID, or a default-invalid ID on failure.
        RoutedSubscriptionId subscribe_ticks(
                BaseMarketDataProvider& provider,
                TickSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a tick subscription through a stable provider ID.
        RoutedSubscriptionId subscribe_ticks(
                MarketDataProviderId provider_id,
                TickSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a tick subscription through an exact provider alias.
        RoutedSubscriptionId subscribe_ticks(
                std::string_view provider_alias,
                TickSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates and stores a bar subscription for this subscriber.
        /// \param provider Provider that owns the physical subscription.
        /// \param request Bar stream request.
        /// \param callback Optional provider subscription result callback.
        /// \return Stable Router ID, or a default-invalid ID on failure.
        RoutedSubscriptionId subscribe_bars(
                BaseMarketDataProvider& provider,
                BarSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a bar subscription through a stable provider ID.
        RoutedSubscriptionId subscribe_bars(
                MarketDataProviderId provider_id,
                BarSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a bar subscription through an exact provider alias.
        RoutedSubscriptionId subscribe_bars(
                std::string_view provider_alias,
                BarSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Explicitly releases one stored subscription.
        /// \param router_id Router-local ID returned by subscribe_ticks/bars.
        /// \param callback Optional provider unsubscribe result callback.
        /// \return True when pending cancellation or provider unsubscribe was accepted.
        bool unsubscribe(
                RoutedSubscriptionId router_id,
                subscription_callback_t callback = {});

        /// \brief Releases every subscription currently owned by this subscriber.
        void unsubscribe_all();

        /// \brief Returns the number of pending and active stored subscriptions.
        [[nodiscard]] std::size_t subscription_count() const;

        /// \brief Returns true when a pending or active route ID is stored.
        [[nodiscard]] bool has_subscription(RoutedSubscriptionId router_id) const;

        /// \brief Returns the provider descriptor assigned to a stored route.
        /// \details The descriptor is invalid while provider acceptance is pending.
        [[nodiscard]] MarketDataSubscriptionHandle provider_subscription(
                RoutedSubscriptionId router_id) const;

        /// \brief Returns the Router used by this subscriber helper.
        [[nodiscard]] MarketDataRouter& market_data_router() const noexcept {
            return *m_router;
        }

    private:
        using subscription_map_t =
            std::unordered_map<
                RoutedSubscriptionId,
                MarketDataRouter::SubscriptionHandle,
                RoutedSubscriptionIdHash>;

        MarketDataRouter* m_router = nullptr; ///< Non-owning Router reference.
        mutable std::mutex m_subscription_mutex; ///< Protects stored handles.
        mutable subscription_map_t m_subscriptions; ///< Handles owned by this subscriber.

        /// \brief Removes handles whose async provider operation already failed.
        void prune_released_no_lock() const;

        /// \brief Stores a Router handle and returns its strong route ID.
        RoutedSubscriptionId store_route(MarketDataRouter::SubscriptionHandle route);

        /// \brief Returns this object as the weak receiving interface used by Router.
        std::weak_ptr<IMarketDataSubscriber> weak_subscriber() noexcept {
            return std::weak_ptr<IMarketDataSubscriber>(weak_from_this());
        }
    };

    inline RoutedSubscriptionId MarketDataSubscriberBase::subscribe_ticks(
            BaseMarketDataProvider& provider,
            TickSubscriptionRequest request,
            subscription_callback_t callback) {
        return store_route(m_router->subscribe_ticks_weak(
            provider,
            weak_subscriber(),
            std::move(request),
            std::move(callback)));
    }

    inline RoutedSubscriptionId MarketDataSubscriberBase::subscribe_ticks(
            MarketDataProviderId provider_id,
            TickSubscriptionRequest request,
            subscription_callback_t callback) {
        return store_route(m_router->subscribe_ticks_weak(
            provider_id,
            weak_subscriber(),
            std::move(request),
            std::move(callback)));
    }

    inline RoutedSubscriptionId MarketDataSubscriberBase::subscribe_ticks(
            std::string_view provider_alias,
            TickSubscriptionRequest request,
            subscription_callback_t callback) {
        return store_route(m_router->subscribe_ticks_weak(
            provider_alias,
            weak_subscriber(),
            std::move(request),
            std::move(callback)));
    }

    inline RoutedSubscriptionId MarketDataSubscriberBase::subscribe_bars(
            BaseMarketDataProvider& provider,
            BarSubscriptionRequest request,
            subscription_callback_t callback) {
        return store_route(m_router->subscribe_bars_weak(
            provider,
            weak_subscriber(),
            std::move(request),
            std::move(callback)));
    }

    inline RoutedSubscriptionId MarketDataSubscriberBase::subscribe_bars(
            MarketDataProviderId provider_id,
            BarSubscriptionRequest request,
            subscription_callback_t callback) {
        return store_route(m_router->subscribe_bars_weak(
            provider_id,
            weak_subscriber(),
            std::move(request),
            std::move(callback)));
    }

    inline RoutedSubscriptionId MarketDataSubscriberBase::subscribe_bars(
            std::string_view provider_alias,
            BarSubscriptionRequest request,
            subscription_callback_t callback) {
        return store_route(m_router->subscribe_bars_weak(
            provider_alias,
            weak_subscriber(),
            std::move(request),
            std::move(callback)));
    }

    inline bool MarketDataSubscriberBase::unsubscribe(
            RoutedSubscriptionId router_id,
            subscription_callback_t callback) {
        if (!router_id.valid()) return false;

        MarketDataRouter::SubscriptionHandle route;
        {
            std::lock_guard<std::mutex> lock(m_subscription_mutex);
            prune_released_no_lock();
            const auto it = m_subscriptions.find(router_id);
            if (it == m_subscriptions.end()) return false;
            route = std::move(it->second);
            m_subscriptions.erase(it);
        }
        return route.unsubscribe(std::move(callback));
    }

    inline void MarketDataSubscriberBase::unsubscribe_all() {
        subscription_map_t subscriptions;
        {
            std::lock_guard<std::mutex> lock(m_subscription_mutex);
            subscriptions.swap(m_subscriptions);
        }
        subscriptions.clear();
    }

    inline std::size_t MarketDataSubscriberBase::subscription_count() const {
        std::lock_guard<std::mutex> lock(m_subscription_mutex);
        prune_released_no_lock();
        return m_subscriptions.size();
    }

    inline bool MarketDataSubscriberBase::has_subscription(
            RoutedSubscriptionId router_id) const {
        if (!router_id.valid()) return false;
        std::lock_guard<std::mutex> lock(m_subscription_mutex);
        prune_released_no_lock();
        return m_subscriptions.find(router_id) != m_subscriptions.end();
    }

    inline MarketDataSubscriptionHandle MarketDataSubscriberBase::provider_subscription(
            RoutedSubscriptionId router_id) const {
        if (!router_id.valid()) return {};
        std::lock_guard<std::mutex> lock(m_subscription_mutex);
        prune_released_no_lock();
        const auto it = m_subscriptions.find(router_id);
        return it == m_subscriptions.end()
            ? MarketDataSubscriptionHandle{}
            : it->second.provider_subscription();
    }

    inline RoutedSubscriptionId MarketDataSubscriberBase::store_route(
            MarketDataRouter::SubscriptionHandle route) {
        if (!route.valid()) return {};

        const auto router_id = route.router_id();
        {
            std::lock_guard<std::mutex> lock(m_subscription_mutex);
            prune_released_no_lock();
            m_subscriptions.emplace(router_id, std::move(route));
        }
        return router_id;
    }

    inline void MarketDataSubscriberBase::prune_released_no_lock() const {
        for (auto it = m_subscriptions.begin(); it != m_subscriptions.end();) {
            if (!it->second.valid()) {
                it = m_subscriptions.erase(it);
            } else {
                ++it;
            }
        }
    }

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_SUBSCRIBER_BASE_HPP_INCLUDED
