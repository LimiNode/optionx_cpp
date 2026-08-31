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
    class MarketDataRouter {
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

        /// \brief Stops routed subscriptions and releases provider callbacks.
        ~MarketDataRouter();

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

        /// \brief Stops all routes and clears provider callback bindings.
        void shutdown() noexcept;

    private:
        std::shared_ptr<detail::MarketDataRouterState> m_state;
    };

    namespace detail {

        class MarketDataRouterState final
                : public std::enable_shared_from_this<MarketDataRouterState> {
        public:
            using subscription_callback_t = BaseMarketDataProvider::subscription_callback_t;

            explicit MarketDataRouterState(
                    MarketDataRouter::owner_dispatcher_t owner_dispatcher = {})
                    : m_owner_dispatcher(std::move(owner_dispatcher)) {}

            struct StreamDescriptor {
                MarketDataType type = MarketDataType::UNKNOWN;
                std::string symbol;
                BarTimeframe timeframe = 0;
                BarPriceSource price_source = BarPriceSource::MID;
                MarketDataTransport transport = MarketDataTransport::AUTO;
            };

            enum class EntryPhase {
                PENDING,
                ACTIVE,
                UNSUBSCRIBING,
                CLEANUP_FAILED
            };

            struct Entry {
                RoutedSubscriptionId router_id;
                ProviderInstanceId provider_id = kInvalidProviderInstanceId;
                BaseMarketDataProvider* provider = nullptr;
                std::weak_ptr<IMarketDataSubscriber> subscriber;
                std::shared_ptr<MarketDataRouterSubscriptionControl> control;
                StreamDescriptor stream;
                MarketDataSubscriptionHandle retained_cleanup_subscription;
                EntryPhase phase = EntryPhase::PENDING;
                bool release_requested = false;
                subscription_callback_t release_callback;
            };

            struct CachedStatus {
                MarketDataStatusUpdate update;
                std::uint64_t sequence = 0;
            };

            struct ProviderSlot {
                BaseMarketDataProvider* provider = nullptr;
                std::size_t route_count = 0;
                std::unordered_map<SubscriptionId, RoutedSubscriptionId> provider_routes;
                std::vector<CachedStatus> statuses;
                std::uint64_t next_status_sequence = 1;
            };

            struct RegisteredProvider {
                BaseMarketDataProvider* provider = nullptr;
                ProviderInstanceId instance_id = kInvalidProviderInstanceId;
                std::vector<std::string> aliases;
            };

            bool register_provider(
                    MarketDataProviderId id,
                    BaseMarketDataProvider& provider,
                    std::vector<std::string> aliases);
            bool add_provider_alias(MarketDataProviderId id, std::string alias);
            bool unregister_provider(MarketDataProviderId id);
            [[nodiscard]] std::size_t registered_provider_count() const;
            [[nodiscard]] MarketDataProviderId registered_provider_id(
                    ProviderInstanceId provider_id) const;
            [[nodiscard]] std::vector<std::string> provider_aliases(
                    MarketDataProviderId id) const;

            MarketDataRouterSubscription subscribe_ticks(
                    BaseMarketDataProvider& provider,
                    std::weak_ptr<IMarketDataSubscriber> subscriber,
                    TickSubscriptionRequest request,
                    subscription_callback_t callback,
                    MarketDataProviderId registered_provider_id = {});

            MarketDataRouterSubscription subscribe_ticks(
                    MarketDataProviderId provider_id,
                    std::weak_ptr<IMarketDataSubscriber> subscriber,
                    TickSubscriptionRequest request,
                    subscription_callback_t callback);

            MarketDataRouterSubscription subscribe_ticks(
                    std::string_view provider_alias,
                    std::weak_ptr<IMarketDataSubscriber> subscriber,
                    TickSubscriptionRequest request,
                    subscription_callback_t callback);

            MarketDataRouterSubscription subscribe_bars(
                    BaseMarketDataProvider& provider,
                    std::weak_ptr<IMarketDataSubscriber> subscriber,
                    BarSubscriptionRequest request,
                    subscription_callback_t callback,
                    MarketDataProviderId registered_provider_id = {});

            MarketDataRouterSubscription subscribe_bars(
                    MarketDataProviderId provider_id,
                    std::weak_ptr<IMarketDataSubscriber> subscriber,
                    BarSubscriptionRequest request,
                    subscription_callback_t callback);

            MarketDataRouterSubscription subscribe_bars(
                    std::string_view provider_alias,
                    std::weak_ptr<IMarketDataSubscriber> subscriber,
                    BarSubscriptionRequest request,
                    subscription_callback_t callback);

            bool unsubscribe(
                    const std::shared_ptr<MarketDataRouterSubscriptionControl>& control,
                    subscription_callback_t callback);

            [[nodiscard]] std::size_t subscription_count() const;
            [[nodiscard]] std::size_t failed_unsubscribe_count() const;
            std::size_t retry_failed_unsubscribes();
            void shutdown() noexcept;

            bool post_to_owner(MarketDataRouter::owner_task_t task) const;
            [[nodiscard]] bool has_owner_dispatcher() const noexcept {
                return static_cast<bool>(m_owner_dispatcher);
            }

            void route_ticks(
                    ProviderInstanceId provider_id,
                    std::unique_ptr<TickDataBatch> batch);
            void route_bars(
                    ProviderInstanceId provider_id,
                    std::unique_ptr<BarDataBatch> batch);
            void route_status(
                    ProviderInstanceId provider_id,
                    MarketDataStatusUpdate update);

        private:
            mutable std::mutex m_mutex;
            std::unordered_map<
                RoutedSubscriptionId,
                std::shared_ptr<Entry>,
                RoutedSubscriptionIdHash> m_entries;
            std::unordered_map<ProviderInstanceId, ProviderSlot> m_providers;
            std::unordered_map<
                MarketDataProviderId,
                RegisteredProvider,
                MarketDataProviderIdHash> m_registered_providers;
            std::unordered_map<std::string, MarketDataProviderId> m_provider_aliases;
            std::unordered_map<ProviderInstanceId, MarketDataProviderId>
                m_registered_provider_ids;
            std::uint64_t m_next_router_id = 1;
            bool m_shutdown = false;
            MarketDataRouter::owner_dispatcher_t m_owner_dispatcher;

            static StreamDescriptor stream_from(const TickSubscriptionRequest& request);
            static StreamDescriptor stream_from(const BarSubscriptionRequest& request);
            static StreamDescriptor stream_from(const MarketDataSubscriptionHandle& subscription);

            static bool same_status_stream(
                    const MarketDataStatusUpdate& lhs,
                    const MarketDataStatusUpdate& rhs) noexcept;
            static bool transport_matches(
                    MarketDataTransport expected,
                    MarketDataTransport actual) noexcept;
            static bool status_matches_stream(
                    const MarketDataStatusUpdate& update,
                    const StreamDescriptor& stream) noexcept;
            static bool batch_matches_stream(
                    const TickDataBatch& batch,
                    const StreamDescriptor& stream) noexcept;
            static bool batch_matches_stream(
                    const BarDataBatch& batch,
                    const StreamDescriptor& stream) noexcept;

            bool bind_provider(BaseMarketDataProvider& provider, std::string& error_message);
            void unbind_provider(BaseMarketDataProvider& provider) const noexcept;

            std::shared_ptr<MarketDataRouterSubscriptionControl> add_pending_entry(
                    BaseMarketDataProvider& provider,
                    std::weak_ptr<IMarketDataSubscriber> subscriber,
                    StreamDescriptor stream,
                    MarketDataProviderId registered_provider_id,
                    std::string& error_message);

            BaseMarketDataProvider* registered_provider_no_lock(
                    MarketDataProviderId id) const noexcept;
            MarketDataProviderId provider_id_for_alias_no_lock(
                    std::string_view alias) const;

            void complete_subscribe(
                    RoutedSubscriptionId router_id,
                    BaseMarketDataProvider& provider,
                    MarketDataSubscriptionResult result,
                    subscription_callback_t callback);

            void fail_pending_subscribe(
                    RoutedSubscriptionId router_id,
                    BaseMarketDataProvider& provider,
                    MarketDataSubscriptionResult result,
                    subscription_callback_t callback);

            void complete_unsubscribe(
                    RoutedSubscriptionId router_id,
                    MarketDataSubscriptionHandle expected_subscription,
                    MarketDataSubscriptionResult result,
                    subscription_callback_t callback);

            BaseMarketDataProvider* remove_entry_no_lock(RoutedSubscriptionId router_id);
            void cache_status_no_lock(ProviderSlot& slot, MarketDataStatusUpdate update);
            bool replay_status_no_lock(
                    const ProviderSlot& slot,
                    const StreamDescriptor& stream,
                    MarketDataStatusUpdate& update) const;

            static void set_control_active(
                    const std::shared_ptr<MarketDataRouterSubscriptionControl>& control,
                    const MarketDataSubscriptionHandle& subscription);
            static void set_control_released(
                    const std::shared_ptr<MarketDataRouterSubscriptionControl>& control);
            static void dispatch_result(
                    subscription_callback_t callback,
                    MarketDataSubscriptionResult result);

            bool dispatch_or_run(MarketDataRouter::owner_task_t task) const;
            void retain_rejected_subscribe_cleanup(
                    RoutedSubscriptionId router_id,
                    BaseMarketDataProvider& provider,
                    MarketDataSubscriptionResult& result);

            friend class ::optionx::market_data::MarketDataRouterSubscription;
        };

        inline bool MarketDataRouterState::post_to_owner(
                MarketDataRouter::owner_task_t task) const {
            if (!task || !m_owner_dispatcher) return false;
            try {
                return m_owner_dispatcher(std::move(task));
            } catch (...) {
                return false;
            }
        }

        inline bool MarketDataRouterState::dispatch_or_run(
                MarketDataRouter::owner_task_t task) const {
            if (!task) return false;
            if (!m_owner_dispatcher) {
                task();
                return true;
            }

            return post_to_owner(std::move(task));
        }

        inline void MarketDataRouterState::retain_rejected_subscribe_cleanup(
                RoutedSubscriptionId router_id,
                BaseMarketDataProvider& provider,
                MarketDataSubscriptionResult& result) {
            if (!result.success() ||
                !result.subscription.valid() ||
                result.subscription.provider_id != provider.provider_id()) {
                return;
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            const auto entry_it = m_entries.find(router_id);
            if (m_shutdown ||
                entry_it == m_entries.end() ||
                entry_it->second->provider != &provider ||
                entry_it->second->phase != EntryPhase::PENDING ||
                entry_it->second->retained_cleanup_subscription.valid()) {
                return;
            }

            entry_it->second->retained_cleanup_subscription =
                std::move(result.subscription);
        }

        inline MarketDataRouterState::StreamDescriptor
        MarketDataRouterState::stream_from(const TickSubscriptionRequest& request) {
            StreamDescriptor stream;
            stream.type = MarketDataType::TICKS;
            stream.symbol = request.symbol;
            stream.transport = request.transport;
            return stream;
        }

        inline MarketDataRouterState::StreamDescriptor
        MarketDataRouterState::stream_from(const BarSubscriptionRequest& request) {
            StreamDescriptor stream;
            stream.type = MarketDataType::BARS;
            stream.symbol = request.symbol;
            stream.timeframe = request.timeframe;
            stream.price_source = request.price_source;
            stream.transport = request.transport;
            return stream;
        }

        inline MarketDataRouterState::StreamDescriptor
        MarketDataRouterState::stream_from(
                const MarketDataSubscriptionHandle& subscription) {
            StreamDescriptor stream;
            stream.type = subscription.stream_type;
            stream.symbol = subscription.symbol;
            stream.timeframe = subscription.timeframe;
            stream.price_source = subscription.price_source;
            stream.transport = subscription.transport;
            return stream;
        }

        inline bool MarketDataRouterState::register_provider(
                MarketDataProviderId id,
                BaseMarketDataProvider& provider,
                std::vector<std::string> aliases) {
            if (!id.valid()) return false;
            for (std::size_t i = 0; i < aliases.size(); ++i) {
                if (aliases[i].empty()) return false;
                for (std::size_t j = i + 1; j < aliases.size(); ++j) {
                    if (aliases[i] == aliases[j]) return false;
                }
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown ||
                m_registered_providers.find(id) != m_registered_providers.end() ||
                m_registered_provider_ids.find(provider.provider_id()) !=
                    m_registered_provider_ids.end()) {
                return false;
            }
            for (const auto& alias : aliases) {
                if (m_provider_aliases.find(alias) != m_provider_aliases.end()) {
                    return false;
                }
            }

            RegisteredProvider registration;
            registration.provider = &provider;
            registration.instance_id = provider.provider_id();
            registration.aliases = std::move(aliases);
            for (const auto& alias : registration.aliases) {
                m_provider_aliases.emplace(alias, id);
            }
            m_registered_provider_ids.emplace(registration.instance_id, id);
            m_registered_providers.emplace(id, std::move(registration));
            return true;
        }

        inline bool MarketDataRouterState::add_provider_alias(
                MarketDataProviderId id,
                std::string alias) {
            if (!id.valid() || alias.empty()) return false;

            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown) return false;
            const auto registration_it = m_registered_providers.find(id);
            if (registration_it == m_registered_providers.end()) return false;

            const auto alias_it = m_provider_aliases.find(alias);
            if (alias_it != m_provider_aliases.end()) {
                return alias_it->second == id;
            }
            registration_it->second.aliases.push_back(alias);
            m_provider_aliases.emplace(std::move(alias), id);
            return true;
        }

        inline bool MarketDataRouterState::unregister_provider(MarketDataProviderId id) {
            if (!id.valid()) return false;

            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown) return false;
            const auto registration_it = m_registered_providers.find(id);
            if (registration_it == m_registered_providers.end()) return false;

            for (const auto& [router_id, entry] : m_entries) {
                (void)router_id;
                if (entry->provider_id == registration_it->second.instance_id) {
                    return false;
                }
            }
            for (const auto& alias : registration_it->second.aliases) {
                m_provider_aliases.erase(alias);
            }
            m_registered_provider_ids.erase(registration_it->second.instance_id);
            m_registered_providers.erase(registration_it);
            return true;
        }

        inline std::size_t MarketDataRouterState::registered_provider_count() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_registered_providers.size();
        }

        inline MarketDataProviderId MarketDataRouterState::registered_provider_id(
                ProviderInstanceId provider_id) const {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_registered_provider_ids.find(provider_id);
            return it == m_registered_provider_ids.end()
                ? MarketDataProviderId{}
                : it->second;
        }

        inline std::vector<std::string> MarketDataRouterState::provider_aliases(
                MarketDataProviderId id) const {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_registered_providers.find(id);
            return it == m_registered_providers.end()
                ? std::vector<std::string>{}
                : it->second.aliases;
        }

        inline BaseMarketDataProvider* MarketDataRouterState::registered_provider_no_lock(
                MarketDataProviderId id) const noexcept {
            const auto it = m_registered_providers.find(id);
            return it == m_registered_providers.end() ? nullptr : it->second.provider;
        }

        inline MarketDataProviderId MarketDataRouterState::provider_id_for_alias_no_lock(
                std::string_view alias) const {
            const auto it = m_provider_aliases.find(std::string(alias));
            return it == m_provider_aliases.end() ? MarketDataProviderId{} : it->second;
        }

        inline bool MarketDataRouterState::same_status_stream(
                const MarketDataStatusUpdate& lhs,
                const MarketDataStatusUpdate& rhs) noexcept {
            return lhs.type == rhs.type &&
                   lhs.symbol == rhs.symbol &&
                   lhs.timeframe == rhs.timeframe &&
                   lhs.transport == rhs.transport;
        }

        inline bool MarketDataRouterState::transport_matches(
                MarketDataTransport expected,
                MarketDataTransport actual) noexcept {
            return expected == actual ||
                   expected == MarketDataTransport::AUTO ||
                   expected == MarketDataTransport::HYBRID ||
                   actual == MarketDataTransport::AUTO ||
                   actual == MarketDataTransport::HYBRID;
        }

        inline bool MarketDataRouterState::status_matches_stream(
                const MarketDataStatusUpdate& update,
                const StreamDescriptor& stream) noexcept {
            return update.type == stream.type &&
                   update.symbol == stream.symbol &&
                   update.timeframe == stream.timeframe &&
                   transport_matches(stream.transport, update.transport);
        }

        inline bool MarketDataRouterState::batch_matches_stream(
                const TickDataBatch& batch,
                const StreamDescriptor& stream) noexcept {
            return stream.type == MarketDataType::TICKS &&
                   batch.type == MarketDataType::TICKS &&
                   batch.symbol == stream.symbol;
        }

        inline bool MarketDataRouterState::batch_matches_stream(
                const BarDataBatch& batch,
                const StreamDescriptor& stream) noexcept {
            return stream.type == MarketDataType::BARS &&
                   batch.type == MarketDataType::BARS &&
                   batch.symbol == stream.symbol &&
                   batch.timeframe == stream.timeframe;
        }

        inline bool MarketDataRouterState::bind_provider(
                BaseMarketDataProvider& provider,
                std::string& error_message) {
            const auto provider_id = provider.provider_id();
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown) {
                error_message = "MarketDataRouter is shut down.";
                return false;
            }

            const auto existing = m_providers.find(provider_id);
            if (existing != m_providers.end()) {
                if (existing->second.provider == &provider) return true;
                error_message = "Market-data provider runtime ID is already bound.";
                return false;
            }

            if (provider.on_tick_data() ||
                provider.on_bar_data() ||
                provider.on_market_data_status()) {
                error_message =
                    "Market-data provider callbacks are already assigned; "
                    "unbind the current hub or callback owner first.";
                return false;
            }

            const auto weak_state = std::weak_ptr<MarketDataRouterState>(shared_from_this());
            provider.on_tick_data() =
                [weak_state, provider_id](std::unique_ptr<TickDataBatch> batch) {
                    if (const auto state = weak_state.lock()) {
                        auto pending = std::make_shared<std::unique_ptr<TickDataBatch>>(
                            std::move(batch));
                        state->dispatch_or_run(
                            [state, provider_id, pending]() mutable {
                                state->route_ticks(provider_id, std::move(*pending));
                            });
                    }
                };
            provider.on_bar_data() =
                [weak_state, provider_id](std::unique_ptr<BarDataBatch> batch) {
                    if (const auto state = weak_state.lock()) {
                        auto pending = std::make_shared<std::unique_ptr<BarDataBatch>>(
                            std::move(batch));
                        state->dispatch_or_run(
                            [state, provider_id, pending]() mutable {
                                state->route_bars(provider_id, std::move(*pending));
                            });
                    }
                };
            provider.on_market_data_status() =
                [weak_state, provider_id](MarketDataStatusUpdate update) {
                    if (const auto state = weak_state.lock()) {
                        auto pending = std::make_shared<MarketDataStatusUpdate>(
                            std::move(update));
                        state->dispatch_or_run(
                            [state, provider_id, pending]() mutable {
                                state->route_status(provider_id, std::move(*pending));
                            });
                    }
                };

            ProviderSlot slot;
            slot.provider = &provider;
            m_providers.emplace(provider_id, std::move(slot));
            return true;
        }

        inline void MarketDataRouterState::unbind_provider(
                BaseMarketDataProvider& provider) const noexcept {
            try {
                provider.on_tick_data() = BaseMarketDataProvider::ticks_callback_t{};
                provider.on_bar_data() = BaseMarketDataProvider::bars_callback_t{};
                provider.on_market_data_status() = BaseMarketDataProvider::status_callback_t{};
            } catch (...) {
            }
        }

        inline std::shared_ptr<MarketDataRouterSubscriptionControl>
        MarketDataRouterState::add_pending_entry(
                BaseMarketDataProvider& provider,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                StreamDescriptor stream,
                MarketDataProviderId registered_provider_id,
                std::string& error_message) {
            if (subscriber.expired()) {
                error_message = "Market-data subscriber is null or expired.";
                return {};
            }
            if (!bind_provider(provider, error_message)) return {};

            auto control = std::make_shared<MarketDataRouterSubscriptionControl>();
            auto entry = std::make_shared<Entry>();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_shutdown) {
                    error_message = "MarketDataRouter is shut down.";
                    return {};
                }

                auto provider_it = m_providers.find(provider.provider_id());
                if (provider_it == m_providers.end() ||
                    provider_it->second.provider != &provider) {
                    error_message = "Market-data provider binding was released.";
                    return {};
                }

                for (const auto& [id, existing_entry] : m_entries) {
                    (void)id;
                    if (existing_entry->provider_id == provider.provider_id() &&
                        (existing_entry->phase == EntryPhase::UNSUBSCRIBING ||
                         existing_entry->phase == EntryPhase::CLEANUP_FAILED ||
                         existing_entry->retained_cleanup_subscription.valid())) {
                        error_message =
                            "Market-data provider has pending or failed physical "
                            "subscription cleanup; wait for completion or retry failed "
                            "unsubscriptions before creating new routes.";
                        return {};
                    }
                }

                auto router_id_value = m_next_router_id++;
                if (router_id_value == 0) {
                    router_id_value = m_next_router_id++;
                }
                const RoutedSubscriptionId router_id(router_id_value);

                control->router_id = router_id;
                control->registered_provider_id = registered_provider_id;
                control->router = shared_from_this();
                entry->router_id = router_id;
                entry->provider_id = provider.provider_id();
                entry->provider = &provider;
                entry->subscriber = std::move(subscriber);
                entry->control = control;
                entry->stream = std::move(stream);
                m_entries.emplace(router_id, entry);
                ++provider_it->second.route_count;
            }
            return control;
        }

        inline MarketDataRouterSubscription MarketDataRouterState::subscribe_ticks(
                BaseMarketDataProvider& provider,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                TickSubscriptionRequest request,
                subscription_callback_t callback,
                MarketDataProviderId registered_provider_id) {
            if (!request.valid()) {
                dispatch_result(
                    std::move(callback),
                    MarketDataSubscriptionResult::failed(
                        std::move(request),
                        MarketDataSubscriptionStatus::INVALID_REQUEST,
                        "Invalid tick subscription request."));
                return {};
            }

            const auto request_for_failure = request;
            std::string error_message;
            auto control = add_pending_entry(
                provider,
                std::move(subscriber),
                stream_from(request),
                registered_provider_id,
                error_message);
            if (!control) {
                dispatch_result(
                    std::move(callback),
                    MarketDataSubscriptionResult::failed(
                        request_for_failure,
                        MarketDataSubscriptionStatus::FAILED,
                        std::move(error_message)));
                return {};
            }

            const auto router_id = control->router_id;
            const auto state = shared_from_this();
            bool accepted = false;
            try {
                accepted = provider.subscribe_ticks(
                    std::move(request),
                    [state, router_id, &provider, callback](
                            MarketDataSubscriptionResult result) mutable {
                        auto pending = std::make_shared<MarketDataSubscriptionResult>(
                            std::move(result));
                        const bool dispatched = state->dispatch_or_run(
                            [state,
                             router_id,
                             provider = &provider,
                             callback = std::move(callback),
                             pending]() mutable {
                                state->complete_subscribe(
                                    router_id,
                                    *provider,
                                    std::move(*pending),
                                    std::move(callback));
                            });
                        if (!dispatched) {
                            state->retain_rejected_subscribe_cleanup(
                                router_id,
                                provider,
                                *pending);
                        }
                    });
            } catch (const std::exception& exception) {
                fail_pending_subscribe(
                    router_id,
                    provider,
                    MarketDataSubscriptionResult::failed(
                        request_for_failure,
                        MarketDataSubscriptionStatus::FAILED,
                        std::string("Market-data provider tick subscription threw: ") +
                            exception.what()),
                    std::move(callback));
                return MarketDataRouterSubscription(std::move(control));
            } catch (...) {
                fail_pending_subscribe(
                    router_id,
                    provider,
                    MarketDataSubscriptionResult::failed(
                        request_for_failure,
                        MarketDataSubscriptionStatus::FAILED,
                        "Market-data provider tick subscription threw."),
                    std::move(callback));
                return MarketDataRouterSubscription(std::move(control));
            }

            if (!accepted) {
                fail_pending_subscribe(
                    router_id,
                    provider,
                    MarketDataSubscriptionResult::failed(
                        request_for_failure,
                        MarketDataSubscriptionStatus::FAILED,
                        "Market-data provider did not accept the tick subscription operation."),
                    std::move(callback));
            }
            return MarketDataRouterSubscription(std::move(control));
        }

        inline MarketDataRouterSubscription MarketDataRouterState::subscribe_ticks(
                MarketDataProviderId provider_id,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                TickSubscriptionRequest request,
                subscription_callback_t callback) {
            BaseMarketDataProvider* provider = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_shutdown) provider = registered_provider_no_lock(provider_id);
            }
            if (!provider) {
                dispatch_result(
                    std::move(callback),
                    MarketDataSubscriptionResult::failed(
                        std::move(request),
                        MarketDataSubscriptionStatus::FAILED,
                        "Market-data provider ID is not registered."));
                return {};
            }
            return subscribe_ticks(
                *provider,
                std::move(subscriber),
                std::move(request),
                std::move(callback),
                provider_id);
        }

        inline MarketDataRouterSubscription MarketDataRouterState::subscribe_ticks(
                std::string_view provider_alias,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                TickSubscriptionRequest request,
                subscription_callback_t callback) {
            MarketDataProviderId provider_id;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_shutdown) {
                    provider_id = provider_id_for_alias_no_lock(provider_alias);
                }
            }
            if (!provider_id.valid()) {
                dispatch_result(
                    std::move(callback),
                    MarketDataSubscriptionResult::failed(
                        std::move(request),
                        MarketDataSubscriptionStatus::FAILED,
                        "Market-data provider alias is not registered."));
                return {};
            }
            return subscribe_ticks(
                provider_id,
                std::move(subscriber),
                std::move(request),
                std::move(callback));
        }

        inline MarketDataRouterSubscription MarketDataRouterState::subscribe_bars(
                BaseMarketDataProvider& provider,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                BarSubscriptionRequest request,
                subscription_callback_t callback,
                MarketDataProviderId registered_provider_id) {
            if (!request.valid()) {
                dispatch_result(
                    std::move(callback),
                    MarketDataSubscriptionResult::failed(
                        std::move(request),
                        MarketDataSubscriptionStatus::INVALID_REQUEST,
                        "Invalid bar subscription request."));
                return {};
            }

            const auto request_for_failure = request;
            std::string error_message;
            auto control = add_pending_entry(
                provider,
                std::move(subscriber),
                stream_from(request),
                registered_provider_id,
                error_message);
            if (!control) {
                dispatch_result(
                    std::move(callback),
                    MarketDataSubscriptionResult::failed(
                        request_for_failure,
                        MarketDataSubscriptionStatus::FAILED,
                        std::move(error_message)));
                return {};
            }

            const auto router_id = control->router_id;
            const auto state = shared_from_this();
            bool accepted = false;
            try {
                accepted = provider.subscribe_bars(
                    std::move(request),
                    [state, router_id, &provider, callback](
                            MarketDataSubscriptionResult result) mutable {
                        auto pending = std::make_shared<MarketDataSubscriptionResult>(
                            std::move(result));
                        const bool dispatched = state->dispatch_or_run(
                            [state,
                             router_id,
                             provider = &provider,
                             callback = std::move(callback),
                             pending]() mutable {
                                state->complete_subscribe(
                                    router_id,
                                    *provider,
                                    std::move(*pending),
                                    std::move(callback));
                            });
                        if (!dispatched) {
                            state->retain_rejected_subscribe_cleanup(
                                router_id,
                                provider,
                                *pending);
                        }
                    });
            } catch (const std::exception& exception) {
                fail_pending_subscribe(
                    router_id,
                    provider,
                    MarketDataSubscriptionResult::failed(
                        request_for_failure,
                        MarketDataSubscriptionStatus::FAILED,
                        std::string("Market-data provider bar subscription threw: ") +
                            exception.what()),
                    std::move(callback));
                return MarketDataRouterSubscription(std::move(control));
            } catch (...) {
                fail_pending_subscribe(
                    router_id,
                    provider,
                    MarketDataSubscriptionResult::failed(
                        request_for_failure,
                        MarketDataSubscriptionStatus::FAILED,
                        "Market-data provider bar subscription threw."),
                    std::move(callback));
                return MarketDataRouterSubscription(std::move(control));
            }

            if (!accepted) {
                fail_pending_subscribe(
                    router_id,
                    provider,
                    MarketDataSubscriptionResult::failed(
                        request_for_failure,
                        MarketDataSubscriptionStatus::FAILED,
                        "Market-data provider did not accept the bar subscription operation."),
                    std::move(callback));
            }
            return MarketDataRouterSubscription(std::move(control));
        }

        inline MarketDataRouterSubscription MarketDataRouterState::subscribe_bars(
                MarketDataProviderId provider_id,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                BarSubscriptionRequest request,
                subscription_callback_t callback) {
            BaseMarketDataProvider* provider = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_shutdown) provider = registered_provider_no_lock(provider_id);
            }
            if (!provider) {
                dispatch_result(
                    std::move(callback),
                    MarketDataSubscriptionResult::failed(
                        std::move(request),
                        MarketDataSubscriptionStatus::FAILED,
                        "Market-data provider ID is not registered."));
                return {};
            }
            return subscribe_bars(
                *provider,
                std::move(subscriber),
                std::move(request),
                std::move(callback),
                provider_id);
        }

        inline MarketDataRouterSubscription MarketDataRouterState::subscribe_bars(
                std::string_view provider_alias,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                BarSubscriptionRequest request,
                subscription_callback_t callback) {
            MarketDataProviderId provider_id;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_shutdown) {
                    provider_id = provider_id_for_alias_no_lock(provider_alias);
                }
            }
            if (!provider_id.valid()) {
                dispatch_result(
                    std::move(callback),
                    MarketDataSubscriptionResult::failed(
                        std::move(request),
                        MarketDataSubscriptionStatus::FAILED,
                        "Market-data provider alias is not registered."));
                return {};
            }
            return subscribe_bars(
                provider_id,
                std::move(subscriber),
                std::move(request),
                std::move(callback));
        }

        inline void MarketDataRouterState::complete_subscribe(
                RoutedSubscriptionId router_id,
                BaseMarketDataProvider& provider,
                MarketDataSubscriptionResult result,
                subscription_callback_t callback) {
            std::shared_ptr<Entry> entry;
            std::shared_ptr<IMarketDataSubscriber> subscriber;
            MarketDataStatusUpdate replay;
            bool has_replay = false;
            bool release_requested = false;
            subscription_callback_t release_callback;
            BaseMarketDataProvider* unbind = nullptr;
            bool cleanup_orphan = false;

            if (result.success() &&
                (!result.subscription.valid() ||
                 result.subscription.provider_id != provider.provider_id())) {
                result = MarketDataSubscriptionResult::failed(
                    std::move(result.subscription),
                    MarketDataSubscriptionStatus::FAILED,
                    "Market-data provider returned an invalid subscription handle.");
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto entry_it = m_entries.find(router_id);
                if (entry_it == m_entries.end() || m_shutdown) {
                    cleanup_orphan = result.success() && result.subscription.valid();
                } else if (!result.success()) {
                    entry = entry_it->second;
                    set_control_released(entry->control);
                    release_callback = std::move(entry->release_callback);
                    unbind = remove_entry_no_lock(router_id);
                } else {
                    entry = entry_it->second;
                    entry->stream = stream_from(result.subscription);
                    entry->phase = EntryPhase::ACTIVE;
                    set_control_active(entry->control, result.subscription);
                    release_requested = entry->release_requested;
                    release_callback = std::move(entry->release_callback);

                    auto provider_it = m_providers.find(entry->provider_id);
                    if (provider_it != m_providers.end() && !release_requested) {
                        provider_it->second.provider_routes[result.subscription.id] = router_id;
                        has_replay = replay_status_no_lock(
                            provider_it->second,
                            entry->stream,
                            replay);
                        subscriber = entry->subscriber.lock();
                    }
                }
            }

            if (unbind) unbind_provider(*unbind);
            dispatch_result(callback, result);

            if (cleanup_orphan) {
                try {
                    provider.unsubscribe(std::move(result.subscription), {});
                } catch (...) {
                }
                return;
            }
            if (!entry) {
                if (release_callback) dispatch_result(std::move(release_callback), result);
                return;
            }
            if (release_requested && result.success()) {
                unsubscribe(entry->control, std::move(release_callback));
                return;
            }
            if (release_callback && !result.success()) {
                dispatch_result(std::move(release_callback), result);
            }
            if (has_replay && subscriber && result.success()) {
                bool still_active = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto current = m_entries.find(router_id);
                    still_active = current != m_entries.end() &&
                                   current->second == entry &&
                                   current->second->phase == EntryPhase::ACTIVE &&
                                   !current->second->release_requested;
                }
                if (!still_active) return;
                replay.subscription = result.subscription;
                subscriber->on_market_data_status(replay);
            }
        }

        inline void MarketDataRouterState::fail_pending_subscribe(
                RoutedSubscriptionId router_id,
                BaseMarketDataProvider& provider,
                MarketDataSubscriptionResult result,
                subscription_callback_t callback) {
            bool pending = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto it = m_entries.find(router_id);
                pending = it != m_entries.end() && it->second->phase == EntryPhase::PENDING;
            }
            if (pending) {
                complete_subscribe(
                    router_id,
                    provider,
                    std::move(result),
                    std::move(callback));
            }
        }

        inline bool MarketDataRouterState::unsubscribe(
                const std::shared_ptr<MarketDataRouterSubscriptionControl>& control,
                subscription_callback_t callback) {
            if (!control) return false;

            std::shared_ptr<Entry> entry;
            BaseMarketDataProvider* provider = nullptr;
            MarketDataSubscriptionHandle subscription;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto it = m_entries.find(control->router_id);
                if (it == m_entries.end() || it->second->control != control) {
                    set_control_released(control);
                    return false;
                }

                entry = it->second;
                set_control_released(control);
                entry->release_requested = true;
                if (entry->phase == EntryPhase::PENDING) {
                    entry->release_callback = std::move(callback);
                    return true;
                }
                if (entry->phase == EntryPhase::UNSUBSCRIBING) return false;

                entry->phase = EntryPhase::UNSUBSCRIBING;
                provider = entry->provider;
                subscription = control->provider_subscription;
                const auto provider_it = m_providers.find(entry->provider_id);
                if (provider_it != m_providers.end()) {
                    provider_it->second.provider_routes.erase(subscription.id);
                }
            }

            if (!provider || !subscription.valid()) {
                complete_unsubscribe(
                    entry->router_id,
                    subscription,
                    MarketDataSubscriptionResult::failed(
                        subscription,
                        MarketDataSubscriptionStatus::FAILED,
                        "Routed market-data subscription has no active provider handle."),
                    std::move(callback));
                return false;
            }

            const auto state = shared_from_this();
            bool accepted = false;
            try {
                accepted = provider->unsubscribe(
                    subscription,
                    [state,
                     router_id = entry->router_id,
                     subscription,
                     callback](
                            MarketDataSubscriptionResult result) mutable {
                        auto pending = std::make_shared<MarketDataSubscriptionResult>(
                            std::move(result));
                        state->dispatch_or_run(
                            [state,
                             router_id,
                             subscription,
                             callback = std::move(callback),
                             pending]() mutable {
                                state->complete_unsubscribe(
                                    router_id,
                                    subscription,
                                    std::move(*pending),
                                    std::move(callback));
                            });
                    });
            } catch (const std::exception& exception) {
                complete_unsubscribe(
                    entry->router_id,
                    subscription,
                    MarketDataSubscriptionResult::failed(
                        subscription,
                        MarketDataSubscriptionStatus::FAILED,
                        std::string("Market-data provider unsubscribe threw: ") +
                            exception.what()),
                    std::move(callback));
                return false;
            } catch (...) {
                complete_unsubscribe(
                    entry->router_id,
                    subscription,
                    MarketDataSubscriptionResult::failed(
                        subscription,
                        MarketDataSubscriptionStatus::FAILED,
                        "Market-data provider unsubscribe threw."),
                    std::move(callback));
                return false;
            }

            if (!accepted) {
                complete_unsubscribe(
                    entry->router_id,
                    subscription,
                    MarketDataSubscriptionResult::failed(
                        subscription,
                        MarketDataSubscriptionStatus::FAILED,
                        "Market-data provider did not accept the unsubscribe operation."),
                    std::move(callback));
            }
            return accepted;
        }

        inline void MarketDataRouterState::complete_unsubscribe(
                RoutedSubscriptionId router_id,
                MarketDataSubscriptionHandle expected_subscription,
                MarketDataSubscriptionResult result,
                subscription_callback_t callback) {
            if (!result.subscription.valid()) {
                result.subscription = expected_subscription;
            }

            BaseMarketDataProvider* unbind = nullptr;
            bool handled = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto it = m_entries.find(router_id);
                if (it != m_entries.end() &&
                    it->second->phase == EntryPhase::UNSUBSCRIBING) {
                    if (result.success()) {
                        set_control_released(it->second->control);
                        unbind = remove_entry_no_lock(router_id);
                    } else {
                        it->second->phase = EntryPhase::CLEANUP_FAILED;
                    }
                    handled = true;
                }
            }

            if (unbind) unbind_provider(*unbind);
            if (handled) dispatch_result(std::move(callback), std::move(result));
        }

        inline BaseMarketDataProvider* MarketDataRouterState::remove_entry_no_lock(
                RoutedSubscriptionId router_id) {
            const auto entry_it = m_entries.find(router_id);
            if (entry_it == m_entries.end()) return nullptr;

            const auto provider_id = entry_it->second->provider_id;
            const auto subscription = entry_it->second->control->provider_subscription;
            m_entries.erase(entry_it);

            const auto provider_it = m_providers.find(provider_id);
            if (provider_it == m_providers.end()) return nullptr;
            provider_it->second.provider_routes.erase(subscription.id);
            if (provider_it->second.route_count > 0) {
                --provider_it->second.route_count;
            }
            if (provider_it->second.route_count != 0) return nullptr;

            auto* provider = provider_it->second.provider;
            m_providers.erase(provider_it);
            return provider;
        }

        inline void MarketDataRouterState::cache_status_no_lock(
                ProviderSlot& slot,
                MarketDataStatusUpdate update) {
            update.subscription = {};
            for (auto& cached : slot.statuses) {
                if (same_status_stream(cached.update, update)) {
                    cached.update = std::move(update);
                    cached.sequence = slot.next_status_sequence++;
                    return;
                }
            }
            slot.statuses.push_back(CachedStatus{
                std::move(update),
                slot.next_status_sequence++});
        }

        inline bool MarketDataRouterState::replay_status_no_lock(
                const ProviderSlot& slot,
                const StreamDescriptor& stream,
                MarketDataStatusUpdate& update) const {
            const CachedStatus* latest = nullptr;
            for (const auto& cached : slot.statuses) {
                if (!status_matches_stream(cached.update, stream)) continue;
                if (!latest || cached.sequence > latest->sequence) {
                    latest = &cached;
                }
            }
            if (!latest) return false;
            update = latest->update;
            return true;
        }

        inline void MarketDataRouterState::route_ticks(
                ProviderInstanceId provider_id,
                std::unique_ptr<TickDataBatch> batch) {
            if (!batch) return;
            std::vector<std::pair<std::shared_ptr<IMarketDataSubscriber>, TickDataBatch>> deliveries;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto provider_it = m_providers.find(provider_id);
                if (provider_it == m_providers.end()) return;

                if (batch->subscription.valid()) {
                    if (batch->subscription.provider_id != provider_id) return;
                    const auto route_it = provider_it->second.provider_routes.find(
                        batch->subscription.id);
                    if (route_it == provider_it->second.provider_routes.end()) return;
                    const auto entry_it = m_entries.find(route_it->second);
                    if (entry_it == m_entries.end()) return;
                    const auto& entry = entry_it->second;
                    auto subscriber = entry->subscriber.lock();
                    if (!subscriber ||
                        entry->phase != EntryPhase::ACTIVE ||
                        !batch_matches_stream(*batch, entry->stream)) {
                        return;
                    }
                    auto routed = *batch;
                    routed.subscription = entry->control->provider_subscription;
                    deliveries.emplace_back(std::move(subscriber), std::move(routed));
                } else {
                    for (const auto& [id, entry] : m_entries) {
                        (void)id;
                        if (entry->provider_id != provider_id ||
                            entry->phase != EntryPhase::ACTIVE ||
                            !batch_matches_stream(*batch, entry->stream)) {
                            continue;
                        }
                        auto subscriber = entry->subscriber.lock();
                        if (!subscriber) continue;
                        auto routed = *batch;
                        routed.subscription = entry->control->provider_subscription;
                        deliveries.emplace_back(std::move(subscriber), std::move(routed));
                    }
                }
            }

            for (auto& delivery : deliveries) {
                delivery.first->on_tick_data(delivery.second);
            }
        }

        inline void MarketDataRouterState::route_bars(
                ProviderInstanceId provider_id,
                std::unique_ptr<BarDataBatch> batch) {
            if (!batch) return;
            std::vector<std::pair<std::shared_ptr<IMarketDataSubscriber>, BarDataBatch>> deliveries;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto provider_it = m_providers.find(provider_id);
                if (provider_it == m_providers.end()) return;

                if (batch->subscription.valid()) {
                    if (batch->subscription.provider_id != provider_id) return;
                    const auto route_it = provider_it->second.provider_routes.find(
                        batch->subscription.id);
                    if (route_it == provider_it->second.provider_routes.end()) return;
                    const auto entry_it = m_entries.find(route_it->second);
                    if (entry_it == m_entries.end()) return;
                    const auto& entry = entry_it->second;
                    auto subscriber = entry->subscriber.lock();
                    if (!subscriber ||
                        entry->phase != EntryPhase::ACTIVE ||
                        !batch_matches_stream(*batch, entry->stream)) {
                        return;
                    }
                    auto routed = *batch;
                    routed.subscription = entry->control->provider_subscription;
                    deliveries.emplace_back(std::move(subscriber), std::move(routed));
                } else {
                    for (const auto& [id, entry] : m_entries) {
                        (void)id;
                        if (entry->provider_id != provider_id ||
                            entry->phase != EntryPhase::ACTIVE ||
                            !batch_matches_stream(*batch, entry->stream)) {
                            continue;
                        }
                        auto subscriber = entry->subscriber.lock();
                        if (!subscriber) continue;
                        auto routed = *batch;
                        routed.subscription = entry->control->provider_subscription;
                        deliveries.emplace_back(std::move(subscriber), std::move(routed));
                    }
                }
            }

            for (auto& delivery : deliveries) {
                delivery.first->on_bar_data(delivery.second);
            }
        }

        inline void MarketDataRouterState::route_status(
                ProviderInstanceId provider_id,
                MarketDataStatusUpdate update) {
            std::vector<std::pair<std::shared_ptr<IMarketDataSubscriber>, MarketDataStatusUpdate>>
                deliveries;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto provider_it = m_providers.find(provider_id);
                if (provider_it == m_providers.end()) return;
                if (update.subscription.valid() &&
                    update.subscription.provider_id != provider_id) {
                    return;
                }

                update.provider_id = provider_id;
                if (update.subscription.valid()) {
                    update.type = update.subscription.stream_type;
                    update.symbol = update.subscription.symbol;
                    update.timeframe = update.subscription.timeframe;
                    if (update.transport == MarketDataTransport::AUTO) {
                        update.transport = update.subscription.transport;
                    }
                }
                cache_status_no_lock(provider_it->second, update);

                if (update.subscription.valid()) {
                    const auto route_it = provider_it->second.provider_routes.find(
                        update.subscription.id);
                    if (route_it != provider_it->second.provider_routes.end()) {
                        const auto entry_it = m_entries.find(route_it->second);
                        if (entry_it != m_entries.end() &&
                            entry_it->second->phase == EntryPhase::ACTIVE) {
                            auto subscriber = entry_it->second->subscriber.lock();
                            if (subscriber) {
                                update.subscription =
                                    entry_it->second->control->provider_subscription;
                                deliveries.emplace_back(
                                    std::move(subscriber),
                                    std::move(update));
                            }
                        }
                    }
                } else {
                    for (const auto& [id, entry] : m_entries) {
                        (void)id;
                        if (entry->provider_id != provider_id ||
                            entry->phase != EntryPhase::ACTIVE ||
                            !status_matches_stream(update, entry->stream)) {
                            continue;
                        }
                        auto subscriber = entry->subscriber.lock();
                        if (!subscriber) continue;
                        auto routed = update;
                        routed.subscription = entry->control->provider_subscription;
                        deliveries.emplace_back(std::move(subscriber), std::move(routed));
                    }
                }
            }

            for (auto& delivery : deliveries) {
                delivery.first->on_market_data_status(delivery.second);
            }
        }

        inline void MarketDataRouterState::set_control_active(
                const std::shared_ptr<MarketDataRouterSubscriptionControl>& control,
                const MarketDataSubscriptionHandle& subscription) {
            if (!control) return;
            std::lock_guard<std::mutex> lock(control->mutex);
            control->provider_subscription = subscription;
            control->active = true;
        }

        inline void MarketDataRouterState::set_control_released(
                const std::shared_ptr<MarketDataRouterSubscriptionControl>& control) {
            if (!control) return;
            std::lock_guard<std::mutex> lock(control->mutex);
            control->active = false;
            control->released = true;
        }

        inline void MarketDataRouterState::dispatch_result(
                subscription_callback_t callback,
                MarketDataSubscriptionResult result) {
            if (callback) callback(std::move(result));
        }

        inline std::size_t MarketDataRouterState::subscription_count() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_entries.size();
        }

        inline std::size_t MarketDataRouterState::failed_unsubscribe_count() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return static_cast<std::size_t>(std::count_if(
                m_entries.begin(),
                m_entries.end(),
                [](const auto& item) {
                    return item.second->phase == EntryPhase::CLEANUP_FAILED;
                }));
        }

        inline std::size_t MarketDataRouterState::retry_failed_unsubscribes() {
            std::vector<std::shared_ptr<MarketDataRouterSubscriptionControl>> controls;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                controls.reserve(m_entries.size());
                for (const auto& [id, entry] : m_entries) {
                    (void)id;
                    if (entry->phase == EntryPhase::CLEANUP_FAILED) {
                        controls.push_back(entry->control);
                    }
                }
            }

            std::size_t accepted = 0;
            for (const auto& control : controls) {
                if (unsubscribe(control, {})) ++accepted;
            }
            return accepted;
        }

        inline void MarketDataRouterState::shutdown() noexcept {
            std::vector<BaseMarketDataProvider*> providers;
            std::vector<std::pair<BaseMarketDataProvider*, MarketDataSubscriptionHandle>>
                subscriptions;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_shutdown) return;
                m_shutdown = true;

                providers.reserve(m_providers.size());
                for (const auto& [id, slot] : m_providers) {
                    (void)id;
                    if (slot.provider) providers.push_back(slot.provider);
                }
                for (const auto& [id, entry] : m_entries) {
                    (void)id;
                    set_control_released(entry->control);
                    const auto subscription = entry->control->provider_subscription;
                    if (entry->provider && subscription.valid()) {
                        subscriptions.emplace_back(entry->provider, subscription);
                    }
                    if (entry->provider &&
                        entry->retained_cleanup_subscription.valid()) {
                        subscriptions.emplace_back(
                            entry->provider,
                            std::move(entry->retained_cleanup_subscription));
                    }
                }
                m_entries.clear();
                m_providers.clear();
                m_registered_providers.clear();
                m_provider_aliases.clear();
                m_registered_provider_ids.clear();
            }

            for (auto* provider : providers) {
                if (provider) unbind_provider(*provider);
            }
            for (auto& [provider, subscription] : subscriptions) {
                if (!provider) continue;
                try {
                    provider->unsubscribe(std::move(subscription), {});
                } catch (...) {
                }
            }
        }

    } // namespace detail

    inline MarketDataRouterSubscription&
    MarketDataRouterSubscription::operator=(MarketDataRouterSubscription&& other) noexcept {
        if (this == &other) return *this;
        reset();
        m_control = std::move(other.m_control);
        return *this;
    }

    inline RoutedSubscriptionId MarketDataRouterSubscription::router_id() const noexcept {
        return m_control ? m_control->router_id : RoutedSubscriptionId{};
    }

    inline MarketDataSubscriptionHandle
    MarketDataRouterSubscription::provider_subscription() const {
        if (!m_control) return {};
        std::lock_guard<std::mutex> lock(m_control->mutex);
        return m_control->provider_subscription;
    }

    inline MarketDataProviderId
    MarketDataRouterSubscription::registered_provider_id() const {
        if (!m_control) return {};
        std::lock_guard<std::mutex> lock(m_control->mutex);
        return m_control->registered_provider_id;
    }

    inline bool MarketDataRouterSubscription::valid() const {
        if (!m_control) return false;
        std::lock_guard<std::mutex> lock(m_control->mutex);
        return !m_control->released &&
               m_control->router_id.valid();
    }

    inline bool MarketDataRouterSubscription::active() const {
        if (!m_control) return false;
        std::lock_guard<std::mutex> lock(m_control->mutex);
        return !m_control->released &&
               m_control->active &&
               m_control->provider_subscription.valid();
    }

    inline bool MarketDataRouterSubscription::unsubscribe(
            BaseMarketDataProvider::subscription_callback_t callback) {
        if (!m_control) return false;
        const auto control = std::move(m_control);
        const auto router = control->router.lock();
        if (!router) {
            detail::MarketDataRouterState::set_control_released(control);
            return false;
        }
        return router->unsubscribe(control, std::move(callback));
    }

    inline void MarketDataRouterSubscription::reset() noexcept {
        if (!m_control) return;
        const auto control = std::move(m_control);
        try {
            if (const auto router = control->router.lock()) {
                router->unsubscribe(control, {});
            } else {
                detail::MarketDataRouterState::set_control_released(control);
            }
        } catch (...) {
            detail::MarketDataRouterState::set_control_released(control);
        }
    }

    inline MarketDataRouter::MarketDataRouter()
            : m_state(std::make_shared<detail::MarketDataRouterState>()) {}

    inline MarketDataRouter::MarketDataRouter(owner_dispatcher_t owner_dispatcher)
            : m_state(std::make_shared<detail::MarketDataRouterState>(
                  std::move(owner_dispatcher))) {}

    inline MarketDataRouter::~MarketDataRouter() {
        shutdown();
    }

    inline bool MarketDataRouter::register_provider(
            MarketDataProviderId id,
            BaseMarketDataProvider& provider,
            std::vector<std::string> aliases) {
        return m_state && m_state->register_provider(
            id,
            provider,
            std::move(aliases));
    }

    inline bool MarketDataRouter::add_provider_alias(
            MarketDataProviderId id,
            std::string alias) {
        return m_state && m_state->add_provider_alias(id, std::move(alias));
    }

    inline bool MarketDataRouter::unregister_provider(MarketDataProviderId id) {
        return m_state && m_state->unregister_provider(id);
    }

    inline std::size_t MarketDataRouter::registered_provider_count() const {
        return m_state ? m_state->registered_provider_count() : 0;
    }

    inline MarketDataProviderId MarketDataRouter::registered_provider_id(
            ProviderInstanceId provider_id) const {
        return m_state ? m_state->registered_provider_id(provider_id) : MarketDataProviderId{};
    }

    inline std::vector<std::string> MarketDataRouter::provider_aliases(
            MarketDataProviderId id) const {
        return m_state ? m_state->provider_aliases(id) : std::vector<std::string>{};
    }

    inline bool MarketDataRouter::post_to_owner(owner_task_t task) const {
        return m_state && m_state->post_to_owner(std::move(task));
    }

    inline bool MarketDataRouter::has_owner_dispatcher() const noexcept {
        return m_state && m_state->has_owner_dispatcher();
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_ticks(
            BaseMarketDataProvider& provider,
            const std::shared_ptr<IMarketDataSubscriber>& subscriber,
            TickSubscriptionRequest request,
            subscription_callback_t callback) {
        return subscribe_ticks_weak(
            provider,
            std::weak_ptr<IMarketDataSubscriber>(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_ticks_weak(
            BaseMarketDataProvider& provider,
            std::weak_ptr<IMarketDataSubscriber> subscriber,
            TickSubscriptionRequest request,
            subscription_callback_t callback) {
        return m_state->subscribe_ticks(
            provider,
            std::move(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_ticks(
            MarketDataProviderId provider_id,
            const std::shared_ptr<IMarketDataSubscriber>& subscriber,
            TickSubscriptionRequest request,
            subscription_callback_t callback) {
        return subscribe_ticks_weak(
            provider_id,
            std::weak_ptr<IMarketDataSubscriber>(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_ticks_weak(
            MarketDataProviderId provider_id,
            std::weak_ptr<IMarketDataSubscriber> subscriber,
            TickSubscriptionRequest request,
            subscription_callback_t callback) {
        return m_state->subscribe_ticks(
            provider_id,
            std::move(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_ticks(
            std::string_view provider_alias,
            const std::shared_ptr<IMarketDataSubscriber>& subscriber,
            TickSubscriptionRequest request,
            subscription_callback_t callback) {
        return subscribe_ticks_weak(
            provider_alias,
            std::weak_ptr<IMarketDataSubscriber>(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_ticks_weak(
            std::string_view provider_alias,
            std::weak_ptr<IMarketDataSubscriber> subscriber,
            TickSubscriptionRequest request,
            subscription_callback_t callback) {
        return m_state->subscribe_ticks(
            provider_alias,
            std::move(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_bars(
            BaseMarketDataProvider& provider,
            const std::shared_ptr<IMarketDataSubscriber>& subscriber,
            BarSubscriptionRequest request,
            subscription_callback_t callback) {
        return subscribe_bars_weak(
            provider,
            std::weak_ptr<IMarketDataSubscriber>(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_bars_weak(
            BaseMarketDataProvider& provider,
            std::weak_ptr<IMarketDataSubscriber> subscriber,
            BarSubscriptionRequest request,
            subscription_callback_t callback) {
        return m_state->subscribe_bars(
            provider,
            std::move(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_bars(
            MarketDataProviderId provider_id,
            const std::shared_ptr<IMarketDataSubscriber>& subscriber,
            BarSubscriptionRequest request,
            subscription_callback_t callback) {
        return subscribe_bars_weak(
            provider_id,
            std::weak_ptr<IMarketDataSubscriber>(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_bars_weak(
            MarketDataProviderId provider_id,
            std::weak_ptr<IMarketDataSubscriber> subscriber,
            BarSubscriptionRequest request,
            subscription_callback_t callback) {
        return m_state->subscribe_bars(
            provider_id,
            std::move(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_bars(
            std::string_view provider_alias,
            const std::shared_ptr<IMarketDataSubscriber>& subscriber,
            BarSubscriptionRequest request,
            subscription_callback_t callback) {
        return subscribe_bars_weak(
            provider_alias,
            std::weak_ptr<IMarketDataSubscriber>(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline MarketDataRouter::SubscriptionHandle MarketDataRouter::subscribe_bars_weak(
            std::string_view provider_alias,
            std::weak_ptr<IMarketDataSubscriber> subscriber,
            BarSubscriptionRequest request,
            subscription_callback_t callback) {
        return m_state->subscribe_bars(
            provider_alias,
            std::move(subscriber),
            std::move(request),
            std::move(callback));
    }

    inline std::size_t MarketDataRouter::subscription_count() const {
        return m_state ? m_state->subscription_count() : 0;
    }

    inline std::size_t MarketDataRouter::failed_unsubscribe_count() const {
        return m_state ? m_state->failed_unsubscribe_count() : 0;
    }

    inline std::size_t MarketDataRouter::retry_failed_unsubscribes() {
        return m_state ? m_state->retry_failed_unsubscribes() : 0;
    }

    inline void MarketDataRouter::shutdown() noexcept {
        if (m_state) m_state->shutdown();
    }

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_HPP_INCLUDED
