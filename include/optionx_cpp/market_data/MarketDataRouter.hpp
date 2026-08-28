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

    namespace detail {

        struct MarketDataRouterSubscriptionControl {
            mutable std::mutex mutex;
            RoutedSubscriptionId router_id;
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
    ///          under the router mutex. Deterministic replay/live ordering still
    ///          requires subscribe and provider publish calls to use one owner
    ///          loop, such as platform process().
    class MarketDataRouter {
    public:
        using SubscriptionHandle = MarketDataRouterSubscription; ///< Move-only route owner.
        using subscription_callback_t = BaseMarketDataProvider::subscription_callback_t; ///< Operation callback.

        /// \brief Constructs an empty router.
        MarketDataRouter();

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

        /// \brief Returns the number of pending and active logical routes.
        [[nodiscard]] std::size_t subscription_count() const;

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
                UNSUBSCRIBING
            };

            struct Entry {
                RoutedSubscriptionId router_id;
                ProviderInstanceId provider_id = kInvalidProviderInstanceId;
                BaseMarketDataProvider* provider = nullptr;
                std::weak_ptr<IMarketDataSubscriber> subscriber;
                std::shared_ptr<MarketDataRouterSubscriptionControl> control;
                StreamDescriptor stream;
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

            MarketDataRouterSubscription subscribe_ticks(
                    BaseMarketDataProvider& provider,
                    std::weak_ptr<IMarketDataSubscriber> subscriber,
                    TickSubscriptionRequest request,
                    subscription_callback_t callback);

            MarketDataRouterSubscription subscribe_bars(
                    BaseMarketDataProvider& provider,
                    std::weak_ptr<IMarketDataSubscriber> subscriber,
                    BarSubscriptionRequest request,
                    subscription_callback_t callback);

            bool unsubscribe(
                    const std::shared_ptr<MarketDataRouterSubscriptionControl>& control,
                    subscription_callback_t callback);

            [[nodiscard]] std::size_t subscription_count() const;
            void shutdown() noexcept;

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
            std::uint64_t m_next_router_id = 1;
            bool m_shutdown = false;

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
                    std::string& error_message);

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

            friend class ::optionx::market_data::MarketDataRouterSubscription;
        };

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
                        state->route_ticks(provider_id, std::move(batch));
                    }
                };
            provider.on_bar_data() =
                [weak_state, provider_id](std::unique_ptr<BarDataBatch> batch) {
                    if (const auto state = weak_state.lock()) {
                        state->route_bars(provider_id, std::move(batch));
                    }
                };
            provider.on_market_data_status() =
                [weak_state, provider_id](MarketDataStatusUpdate update) {
                    if (const auto state = weak_state.lock()) {
                        state->route_status(provider_id, std::move(update));
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

                auto router_id_value = m_next_router_id++;
                if (router_id_value == 0) {
                    router_id_value = m_next_router_id++;
                }
                const RoutedSubscriptionId router_id(router_id_value);

                control->router_id = router_id;
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
                subscription_callback_t callback) {
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
                        state->complete_subscribe(
                            router_id,
                            provider,
                            std::move(result),
                            std::move(callback));
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

        inline MarketDataRouterSubscription MarketDataRouterState::subscribe_bars(
                BaseMarketDataProvider& provider,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                BarSubscriptionRequest request,
                subscription_callback_t callback) {
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
                        state->complete_subscribe(
                            router_id,
                            provider,
                            std::move(result),
                            std::move(callback));
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
                        state->complete_unsubscribe(
                            router_id,
                            subscription,
                            std::move(result),
                            std::move(callback));
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
            BaseMarketDataProvider* unbind = nullptr;
            bool removed = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto it = m_entries.find(router_id);
                if (it != m_entries.end() &&
                    it->second->phase == EntryPhase::UNSUBSCRIBING) {
                    set_control_released(it->second->control);
                    unbind = remove_entry_no_lock(router_id);
                    removed = true;
                }
            }

            if (!result.subscription.valid()) {
                result.subscription = std::move(expected_subscription);
            }
            if (unbind) unbind_provider(*unbind);
            if (removed) dispatch_result(std::move(callback), std::move(result));
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
                }
                m_entries.clear();
                m_providers.clear();
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

    inline MarketDataRouter::~MarketDataRouter() {
        shutdown();
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

    inline std::size_t MarketDataRouter::subscription_count() const {
        return m_state ? m_state->subscription_count() : 0;
    }

    inline void MarketDataRouter::shutdown() noexcept {
        if (m_state) m_state->shutdown();
    }

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_HPP_INCLUDED
