#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_HPP_INCLUDED

/// \file MarketDataRouter.hpp
/// \brief Defines subscription-scoped market-data routing utilities.

namespace optionx::market_data {

    namespace detail {
        class MarketDataRouterState;
    } // namespace detail

    /// \class RoutedSubscriptionId
    /// \brief Strong Router-local identifier of a logical market-data subscription.
    class RoutedSubscriptionId {
    public:
        /// \brief Constructs an invalid identifier.
        constexpr RoutedSubscriptionId() noexcept = default;

        /// \brief Returns true when the Router assigned an identifier.
        [[nodiscard]] constexpr bool valid() const noexcept {
            return m_value != 0;
        }

        /// \brief Allows identifiers to be checked directly in conditions.
        constexpr explicit operator bool() const noexcept {
            return valid();
        }

        /// \brief Returns the numeric value for logging and diagnostics.
        [[nodiscard]] constexpr std::uint64_t value() const noexcept {
            return m_value;
        }

        friend constexpr bool operator==(
                RoutedSubscriptionId lhs,
                RoutedSubscriptionId rhs) noexcept {
            return lhs.m_value == rhs.m_value;
        }

        friend constexpr bool operator!=(
                RoutedSubscriptionId lhs,
                RoutedSubscriptionId rhs) noexcept {
            return !(lhs == rhs);
        }

    private:
        std::uint64_t m_value = 0;

        explicit constexpr RoutedSubscriptionId(std::uint64_t value) noexcept
                : m_value(value) {}

        friend class detail::MarketDataRouterState;
    };

    /// \struct RoutedSubscriptionIdHash
    /// \brief Hashes a routed subscription identifier for unordered containers.
    struct RoutedSubscriptionIdHash {
        [[nodiscard]] std::size_t operator()(RoutedSubscriptionId id) const noexcept {
            return std::hash<std::uint64_t>{}(id.value());
        }
    };

    /// \class MarketDataProviderId
    /// \brief Stable application-assigned identifier of a registered provider.
    class MarketDataProviderId {
    public:
        /// \brief Constructs an invalid provider identifier.
        constexpr MarketDataProviderId() noexcept = default;

        /// \brief Constructs a stable provider identifier from application data.
        explicit constexpr MarketDataProviderId(std::uint64_t value) noexcept
                : m_value(value) {}

        /// \brief Returns true when the identifier can select a provider.
        [[nodiscard]] constexpr bool valid() const noexcept {
            return m_value != 0;
        }

        /// \brief Allows identifiers to be checked directly in conditions.
        constexpr explicit operator bool() const noexcept {
            return valid();
        }

        /// \brief Returns the application-assigned numeric value.
        [[nodiscard]] constexpr std::uint64_t value() const noexcept {
            return m_value;
        }

        friend constexpr bool operator==(
                MarketDataProviderId lhs,
                MarketDataProviderId rhs) noexcept {
            return lhs.m_value == rhs.m_value;
        }

        friend constexpr bool operator!=(
                MarketDataProviderId lhs,
                MarketDataProviderId rhs) noexcept {
            return !(lhs == rhs);
        }

    private:
        std::uint64_t m_value = 0;
    };

    /// \struct MarketDataProviderIdHash
    /// \brief Hashes an application-assigned provider identifier.
    struct MarketDataProviderIdHash {
        [[nodiscard]] std::size_t operator()(MarketDataProviderId id) const noexcept {
            return std::hash<std::uint64_t>{}(id.value());
        }
    };

    namespace detail {

        struct MarketDataRouterSubscriptionControl {
            mutable std::mutex mutex;
            RoutedSubscriptionId router_id;
            MarketDataProviderId registered_provider_id;
            MarketDataSubscriptionHandle provider_subscription;
            std::weak_ptr<MarketDataRouterState> router;
            bool active = false;
            bool released = false;
        };
    } // namespace detail

    /// \class MarketDataRouterSubscription
    /// \brief Move-only RAII owner of one subscription created by MarketDataRouter.
    /// \details Destroying or resetting the handle requests unsubscription. The
    ///          handle is created before an asynchronous provider necessarily
    ///          accepts the subscription, so valid() can be true while pending()
    ///          is also true. provider_subscription() becomes valid after the
    ///          provider reports SUBSCRIBED.
    class MarketDataRouterSubscription {
    public:
        /// \brief Default-constructs an empty handle.
        MarketDataRouterSubscription() = default;

        /// \brief Copy construction is disabled because ownership is unique.
        MarketDataRouterSubscription(const MarketDataRouterSubscription&) = delete;
        /// \brief Copy assignment is disabled because ownership is unique.
        MarketDataRouterSubscription& operator=(const MarketDataRouterSubscription&) = delete;

        /// \brief Transfers subscription ownership.
        MarketDataRouterSubscription(MarketDataRouterSubscription&& other) noexcept
                : m_control(std::move(other.m_control)) {}

        /// \brief Releases the current subscription and transfers ownership.
        MarketDataRouterSubscription& operator=(MarketDataRouterSubscription&& other) noexcept;

        /// \brief Requests unsubscription for an owned subscription.
        ~MarketDataRouterSubscription() {
            reset();
        }

        /// \brief Returns the stable router-local subscription ID.
        [[nodiscard]] RoutedSubscriptionId router_id() const noexcept;

        /// \brief Returns the provider-assigned subscription descriptor, if accepted.
        [[nodiscard]] MarketDataSubscriptionHandle provider_subscription() const;

        /// \brief Returns the stable registered provider ID used to create this route.
        /// \details Direct provider-reference subscriptions return an invalid ID.
        [[nodiscard]] MarketDataProviderId registered_provider_id() const;

        /// \brief Returns true while this object owns a pending or active route.
        [[nodiscard]] bool valid() const;

        /// \brief Returns true after the provider accepted the subscription.
        [[nodiscard]] bool active() const;

        /// \brief Returns true while provider acceptance is still pending.
        [[nodiscard]] bool pending() const {
            return valid() && !active();
        }

        /// \brief Allows handles to be used in boolean contexts.
        explicit operator bool() const {
            return valid();
        }

        /// \brief Explicitly releases the route and requests provider unsubscription.
        /// \param callback Optional callback receiving the provider unsubscribe result.
        /// \return True when an unsubscribe or pending cancellation was accepted.
        /// \details The logical route is released even when physical provider cleanup
        ///          fails. MarketDataRouter retains failed cleanup ownership for retry.
        bool unsubscribe(BaseMarketDataProvider::subscription_callback_t callback = {});

        /// \brief Releases the route without an unsubscribe completion callback.
        void reset() noexcept;

    private:
        std::shared_ptr<detail::MarketDataRouterSubscriptionControl> m_control;

        explicit MarketDataRouterSubscription(
                std::shared_ptr<detail::MarketDataRouterSubscriptionControl> control)
                : m_control(std::move(control)) {}

        friend class MarketDataRouter;
        friend class detail::MarketDataRouterState;
    };

    /// \class MarketDataRouter
    /// \brief Binds provider subscriptions to concrete subscriber objects.
    /// \details Unlike MarketDataHub, the router owns provider subscription
    ///          state. It routes ticks, bars, and statuses only to the subscriber
    ///          associated with each logical subscription. Stream-level provider
    ///          statuses are cached and replayed with concrete subscription
    ///          context when a matching late subscription is accepted.
    ///
    ///          Subscriber objects are stored as weak references. The caller
    ///          must keep each subscriber alive and must keep every provider
    ///          alive until the router and its pending provider operations are
    ///          shut down. A router owns the provider's three live market-data
    ///          callback slots while at least one route for that provider exists;
    ///          do not bind MarketDataHub or assign those callbacks concurrently.
    ///
    ///          Container access is synchronized and callbacks are never invoked
    ///          under the router mutex. Synchronous subscribe/unsubscribe methods
    ///          remain owner-loop operations. Applications with bot threads can
    ///          configure an owner dispatcher and use the posted helpers exposed
    ///          by MarketDataSubscriberBase. Provider completions and market-data
    ///          delivery are then marshalled back into the same owner loop.
    ///
    ///          Releasing a route stops delivery immediately. If the provider rejects
    ///          or fails physical unsubscription, the router retains that provider
    ///          handle, keeps the callback binding, and rejects new routes through the
    ///          affected provider until retry_failed_unsubscribes() succeeds.
    class MarketDataRouter : public lifecycle::ILifecycleModule {
    public:
        using SubscriptionHandle = MarketDataRouterSubscription; ///< Move-only route owner.
        using subscription_callback_t = BaseMarketDataProvider::subscription_callback_t; ///< Operation callback.
        using owner_task_t = std::function<void()>; ///< One owner-loop operation.
        using owner_dispatcher_t = std::function<bool(owner_task_t)>; ///< Thread-safe task ingress.

        /// \brief Constructs an empty router for direct owner-loop use.
        MarketDataRouter();

        /// \brief Constructs a router with cross-thread owner-loop dispatch.
        /// \details The dispatcher must be thread-safe, enqueue tasks in FIFO
        ///          order, and remain available until Router shutdown completes.
        ///          It must not execute posted work inline on a foreign caller.
        ///          Provider events are dropped if the configured dispatcher
        ///          rejects them during shutdown.
        explicit MarketDataRouter(owner_dispatcher_t owner_dispatcher);

        /// \brief Copy construction is disabled because provider callbacks are owned.
        MarketDataRouter(const MarketDataRouter&) = delete;
        /// \brief Copy assignment is disabled because provider callbacks are owned.
        MarketDataRouter& operator=(const MarketDataRouter&) = delete;
        /// \brief Moving is disabled because handles refer to one router state.
        MarketDataRouter(MarketDataRouter&&) = delete;
        /// \brief Move assignment is disabled because handles refer to one router state.
        MarketDataRouter& operator=(MarketDataRouter&&) = delete;

        /// \brief Requests shutdown for any routes still owned by this instance.
        /// \details Drain asynchronous provider operations before destruction.
        ~MarketDataRouter() override;

        /// \brief Adds a non-owning provider registration with stable aliases.
        /// \details Registration does not bind provider callbacks. Aliases are
        ///          exact, case-sensitive and unique inside this Router.
        /// \param id Stable non-zero application-assigned provider ID.
        /// \param provider Provider that outlives its registration and Router use.
        /// \param aliases Optional selection aliases for configuration-facing code.
        /// \return True when the complete registration was added atomically.
        bool register_provider(
                MarketDataProviderId id,
                BaseMarketDataProvider& provider,
                std::vector<std::string> aliases = {});

        /// \brief Adds one exact alias to an existing provider registration.
        bool add_provider_alias(MarketDataProviderId id, std::string alias);

        /// \brief Removes an idle provider registration and all of its aliases.
        /// \return False when the provider is unknown or still has Router routes.
        bool unregister_provider(MarketDataProviderId id);

        /// \brief Returns the number of registered provider entries.
        [[nodiscard]] std::size_t registered_provider_count() const;

        /// \brief Resolves a runtime provider instance to its stable registered ID.
        [[nodiscard]] MarketDataProviderId registered_provider_id(
                ProviderInstanceId provider_id) const;

        /// \brief Returns a copy of the aliases assigned to a registered provider.
        [[nodiscard]] std::vector<std::string> provider_aliases(
                MarketDataProviderId id) const;

        /// \brief Posts work to the configured provider owner loop.
        /// \return False when no dispatcher is configured or it rejects the task.
        bool post_to_owner(owner_task_t task) const;

        /// \brief Returns true when cross-thread owner-loop dispatch is configured.
        [[nodiscard]] bool has_owner_dispatcher() const noexcept;

        /// \brief Creates a tick route for a shared subscriber.
        /// \param provider Provider that owns the physical subscription.
        /// \param subscriber Subscriber that receives this route's events.
        /// \param request Tick stream request.
        /// \param callback Optional provider subscription result callback.
        /// \return Move-only owner of the pending or accepted route.
        SubscriptionHandle subscribe_ticks(
                BaseMarketDataProvider& provider,
                const std::shared_ptr<IMarketDataSubscriber>& subscriber,
                TickSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a tick route from an explicitly weak subscriber.
        SubscriptionHandle subscribe_ticks_weak(
                BaseMarketDataProvider& provider,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                TickSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a tick route through a stable provider registration ID.
        SubscriptionHandle subscribe_ticks(
                MarketDataProviderId provider_id,
                const std::shared_ptr<IMarketDataSubscriber>& subscriber,
                TickSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a tick route through a stable provider ID for a weak subscriber.
        SubscriptionHandle subscribe_ticks_weak(
                MarketDataProviderId provider_id,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                TickSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a tick route through an exact registered provider alias.
        SubscriptionHandle subscribe_ticks(
                std::string_view provider_alias,
                const std::shared_ptr<IMarketDataSubscriber>& subscriber,
                TickSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates an aliased tick route for a weak subscriber.
        SubscriptionHandle subscribe_ticks_weak(
                std::string_view provider_alias,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                TickSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a bar route for a shared subscriber.
        /// \param provider Provider that owns the physical subscription.
        /// \param subscriber Subscriber that receives this route's events.
        /// \param request Bar stream request.
        /// \param callback Optional provider subscription result callback.
        /// \return Move-only owner of the pending or accepted route.
        SubscriptionHandle subscribe_bars(
                BaseMarketDataProvider& provider,
                const std::shared_ptr<IMarketDataSubscriber>& subscriber,
                BarSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a bar route from an explicitly weak subscriber.
        SubscriptionHandle subscribe_bars_weak(
                BaseMarketDataProvider& provider,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                BarSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a bar route through a stable provider registration ID.
        SubscriptionHandle subscribe_bars(
                MarketDataProviderId provider_id,
                const std::shared_ptr<IMarketDataSubscriber>& subscriber,
                BarSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a bar route through a stable provider ID for a weak subscriber.
        SubscriptionHandle subscribe_bars_weak(
                MarketDataProviderId provider_id,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                BarSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates a bar route through an exact registered provider alias.
        SubscriptionHandle subscribe_bars(
                std::string_view provider_alias,
                const std::shared_ptr<IMarketDataSubscriber>& subscriber,
                BarSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Creates an aliased bar route for a weak subscriber.
        SubscriptionHandle subscribe_bars_weak(
                std::string_view provider_alias,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                BarSubscriptionRequest request,
                subscription_callback_t callback = {});

        /// \brief Returns the number of owned provider subscription entries.
        /// \details Includes pending routes, active routes, and failed physical
        ///          unsubscriptions retained for cleanup.
        [[nodiscard]] std::size_t subscription_count() const;

        /// \brief Returns the number of failed physical unsubscriptions awaiting retry.
        [[nodiscard]] std::size_t failed_unsubscribe_count() const;

        /// \brief Retries physical cleanup retained after unsubscribe failures.
        /// \return Number of retry operations accepted by providers.
        /// \details Call from the same owner loop used for subscribe and provider
        ///          publish operations. A provider remains quarantined until every
        ///          retained cleanup for it completes successfully.
        std::size_t retry_failed_unsubscribes();

        /// \brief Advances deferred Router lifecycle work on the owner loop.
        /// \details Processes provider completions retained during shutdown and
        ///          starts or completes physical subscription cleanup. This method
        ///          does not poll providers or transport data.
        void process() override;

        /// \brief Returns true after shutdown drained every provider operation.
        [[nodiscard]] bool is_shutdown_complete() const noexcept;

        /// \brief Returns the common lifecycle terminal state.
        [[nodiscard]] bool is_stopped() const noexcept override {
            return is_shutdown_complete();
        }

        /// \brief Starts an idempotent graceful shutdown.
        /// \details New routes and user delivery stop immediately. Pending provider
        ///          operations remain owned until later process() calls finish their
        ///          physical cleanup on the owner loop.
        void shutdown() noexcept override;

    private:
        std::shared_ptr<detail::MarketDataRouterState> m_state;
    };

} // namespace optionx::market_data

#include "detail/MarketDataRouter.ipp"

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_HPP_INCLUDED
