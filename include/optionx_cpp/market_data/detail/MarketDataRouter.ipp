#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_DETAIL_MARKET_DATA_ROUTER_IPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_DETAIL_MARKET_DATA_ROUTER_IPP_INCLUDED

/// \file MarketDataRouter.ipp
/// \brief Implements subscription-scoped market-data routing utilities.

namespace optionx::market_data {

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

            enum class ContinuityRequestKind {
                PREFILL,
                GAP_BACKFILL,
                RECONNECT_BACKFILL
            };

            enum class ContinuityPhase {
                LIVE,
                PREFILLING,
                WAITING_FOR_READY,
                RECOVERING,
                FLUSHING,
                DEGRADED
            };

            struct PendingContinuityRequest;

            struct ContinuityState {
                ContinuityPhase phase = ContinuityPhase::LIVE;
                bool initial_prefill_pending = false;
                bool request_in_flight = false;
                std::deque<BarDataBatch> buffer;
                std::size_t buffered_items = 0;
                std::uint64_t last_observed_time_ms = 0;
                std::uint64_t verified_through_time_ms = 0;
                std::uint64_t unverified_from_time_ms = 0;
                std::uint64_t initial_prefill_boundary_time_ms = 0;
                std::uint64_t generation = 0;
                std::uint64_t reconnect_target_time_ms = 0;
                std::optional<PendingContinuityRequest> retry_request;
                std::chrono::steady_clock::time_point retry_at;

                [[nodiscard]] bool buffers_live_data() const noexcept {
                    return phase == ContinuityPhase::PREFILLING ||
                        phase == ContinuityPhase::WAITING_FOR_READY ||
                        phase == ContinuityPhase::RECOVERING ||
                        phase == ContinuityPhase::FLUSHING;
                }
            };

            struct Entry {
                RoutedSubscriptionId router_id;
                ProviderInstanceId provider_id = kInvalidProviderInstanceId;
                BaseMarketDataProvider* provider = nullptr;
                std::weak_ptr<IMarketDataSubscriber> subscriber;
                std::shared_ptr<MarketDataRouterSubscriptionControl> control;
                StreamDescriptor stream;
                MarketDataContinuityOptions continuity;
                ContinuityState continuity_state;
                MarketDataSubscriptionHandle retained_cleanup_subscription;
                MarketDataSubscriptionResult unsubscribe_completion;
                bool subscribe_completion_posted = false;
                bool subscribe_completion_received = false;
                bool unsubscribe_completion_received = false;
                EntryPhase phase = EntryPhase::PENDING;
                bool release_requested = false;
                subscription_callback_t release_callback;
            };

            struct PendingContinuityRequest {
                RoutedSubscriptionId router_id;
                MarketDataSubscriptionHandle subscription;
                BarHistoryRequest request;
                ContinuityRequestKind kind = ContinuityRequestKind::PREFILL;
                bool announce_gap = false;
                std::uint64_t from_time_ms = 0;
                std::uint64_t to_time_ms = 0;
                std::size_t requested_items = 0;
                std::size_t attempt = 1;
            };

            struct ContinuityOperation {
                bool completed = false;
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

            template <typename Request, typename SubscribeOperation>
            MarketDataRouterSubscription subscribe_impl(
                    BaseMarketDataProvider& provider,
                    std::weak_ptr<IMarketDataSubscriber> subscriber,
                    Request request,
                    subscription_callback_t callback,
                    MarketDataProviderId registered_provider_id,
                    SubscribeOperation subscribe_operation,
                    const char* invalid_request_message,
                    const char* operation_name,
                    const char* not_accepted_message);

            bool unsubscribe(
                    const std::shared_ptr<MarketDataRouterSubscriptionControl>& control,
                    subscription_callback_t callback);

            [[nodiscard]] std::size_t subscription_count() const;
            [[nodiscard]] std::size_t failed_unsubscribe_count() const;
            std::size_t retry_failed_unsubscribes();
            void process();
            [[nodiscard]] bool is_shutdown_complete() const noexcept;
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

            template <typename Batch, typename MatchesStream, typename Deliver>
            void route_batch(
                    ProviderInstanceId provider_id,
                    std::unique_ptr<Batch> batch,
                    MatchesStream matches_stream,
                    Deliver deliver);

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
            bool m_shutdown_complete = false;
            std::size_t m_continuity_operations_in_flight = 0;
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
                    MarketDataContinuityOptions continuity,
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

            static MarketDataContinuityOptions continuity_from(
                    const TickSubscriptionRequest&) noexcept {
                return {};
            }

            static MarketDataContinuityOptions continuity_from(
                    const BarSubscriptionRequest& request) noexcept {
                return request.continuity;
            }

            void start_continuity(
                    RoutedSubscriptionId router_id,
                    MarketDataSubscriptionHandle subscription);
            void start_reconnect_recovery(RoutedSubscriptionId router_id);
            void request_continuity_history(
                    RoutedSubscriptionId router_id,
                    MarketDataSubscriptionHandle subscription,
                    BarHistoryRequest request,
                    ContinuityRequestKind kind,
                    bool announce_gap,
                    std::uint64_t from_time_ms,
                    std::uint64_t to_time_ms,
                    std::size_t requested_items,
                    std::size_t attempt,
                    std::uint64_t generation = 0);
            void complete_continuity(
                    RoutedSubscriptionId router_id,
                    MarketDataSubscriptionHandle subscription,
                    BarHistoryRequest request,
                    ContinuityRequestKind kind,
                    std::uint64_t from_time_ms,
                    std::uint64_t to_time_ms,
                    std::size_t requested_items,
                    std::size_t attempt,
                    std::uint64_t generation,
                    BarHistoryResult result);
            void notify_continuity(
                    RoutedSubscriptionId router_id,
                    MarketDataContinuityUpdate update);
            static MarketDataContinuityUpdate make_continuity_update(
                    const MarketDataSubscriptionHandle& subscription,
                    MarketDataContinuityStatus status,
                    std::uint64_t from_time_ms,
                    std::uint64_t to_time_ms,
                    std::size_t requested_items,
                    std::size_t delivered_items,
                    std::string message);
            bool route_bar_to_entry_no_lock(
                    const std::shared_ptr<Entry>& entry,
                    const BarDataBatch& batch,
                    std::vector<std::pair<
                        std::shared_ptr<IMarketDataSubscriber>,
                        BarDataBatch>>& deliveries,
                    std::vector<PendingContinuityRequest>& continuity_requests,
                    std::vector<std::pair<
                        std::shared_ptr<IMarketDataSubscriber>,
                        MarketDataContinuityUpdate>>& continuity_deliveries,
                    bool process_buffered = false,
                    bool allow_gap_recovery = true,
                    std::uint64_t confirmed_through_time_ms = 0);
            static bool buffer_continuity_batch_no_lock(
                    const std::shared_ptr<Entry>& entry,
                    std::shared_ptr<IMarketDataSubscriber> subscriber,
                    BarDataBatch batch,
                    std::vector<std::pair<
                        std::shared_ptr<IMarketDataSubscriber>,
                        BarDataBatch>>& deliveries,
                    std::vector<std::pair<
                        std::shared_ptr<IMarketDataSubscriber>,
                        MarketDataContinuityUpdate>>& continuity_deliveries,
                    bool push_front);
            static bool history_covers_range(
                    const BarDataBatch& batch,
                    std::uint64_t from_time_ms,
                    std::uint64_t to_time_ms,
                    std::uint64_t timeframe_ms) noexcept;
            static void clip_history_to_range(
                    BarDataBatch& batch,
                    std::uint64_t from_time_ms,
                    std::uint64_t to_time_ms);
            static void mark_unverified_no_lock(
                    const std::shared_ptr<Entry>& entry,
                    std::uint64_t from_time_ms) noexcept;
            static void record_verified_range_no_lock(
                    const std::shared_ptr<Entry>& entry,
                    std::uint64_t from_time_ms,
                    std::uint64_t to_time_ms,
                    std::uint64_t timeframe_ms,
                    bool has_more_reconnect_history) noexcept;
            static void record_bar_progress_no_lock(
                    const std::shared_ptr<Entry>& entry,
                    const std::vector<Bar>& bars) noexcept;
            static std::chrono::steady_clock::duration continuity_retry_delay(
                    const MarketDataContinuityRetryPolicy& policy,
                    std::size_t attempt) noexcept;
            static void apply_stream_status_to_entry_no_lock(
                    const std::shared_ptr<Entry>& entry,
                    const MarketDataStatusUpdate& update,
                    std::vector<std::pair<
                        std::shared_ptr<IMarketDataSubscriber>,
                        MarketDataContinuityUpdate>>& continuity_deliveries,
                     std::vector<RoutedSubscriptionId>& prefill_routes,
                     std::vector<RoutedSubscriptionId>& reconnect_routes);

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

            bool start_unsubscribe(
                    const std::shared_ptr<Entry>& entry,
                    MarketDataSubscriptionHandle subscription,
                    subscription_callback_t callback);
            bool record_unsubscribe_completion(
                    RoutedSubscriptionId router_id,
                    const MarketDataSubscriptionHandle& expected_subscription,
                    const MarketDataSubscriptionResult& result,
                    bool& shutdown_requested);
            void dispatch_unsubscribe_completion(
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
            bool record_subscribe_completion(
                    RoutedSubscriptionId router_id,
                    BaseMarketDataProvider& provider,
                    const MarketDataSubscriptionResult& result,
                    bool& shutdown_requested);
            bool record_continuity_completion(
                    const std::shared_ptr<ContinuityOperation>& operation);
            void mark_subscribe_completion_posted(
                    RoutedSubscriptionId router_id,
                    const MarketDataSubscriptionHandle& subscription);
            void dispatch_subscribe_completion(
                    RoutedSubscriptionId router_id,
                    BaseMarketDataProvider& provider,
                    MarketDataSubscriptionResult result,
                    subscription_callback_t callback);
            void request_process();

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

        inline bool MarketDataRouterState::record_subscribe_completion(
                RoutedSubscriptionId router_id,
                BaseMarketDataProvider& provider,
                const MarketDataSubscriptionResult& result,
                bool& shutdown_requested) {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto entry_it = m_entries.find(router_id);
            if (entry_it == m_entries.end() ||
                entry_it->second->provider != &provider ||
                entry_it->second->phase != EntryPhase::PENDING ||
                entry_it->second->subscribe_completion_received) {
                return false;
            }

            entry_it->second->subscribe_completion_received = true;
            if (result.success()) {
                entry_it->second->retained_cleanup_subscription =
                    result.subscription;
            }
            entry_it->second->subscribe_completion_posted = false;
            shutdown_requested = m_shutdown;
            return true;
        }

        inline void MarketDataRouterState::mark_subscribe_completion_posted(
                RoutedSubscriptionId router_id,
                const MarketDataSubscriptionHandle& subscription) {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto entry_it = m_entries.find(router_id);
            if (entry_it == m_entries.end() ||
                entry_it->second->phase != EntryPhase::PENDING) {
                return;
            }

            const auto& reservation =
                entry_it->second->retained_cleanup_subscription;
            if (reservation.valid() &&
                reservation.provider_id == subscription.provider_id &&
                reservation.id == subscription.id) {
                entry_it->second->subscribe_completion_posted = true;
            }
        }

        inline void MarketDataRouterState::dispatch_subscribe_completion(
                RoutedSubscriptionId router_id,
                BaseMarketDataProvider& provider,
                MarketDataSubscriptionResult result,
                subscription_callback_t callback) {
            if (result.success() &&
                (!result.subscription.valid() ||
                 result.subscription.provider_id != provider.provider_id())) {
                result = MarketDataSubscriptionResult::failed(
                    std::move(result.subscription),
                    MarketDataSubscriptionStatus::FAILED,
                    "Market-data provider returned an invalid subscription handle.");
            }

            bool shutdown_requested = false;
            if (!record_subscribe_completion(
                    router_id,
                    provider,
                    result,
                    shutdown_requested)) {
                return;
            }

            if (shutdown_requested) {
                request_process();
                return;
            }

            const auto subscription = result.subscription;
            auto pending = std::make_shared<MarketDataSubscriptionResult>(
                std::move(result));
            const auto state = shared_from_this();
            const bool posted = dispatch_or_run(
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
            if (posted && subscription.valid()) {
                mark_subscribe_completion_posted(router_id, subscription);
            }
        }

        inline void MarketDataRouterState::request_process() {
            if (!m_owner_dispatcher) return;
            const auto state = shared_from_this();
            post_to_owner([state]() {
                state->process();
            });
        }

        inline bool MarketDataRouterState::record_continuity_completion(
                const std::shared_ptr<ContinuityOperation>& operation) {
            if (!operation) return false;

            std::lock_guard<std::mutex> lock(m_mutex);
            if (operation->completed) return false;
            operation->completed = true;
            if (m_continuity_operations_in_flight > 0) {
                --m_continuity_operations_in_flight;
            }
            return true;
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
                MarketDataContinuityOptions continuity,
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
                         (existing_entry->retained_cleanup_subscription.valid() &&
                          !existing_entry->subscribe_completion_posted))) {
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
                entry->continuity = std::move(continuity);
                entry->continuity_state.initial_prefill_pending =
                    entry->continuity.enabled() && entry->continuity.prefill_bars > 0;
                entry->continuity_state.phase =
                    entry->continuity_state.initial_prefill_pending
                        ? ContinuityPhase::PREFILLING
                        : ContinuityPhase::LIVE;
                m_entries.emplace(router_id, entry);
                ++provider_it->second.route_count;
            }
            return control;
        }

        template <typename Request, typename SubscribeOperation>
        inline MarketDataRouterSubscription
        MarketDataRouterState::subscribe_impl(
                BaseMarketDataProvider& provider,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                Request request,
                subscription_callback_t callback,
                MarketDataProviderId registered_provider_id,
                SubscribeOperation subscribe_operation,
                const char* invalid_request_message,
                const char* operation_name,
                const char* not_accepted_message) {
            if (!request.valid()) {
                dispatch_result(
                    std::move(callback),
                    MarketDataSubscriptionResult::failed(
                        std::move(request),
                        MarketDataSubscriptionStatus::INVALID_REQUEST,
                        invalid_request_message));
                return {};
            }

            const auto request_for_failure = request;
            std::string error_message;
            auto control = add_pending_entry(
                provider,
                std::move(subscriber),
                stream_from(request),
                continuity_from(request),
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
                accepted = subscribe_operation(
                    provider,
                    std::move(request),
                    [state, router_id, &provider, callback](
                            MarketDataSubscriptionResult result) mutable {
                        state->dispatch_subscribe_completion(
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
                        std::string(operation_name) + " threw: " + exception.what()),
                    std::move(callback));
                return MarketDataRouterSubscription(std::move(control));
            } catch (...) {
                fail_pending_subscribe(
                    router_id,
                    provider,
                    MarketDataSubscriptionResult::failed(
                        request_for_failure,
                        MarketDataSubscriptionStatus::FAILED,
                        std::string(operation_name) + " threw."),
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
                        not_accepted_message),
                    std::move(callback));
            }
            return MarketDataRouterSubscription(std::move(control));
        }

        inline MarketDataRouterSubscription MarketDataRouterState::subscribe_ticks(
                BaseMarketDataProvider& provider,
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                TickSubscriptionRequest request,
                subscription_callback_t callback,
                MarketDataProviderId registered_provider_id) {
            return subscribe_impl(
                provider,
                std::move(subscriber),
                std::move(request),
                std::move(callback),
                registered_provider_id,
                [](BaseMarketDataProvider& provider,
                   TickSubscriptionRequest request,
                   subscription_callback_t operation_callback) {
                    return provider.subscribe_ticks(
                        std::move(request),
                        std::move(operation_callback));
                },
                "Invalid tick subscription request.",
                "Market-data provider tick subscription",
                "Market-data provider did not accept the tick subscription operation.");
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
            return subscribe_impl(
                provider,
                std::move(subscriber),
                std::move(request),
                std::move(callback),
                registered_provider_id,
                [](BaseMarketDataProvider& provider,
                   BarSubscriptionRequest request,
                   subscription_callback_t operation_callback) {
                    return provider.subscribe_bars(
                        std::move(request),
                        std::move(operation_callback));
                },
                "Invalid bar subscription request.",
                "Market-data provider bar subscription",
                "Market-data provider did not accept the bar subscription operation.");
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
            bool needs_continuity_prefill = false;
            subscription_callback_t release_callback;
            BaseMarketDataProvider* unbind = nullptr;
            std::vector<std::pair<
                std::shared_ptr<IMarketDataSubscriber>,
                MarketDataContinuityUpdate>> replay_continuity_deliveries;
            std::vector<RoutedSubscriptionId> replay_prefill_routes;
            std::vector<RoutedSubscriptionId> replay_reconnect_routes;

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
                if (entry_it == m_entries.end() ||
                    m_shutdown ||
                    !entry_it->second->subscribe_completion_received) {
                    return;
                }

                if (!result.success()) {
                    entry = entry_it->second;
                    set_control_released(entry->control);
                    release_callback = std::move(entry->release_callback);
                    unbind = remove_entry_no_lock(router_id);
                } else {
                    entry = entry_it->second;
                    const auto& reservation =
                        entry->retained_cleanup_subscription;
                    if (!reservation.valid() ||
                        reservation.provider_id != result.subscription.provider_id ||
                        reservation.id != result.subscription.id) {
                        return;
                    }

                    entry->retained_cleanup_subscription = {};
                    entry->subscribe_completion_posted = false;
                    entry->subscribe_completion_received = false;
                    entry->stream = stream_from(result.subscription);
                    entry->phase = EntryPhase::ACTIVE;
                    set_control_active(entry->control, result.subscription);
                    needs_continuity_prefill =
                        entry->stream.type == MarketDataType::BARS &&
                        entry->continuity.prefill_bars > 0;
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
                        if (has_replay) {
                            apply_stream_status_to_entry_no_lock(
                                entry,
                                replay,
                                replay_continuity_deliveries,
                                replay_prefill_routes,
                                replay_reconnect_routes);
                        }
                    }
                }
            }

            if (unbind) unbind_provider(*unbind);
            dispatch_result(callback, result);

            if (release_requested && result.success()) {
                unsubscribe(entry->control, std::move(release_callback));
                return;
            }
            if (release_callback && !result.success()) {
                dispatch_result(std::move(release_callback), result);
            }
            if (needs_continuity_prefill && result.success()) {
                start_continuity(router_id, result.subscription);
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
            for (auto& delivery : replay_continuity_deliveries) {
                delivery.first->on_market_data_continuity(delivery.second);
            }
            for (const auto replay_router_id : replay_prefill_routes) {
                start_continuity(replay_router_id, result.subscription);
            }
            for (const auto replay_router_id : replay_reconnect_routes) {
                start_reconnect_recovery(replay_router_id);
            }
        }

        inline void MarketDataRouterState::start_continuity(
                RoutedSubscriptionId router_id,
                MarketDataSubscriptionHandle subscription) {
            PendingContinuityRequest pending;
            std::shared_ptr<IMarketDataSubscriber> subscriber;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto entry_it = m_entries.find(router_id);
                if (entry_it == m_entries.end() ||
                    entry_it->second->phase != EntryPhase::ACTIVE ||
                    !entry_it->second->continuity_state.initial_prefill_pending ||
                    entry_it->second->continuity_state.phase !=
                        ContinuityPhase::PREFILLING ||
                    entry_it->second->continuity.prefill_bars == 0 ||
                    entry_it->second->continuity_state.request_in_flight) {
                    return;
                }

                const auto& entry = entry_it->second;
                BarSubscriptionRequest bar_request(
                    entry->stream.symbol,
                    entry->stream.timeframe,
                    entry->stream.price_source,
                    entry->stream.transport);
                bar_request.continuity = entry->continuity;
                const auto now_ms =
                    entry->continuity_state.initial_prefill_boundary_time_ms != 0
                    ? entry->continuity_state.initial_prefill_boundary_time_ms
                    : static_cast<std::uint64_t>(OPTIONX_TIMESTAMP_MS);
                pending.router_id = router_id;
                pending.subscription = subscription;
                pending.request = MarketDataContinuityService::make_prefill_request(
                    bar_request,
                    now_ms,
                    entry->continuity.prefill_bars);
                pending.kind = ContinuityRequestKind::PREFILL;
                pending.announce_gap = false;
                pending.from_time_ms =
                    MarketDataContinuityService::seconds_to_milliseconds(
                        pending.request.from_ts);
                pending.to_time_ms =
                    MarketDataContinuityService::seconds_to_milliseconds(
                        pending.request.to_ts);
                if (entry->continuity_state.initial_prefill_boundary_time_ms == 0) {
                    entry->continuity_state.initial_prefill_boundary_time_ms =
                        pending.to_time_ms;
                }
                pending.requested_items = entry->continuity.prefill_bars;
                entry->continuity_state.phase = ContinuityPhase::PREFILLING;
                entry->continuity_state.request_in_flight = true;
                subscriber = entry->subscriber.lock();
            }

            if (subscriber) {
                subscriber->on_market_data_continuity(make_continuity_update(
                    subscription,
                    MarketDataContinuityStatus::PREFILLING,
                    pending.from_time_ms,
                    pending.to_time_ms,
                    pending.requested_items,
                    0,
                    "Requesting historical bar prefill."));
            }
            request_continuity_history(
                pending.router_id,
                std::move(pending.subscription),
                std::move(pending.request),
                pending.kind,
                pending.announce_gap,
                pending.from_time_ms,
                pending.to_time_ms,
                pending.requested_items,
                1);
        }

        inline void MarketDataRouterState::start_reconnect_recovery(
                RoutedSubscriptionId router_id) {
            PendingContinuityRequest pending;
            std::shared_ptr<IMarketDataSubscriber> subscriber;
            MarketDataSubscriptionHandle live_subscription;
            bool notify_live = false;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto entry_it = m_entries.find(router_id);
                if (entry_it == m_entries.end() ||
                    entry_it->second->phase != EntryPhase::ACTIVE) {
                    return;
                }

                const auto& entry = entry_it->second;
                auto& continuity = entry->continuity_state;
                if (continuity.phase != ContinuityPhase::RECOVERING ||
                    !entry->continuity.recovers_gaps() ||
                    continuity.initial_prefill_pending ||
                    continuity.request_in_flight) {
                    return;
                }

                const auto timeframe_ms = static_cast<std::uint64_t>(
                    entry->stream.timeframe) * 1000U;
                if (timeframe_ms == 0) return;

                std::uint64_t earliest_buffered_time_ms = 0;
                std::uint64_t latest_buffered_time_ms = 0;
                for (const auto& batch : continuity.buffer) {
                    for (const auto& bar : batch.items) {
                        if (bar.time_ms == 0) continue;
                        if (earliest_buffered_time_ms == 0 ||
                            bar.time_ms < earliest_buffered_time_ms) {
                            earliest_buffered_time_ms = bar.time_ms;
                        }
                        latest_buffered_time_ms = std::max(
                            latest_buffered_time_ms,
                            bar.time_ms);
                    }
                }

                const auto observed_time_ms = std::max(
                    continuity.last_observed_time_ms,
                    latest_buffered_time_ms);
                const bool has_untrusted_data =
                    !continuity.buffer.empty() ||
                    observed_time_ms > continuity.verified_through_time_ms;

                const auto now_ms = static_cast<std::uint64_t>(
                    OPTIONX_TIMESTAMP_MS);
                const auto aligned_now_ms = now_ms - (now_ms % timeframe_ms);
                const auto last_closed_time_ms = aligned_now_ms > timeframe_ms
                    ? aligned_now_ms - timeframe_ms
                    : 0;

                auto from_time_ms = continuity.unverified_from_time_ms;
                if (from_time_ms == 0 && has_untrusted_data) {
                    if (continuity.verified_through_time_ms > 0 &&
                        continuity.verified_through_time_ms <=
                            std::numeric_limits<std::uint64_t>::max() - timeframe_ms) {
                        from_time_ms = continuity.verified_through_time_ms + timeframe_ms;
                    } else {
                        from_time_ms = earliest_buffered_time_ms != 0
                            ? earliest_buffered_time_ms
                            : observed_time_ms;
                    }
                } else if (from_time_ms == 0 &&
                           observed_time_ms > 0 &&
                           observed_time_ms <=
                               std::numeric_limits<std::uint64_t>::max() - timeframe_ms) {
                    from_time_ms = observed_time_ms + timeframe_ms;
                }

                if (from_time_ms == 0) {
                    continuity.phase = ContinuityPhase::LIVE;
                    continuity.reconnect_target_time_ms = 0;
                    live_subscription = entry->control->provider_subscription;
                    subscriber = entry->subscriber.lock();
                    notify_live = true;
                } else if (from_time_ms > last_closed_time_ms) {
                    // A dirty current candle cannot be verified until it closes.
                    return;
                } else {
                    BarSubscriptionRequest bar_request(
                        entry->stream.symbol,
                        entry->stream.timeframe,
                        entry->stream.price_source,
                        entry->stream.transport);
                    bar_request.continuity = entry->continuity;

                    const auto history_request =
                        MarketDataContinuityService::make_gap_request(
                            bar_request,
                            from_time_ms,
                            last_closed_time_ms,
                            entry->continuity.max_backfill_bars);
                    const auto request_from_time_ms =
                        MarketDataContinuityService::seconds_to_milliseconds(
                            history_request.from_ts);
                    const auto request_to_time_ms =
                        MarketDataContinuityService::seconds_to_milliseconds(
                            history_request.to_ts);
                    if (request_from_time_ms == 0 ||
                        request_to_time_ms < request_from_time_ms) {
                        return;
                    }

                    const auto request_span =
                        (request_to_time_ms - request_from_time_ms) /
                            timeframe_ms + 1U;
                    const auto requested_items = request_span >
                            static_cast<std::uint64_t>(
                                std::numeric_limits<std::size_t>::max())
                        ? std::numeric_limits<std::size_t>::max()
                        : static_cast<std::size_t>(request_span);

                    mark_unverified_no_lock(entry, from_time_ms);
                    continuity.phase = ContinuityPhase::RECOVERING;
                    continuity.request_in_flight = true;
                    continuity.reconnect_target_time_ms = last_closed_time_ms;
                    pending.router_id = router_id;
                    pending.subscription = entry->control->provider_subscription;
                    pending.request = history_request;
                    pending.kind = ContinuityRequestKind::RECONNECT_BACKFILL;
                    pending.from_time_ms = request_from_time_ms;
                    pending.to_time_ms = request_to_time_ms;
                    pending.requested_items = requested_items;
                    pending.attempt = 1;
                }
            }

            if (notify_live) {
                if (subscriber) {
                    subscriber->on_market_data_continuity(make_continuity_update(
                        live_subscription,
                        MarketDataContinuityStatus::LIVE,
                        0,
                        0,
                        0,
                        0,
                        "No observed bars required reconnect recovery."));
                }
                return;
            }

            request_continuity_history(
                pending.router_id,
                std::move(pending.subscription),
                std::move(pending.request),
                pending.kind,
                pending.announce_gap,
                pending.from_time_ms,
                pending.to_time_ms,
                pending.requested_items,
                pending.attempt);
        }

        inline void MarketDataRouterState::request_continuity_history(
                RoutedSubscriptionId router_id,
                MarketDataSubscriptionHandle subscription,
                BarHistoryRequest request,
                ContinuityRequestKind kind,
                bool announce_gap,
                std::uint64_t from_time_ms,
                std::uint64_t to_time_ms,
                std::size_t requested_items,
                std::size_t attempt,
                std::uint64_t generation) {
            BaseMarketDataProvider* provider = nullptr;
            auto operation = std::make_shared<ContinuityOperation>();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto entry_it = m_entries.find(router_id);
                if (m_shutdown ||
                    entry_it == m_entries.end() ||
                    entry_it->second->phase != EntryPhase::ACTIVE ||
                    !entry_it->second->continuity_state.request_in_flight) {
                    return;
                }
                if (generation != 0 &&
                    generation != entry_it->second->continuity_state.generation) {
                    return;
                }
                generation = entry_it->second->continuity_state.generation;
                provider = entry_it->second->provider;
                if (provider) ++m_continuity_operations_in_flight;
            }
            if (!provider) return;

            if (kind == ContinuityRequestKind::GAP_BACKFILL) {
                if (announce_gap) {
                    notify_continuity(
                        router_id,
                        make_continuity_update(
                            subscription,
                            MarketDataContinuityStatus::GAP_DETECTED,
                            from_time_ms,
                            to_time_ms,
                            requested_items,
                            0,
                            "A gap was detected in the live bar stream."));
                }
                notify_continuity(
                    router_id,
                    make_continuity_update(
                        subscription,
                        MarketDataContinuityStatus::BACKFILLING,
                        from_time_ms,
                        to_time_ms,
                        requested_items,
                        0,
                        "Requesting historical bars for the detected gap."));
            } else if (kind == ContinuityRequestKind::RECONNECT_BACKFILL) {
                notify_continuity(
                    router_id,
                    make_continuity_update(
                        subscription,
                        MarketDataContinuityStatus::BACKFILLING,
                        from_time_ms,
                        to_time_ms,
                        requested_items,
                        0,
                        "Revalidating overlapping historical bars after reconnect."));
            }

            const auto state = shared_from_this();
            auto complete_on_owner = [state,
                                            operation,
                                            router_id,
                                            subscription,
                                            request,
                                            kind,
                                            from_time_ms,
                                            to_time_ms,
                                            requested_items,
                                            attempt,
                                            generation](BarHistoryResult result) mutable {
                if (!state->record_continuity_completion(operation)) return;

                auto task = [state,
                             router_id,
                             subscription,
                             request,
                             kind,
                             from_time_ms,
                             to_time_ms,
                             requested_items,
                             attempt,
                             generation,
                             result = std::move(result)]() mutable {
                    state->complete_continuity(
                        router_id,
                        subscription,
                        request,
                        kind,
                        from_time_ms,
                        to_time_ms,
                        requested_items,
                        attempt,
                        generation,
                        std::move(result));
                };
                state->dispatch_or_run(std::move(task));
            };
            try {
                const bool accepted = provider->fetch_bar_history(
                    request,
                    [complete_on_owner](BarHistoryResult result) mutable {
                        complete_on_owner(std::move(result));
                    });
                if (!accepted) {
                    complete_on_owner(BarHistoryResult::fail(
                        "Market-data provider did not accept the history request."));
                }
            } catch (const std::exception& exception) {
                complete_on_owner(BarHistoryResult::fail(
                    std::string("Market-data provider history request threw: ") +
                    exception.what()));
            } catch (...) {
                complete_on_owner(BarHistoryResult::fail(
                    "Market-data provider history request threw."));
            }
        }

        inline bool MarketDataRouterState::history_covers_range(
                const BarDataBatch& batch,
                std::uint64_t from_time_ms,
                std::uint64_t to_time_ms,
                std::uint64_t timeframe_ms) noexcept {
            if (from_time_ms == 0 ||
                to_time_ms < from_time_ms ||
                timeframe_ms == 0) {
                return false;
            }

            auto expected_time_ms = from_time_ms;
            for (const auto& bar : batch.items) {
                if (bar.time_ms < expected_time_ms) continue;
                if (bar.time_ms != expected_time_ms) return false;
                if (expected_time_ms == to_time_ms) return true;
                if (expected_time_ms >
                    std::numeric_limits<std::uint64_t>::max() - timeframe_ms) {
                    return false;
                }
                expected_time_ms += timeframe_ms;
            }
            return false;
        }

        inline void MarketDataRouterState::clip_history_to_range(
                BarDataBatch& batch,
                std::uint64_t from_time_ms,
                std::uint64_t to_time_ms) {
            batch.items.erase(
                std::remove_if(
                    batch.items.begin(),
                    batch.items.end(),
                    [from_time_ms, to_time_ms](const Bar& bar) {
                        return bar.time_ms < from_time_ms ||
                            bar.time_ms > to_time_ms;
                    }),
                batch.items.end());
        }

        inline void MarketDataRouterState::mark_unverified_no_lock(
                const std::shared_ptr<Entry>& entry,
                std::uint64_t from_time_ms) noexcept {
            if (!entry || from_time_ms == 0) return;
            auto& unverified_from =
                entry->continuity_state.unverified_from_time_ms;
            if (unverified_from == 0 || from_time_ms < unverified_from) {
                unverified_from = from_time_ms;
            }
        }

        inline void MarketDataRouterState::record_verified_range_no_lock(
                const std::shared_ptr<Entry>& entry,
                std::uint64_t from_time_ms,
                std::uint64_t to_time_ms,
                std::uint64_t timeframe_ms,
                bool has_more_reconnect_history) noexcept {
            if (!entry || from_time_ms == 0 || to_time_ms < from_time_ms) return;

            auto& continuity = entry->continuity_state;
            if (continuity.unverified_from_time_ms != 0 &&
                (from_time_ms > continuity.unverified_from_time_ms ||
                 to_time_ms < continuity.unverified_from_time_ms)) {
                return;
            }

            continuity.verified_through_time_ms = std::max(
                continuity.verified_through_time_ms,
                to_time_ms);
            if (!has_more_reconnect_history) {
                continuity.unverified_from_time_ms = 0;
                return;
            }

            continuity.unverified_from_time_ms =
                timeframe_ms > 0 &&
                    to_time_ms <=
                        std::numeric_limits<std::uint64_t>::max() - timeframe_ms
                ? to_time_ms + timeframe_ms
                : to_time_ms;
        }

        inline void MarketDataRouterState::record_bar_progress_no_lock(
                const std::shared_ptr<Entry>& entry,
                const std::vector<Bar>& bars) noexcept {
            if (!entry) return;
            auto& continuity = entry->continuity_state;
            for (const auto& bar : bars) {
                if (bar.time_ms > continuity.last_observed_time_ms) {
                    continuity.last_observed_time_ms = bar.time_ms;
                }

                const bool trusted_finalized =
                    bar.has_flag(MarketDataFlags::FINALIZED) ||
                    (bar.has_flag(MarketDataFlags::HISTORICAL) &&
                     !bar.has_flag(MarketDataFlags::INCOMPLETE));
                if (trusted_finalized &&
                    (continuity.unverified_from_time_ms == 0 ||
                     bar.time_ms < continuity.unverified_from_time_ms) &&
                    bar.time_ms > continuity.verified_through_time_ms) {
                    continuity.verified_through_time_ms = bar.time_ms;
                }
            }
        }

        inline std::chrono::steady_clock::duration
        MarketDataRouterState::continuity_retry_delay(
                const MarketDataContinuityRetryPolicy& policy,
                std::size_t attempt) noexcept {
            if (attempt == 0 || policy.initial_backoff_ms == 0) {
                return std::chrono::steady_clock::duration::zero();
            }

            auto delay_ms = policy.initial_backoff_ms;
            for (std::size_t index = 1; index < attempt; ++index) {
                if (delay_ms > std::numeric_limits<std::uint64_t>::max() / 2U) {
                    delay_ms = std::numeric_limits<std::uint64_t>::max();
                    break;
                }
                delay_ms *= 2U;
                if (policy.max_backoff_ms > 0 &&
                    delay_ms >= policy.max_backoff_ms) {
                    delay_ms = policy.max_backoff_ms;
                    break;
                }
            }
            if (policy.max_backoff_ms > 0) {
                delay_ms = std::min(delay_ms, policy.max_backoff_ms);
            }

            using milliseconds = std::chrono::milliseconds;
            const auto max_delay_count = std::chrono::duration_cast<milliseconds>(
                std::chrono::steady_clock::duration::max()).count();
            const auto max_delay_ms = max_delay_count > 0
                ? static_cast<std::uint64_t>(max_delay_count)
                : 0U;
            return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                milliseconds(static_cast<milliseconds::rep>(
                    std::min(delay_ms, max_delay_ms))));
        }

        inline void MarketDataRouterState::complete_continuity(
                RoutedSubscriptionId router_id,
                MarketDataSubscriptionHandle subscription,
                BarHistoryRequest request,
                ContinuityRequestKind kind,
                std::uint64_t from_time_ms,
                std::uint64_t to_time_ms,
                std::size_t requested_items,
                std::size_t attempt,
                std::uint64_t generation,
                BarHistoryResult result) {
            StreamDescriptor expected_stream;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto entry_it = m_entries.find(router_id);
                if (entry_it == m_entries.end() ||
                    entry_it->second->phase != EntryPhase::ACTIVE ||
                    !entry_it->second->continuity_state.request_in_flight ||
                    entry_it->second->continuity_state.generation != generation) {
                    return;
                }
                expected_stream = entry_it->second->stream;
                entry_it->second->continuity_state.request_in_flight = false;
                entry_it->second->continuity_state.phase =
                    ContinuityPhase::FLUSHING;
            }

            const bool history_success = static_cast<bool>(result);
            const auto timeframe_ms = expected_stream.timeframe > 0
                ? static_cast<std::uint64_t>(expected_stream.timeframe) * 1000U
                : 0U;
            std::size_t delivered_history_items = 0;
            BarDataBatch history_batch;
            bool has_history_batch = false;
            bool history_stream_matches = false;
            if (history_success) {
                history_batch = *MarketDataContinuityService::make_bar_batch(
                    std::move(result.sequence),
                    request,
                    subscription,
                    kind == ContinuityRequestKind::GAP_BACKFILL);
                history_stream_matches = batch_matches_stream(
                    history_batch,
                    expected_stream);
                if (history_stream_matches) {
                    if (kind != ContinuityRequestKind::PREFILL) {
                        clip_history_to_range(
                            history_batch,
                            from_time_ms,
                            to_time_ms);
                    }
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto entry_it = m_entries.find(router_id);
                    if (entry_it == m_entries.end() ||
                        entry_it->second->phase != EntryPhase::ACTIVE ||
                        entry_it->second->continuity_state.generation != generation) {
                        return;
                    }
                    delivered_history_items = history_batch.items.size();
                    has_history_batch = !history_batch.items.empty();
                }
            }

            const bool recovery_requires_full_range =
                kind != ContinuityRequestKind::PREFILL;
            const bool history_covers_recovery_range =
                !recovery_requires_full_range ||
                history_covers_range(
                    history_batch,
                    from_time_ms,
                    to_time_ms,
                    timeframe_ms);
            if (recovery_requires_full_range &&
                !history_covers_recovery_range) {
                // Do not expose a partial recovery response as trusted history.
                // The buffered live payload is released below as degraded data.
                has_history_batch = false;
                delivered_history_items = 0;
            }
            const bool usable_history = history_success &&
                history_stream_matches &&
                (kind == ContinuityRequestKind::PREFILL ||
                 delivered_history_items > 0) &&
                history_covers_recovery_range;

            bool retry_scheduled = false;
            if (!history_success) {
                PendingContinuityRequest retry_request;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto entry_it = m_entries.find(router_id);
                    if (entry_it == m_entries.end() ||
                        entry_it->second->phase != EntryPhase::ACTIVE ||
                        entry_it->second->continuity_state.generation != generation) {
                        return;
                    }

                    const auto& entry = entry_it->second;
                    if (attempt < entry->continuity.retry.max_attempts) {
                        retry_request.router_id = router_id;
                        retry_request.subscription = subscription;
                        retry_request.request = request;
                        retry_request.kind = kind;
                        retry_request.announce_gap = false;
                        retry_request.from_time_ms = from_time_ms;
                        retry_request.to_time_ms = to_time_ms;
                        retry_request.requested_items = requested_items;
                        retry_request.attempt = attempt + 1;

                        const auto now = std::chrono::steady_clock::now();
                        const auto delay = continuity_retry_delay(
                            entry->continuity.retry,
                            attempt);
                        const auto remaining =
                            std::chrono::steady_clock::time_point::max() - now;
                        entry->continuity_state.retry_request =
                            std::move(retry_request);
                        entry->continuity_state.retry_at = delay >= remaining
                            ? std::chrono::steady_clock::time_point::max()
                            : now + delay;
                        entry->continuity_state.phase =
                            kind == ContinuityRequestKind::PREFILL
                                ? ContinuityPhase::PREFILLING
                                : ContinuityPhase::RECOVERING;
                        retry_scheduled = true;
                    }
                }

                if (retry_scheduled) {
                    notify_continuity(
                        router_id,
                        make_continuity_update(
                            subscription,
                            MarketDataContinuityStatus::RETRYING,
                            from_time_ms,
                            to_time_ms,
                            requested_items,
                            0,
                            "Retrying historical market-data continuity request."));
                    return;
                }
            }

            bool start_reconnect_after_prefill = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto entry_it = m_entries.find(router_id);
                if (entry_it == m_entries.end() ||
                    entry_it->second->phase != EntryPhase::ACTIVE ||
                    entry_it->second->continuity_state.generation != generation) {
                    return;
                }

                const auto& entry = entry_it->second;
                if (kind == ContinuityRequestKind::PREFILL) {
                    entry->continuity_state.initial_prefill_pending = false;
                    start_reconnect_after_prefill =
                        usable_history &&
                        entry->continuity.recovers_gaps() &&
                        entry->continuity_state.unverified_from_time_ms != 0;
                    if (start_reconnect_after_prefill) {
                        entry->continuity_state.phase =
                            ContinuityPhase::RECOVERING;
                    }
                }
                if (!usable_history) {
                    mark_unverified_no_lock(entry, from_time_ms);
                } else if (recovery_requires_full_range) {
                    const bool has_more_reconnect_history =
                        kind == ContinuityRequestKind::RECONNECT_BACKFILL &&
                        entry->continuity_state.reconnect_target_time_ms > to_time_ms;
                    record_verified_range_no_lock(
                        entry,
                        from_time_ms,
                        to_time_ms,
                        timeframe_ms,
                        has_more_reconnect_history);
                }
            }

            if (!usable_history) {
                notify_continuity(
                    router_id,
                    make_continuity_update(
                        subscription,
                        MarketDataContinuityStatus::FAILED,
                        from_time_ms,
                        to_time_ms,
                        requested_items,
                        0,
                        result.error_desc.empty()
                            ? (!history_success
                                ? "Historical market-data continuity request failed."
                                : !history_stream_matches
                                ? "Historical bar response does not match the subscribed stream."
                                : recovery_requires_full_range
                                ? "Historical bars do not cover the requested recovery range."
                                : "No historical bars were returned for prefill.")
                            : result.error_desc));
            }

            if (has_history_batch) {
                std::shared_ptr<IMarketDataSubscriber> subscriber;
                bool active = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto entry_it = m_entries.find(router_id);
                    active = entry_it != m_entries.end() &&
                        entry_it->second->phase == EntryPhase::ACTIVE &&
                        entry_it->second->continuity_state.generation == generation &&
                        !entry_it->second->release_requested;
                    if (active) {
                        subscriber = entry_it->second->subscriber.lock();
                        record_bar_progress_no_lock(
                            entry_it->second,
                            history_batch.items);
                    }
                }
                if (active && subscriber) {
                    subscriber->on_bar_data(history_batch);
                }
            }

            if (start_reconnect_after_prefill) {
                start_reconnect_recovery(router_id);
                return;
            }

            bool schedule_next_reconnect = false;
            PendingContinuityRequest next_reconnect;
            if (usable_history &&
                kind == ContinuityRequestKind::RECONNECT_BACKFILL) {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto entry_it = m_entries.find(router_id);
                if (entry_it == m_entries.end() ||
                    entry_it->second->phase != EntryPhase::ACTIVE ||
                    entry_it->second->continuity_state.generation != generation) {
                    return;
                }

                const auto& entry = entry_it->second;
                auto& continuity = entry->continuity_state;
                if (continuity.reconnect_target_time_ms > to_time_ms) {
                    if (timeframe_ms == 0 ||
                        to_time_ms >
                            std::numeric_limits<std::uint64_t>::max() - timeframe_ms) {
                        return;
                    }

                    const auto next_from_time_ms = to_time_ms + timeframe_ms;
                    const auto next_to_time_ms =
                        continuity.reconnect_target_time_ms;
                    BarSubscriptionRequest bar_request(
                        entry->stream.symbol,
                        entry->stream.timeframe,
                        entry->stream.price_source,
                        entry->stream.transport);
                    bar_request.continuity = entry->continuity;
                    const auto next_request =
                        MarketDataContinuityService::make_gap_request(
                            bar_request,
                            next_from_time_ms,
                            next_to_time_ms,
                            entry->continuity.max_backfill_bars);
                    const auto next_request_from_time_ms =
                        MarketDataContinuityService::seconds_to_milliseconds(
                            next_request.from_ts);
                    const auto bounded_next_to_time_ms =
                        MarketDataContinuityService::seconds_to_milliseconds(
                            next_request.to_ts);
                    if (next_request_from_time_ms == 0 ||
                        bounded_next_to_time_ms < next_request_from_time_ms) {
                        return;
                    }

                    const auto request_span =
                        (bounded_next_to_time_ms - next_request_from_time_ms) /
                            timeframe_ms + 1U;
                    next_reconnect.router_id = router_id;
                    next_reconnect.subscription =
                        entry->control->provider_subscription;
                    next_reconnect.request = next_request;
                    next_reconnect.kind = ContinuityRequestKind::RECONNECT_BACKFILL;
                    next_reconnect.from_time_ms = next_request_from_time_ms;
                    next_reconnect.to_time_ms = bounded_next_to_time_ms;
                    next_reconnect.requested_items = request_span >
                            static_cast<std::uint64_t>(
                                std::numeric_limits<std::size_t>::max())
                        ? std::numeric_limits<std::size_t>::max()
                        : static_cast<std::size_t>(request_span);
                    next_reconnect.attempt = 1;
                    continuity.phase = ContinuityPhase::RECOVERING;
                    continuity.request_in_flight = true;
                    schedule_next_reconnect = true;
                }
            }

            if (schedule_next_reconnect) {
                request_continuity_history(
                    next_reconnect.router_id,
                    std::move(next_reconnect.subscription),
                    std::move(next_reconnect.request),
                    next_reconnect.kind,
                    next_reconnect.announce_gap,
                    next_reconnect.from_time_ms,
                    next_reconnect.to_time_ms,
                    next_reconnect.requested_items,
                    next_reconnect.attempt);
                return;
            }

            for (;;) {
                std::vector<std::pair<
                    std::shared_ptr<IMarketDataSubscriber>,
                    BarDataBatch>> deliveries;
                std::vector<std::pair<
                    std::shared_ptr<IMarketDataSubscriber>,
                    MarketDataContinuityUpdate>> continuity_deliveries;
                std::vector<PendingContinuityRequest> continuity_requests;
                bool finished = false;
                bool continuity_still_enabled = false;
                bool continuity_verified = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto entry_it = m_entries.find(router_id);
                    if (entry_it == m_entries.end() ||
                        entry_it->second->phase != EntryPhase::ACTIVE ||
                        entry_it->second->continuity_state.generation != generation) {
                        return;
                    }
                    auto& continuity = entry_it->second->continuity_state;
                    if (continuity.buffer.empty()) {
                        continuity_still_enabled =
                            entry_it->second->continuity.enabled();
                        continuity_verified = usable_history &&
                            continuity.unverified_from_time_ms == 0;
                        continuity.phase = continuity_verified
                            ? ContinuityPhase::LIVE
                            : ContinuityPhase::DEGRADED;
                        continuity.reconnect_target_time_ms = 0;
                        finished = true;
                    } else {
                        auto batch = std::move(continuity.buffer.front());
                        continuity.buffer.pop_front();
                        if (continuity.buffered_items >= batch.items.size()) {
                            continuity.buffered_items -= batch.items.size();
                        } else {
                            continuity.buffered_items = 0;
                        }
                        route_bar_to_entry_no_lock(
                            entry_it->second,
                            batch,
                            deliveries,
                            continuity_requests,
                            continuity_deliveries,
                            true,
                            usable_history,
                            usable_history &&
                                    kind == ContinuityRequestKind::RECONNECT_BACKFILL
                                ? to_time_ms
                                : 0);
                    }
                }

                for (auto& continuity_delivery : continuity_deliveries) {
                    continuity_delivery.first->on_market_data_continuity(
                        continuity_delivery.second);
                }
                for (auto& delivery : deliveries) {
                    delivery.first->on_bar_data(delivery.second);
                }

                if (!continuity_requests.empty()) {
                    auto pending = std::move(continuity_requests.front());
                    request_continuity_history(
                        pending.router_id,
                        std::move(pending.subscription),
                        std::move(pending.request),
                        pending.kind,
                        pending.announce_gap,
                        pending.from_time_ms,
                        pending.to_time_ms,
                        pending.requested_items,
                        pending.attempt);
                    return;
                }

                if (finished) {
                    if (continuity_still_enabled) {
                        notify_continuity(
                            router_id,
                            make_continuity_update(
                                subscription,
                                continuity_verified
                                    ? MarketDataContinuityStatus::LIVE
                                    : MarketDataContinuityStatus::DEGRADED,
                                from_time_ms,
                                to_time_ms,
                                requested_items,
                                delivered_history_items,
                                continuity_verified
                                    ? "Historical market-data continuity is ready."
                                    : "Live delivery continues without verified continuity."));
                    }
                    return;
                }
            }
        }

        inline void MarketDataRouterState::fail_pending_subscribe(
                RoutedSubscriptionId router_id,
                BaseMarketDataProvider& provider,
                MarketDataSubscriptionResult result,
                subscription_callback_t callback) {
            dispatch_subscribe_completion(
                router_id,
                provider,
                std::move(result),
                std::move(callback));
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
                if (!subscription.valid()) {
                    subscription = entry->retained_cleanup_subscription;
                }
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

            return start_unsubscribe(
                entry,
                std::move(subscription),
                std::move(callback));
        }

        inline bool MarketDataRouterState::start_unsubscribe(
                const std::shared_ptr<Entry>& entry,
                MarketDataSubscriptionHandle subscription,
                subscription_callback_t callback) {
            if (!entry || !entry->provider || !subscription.valid()) return false;

            const auto state = shared_from_this();
            bool accepted = false;
            try {
                accepted = entry->provider->unsubscribe(
                    subscription,
                    [state,
                     router_id = entry->router_id,
                     subscription,
                     callback](MarketDataSubscriptionResult result) mutable {
                        state->dispatch_unsubscribe_completion(
                            router_id,
                            subscription,
                            std::move(result),
                            std::move(callback));
                    });
            } catch (const std::exception& exception) {
                dispatch_unsubscribe_completion(
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
                dispatch_unsubscribe_completion(
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
                dispatch_unsubscribe_completion(
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

        inline bool MarketDataRouterState::record_unsubscribe_completion(
                RoutedSubscriptionId router_id,
                const MarketDataSubscriptionHandle& expected_subscription,
                const MarketDataSubscriptionResult& result,
                bool& shutdown_requested) {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_entries.find(router_id);
            if (it == m_entries.end() ||
                it->second->phase != EntryPhase::UNSUBSCRIBING ||
                it->second->unsubscribe_completion_received) {
                return false;
            }

            const auto actual_subscription = result.subscription.valid()
                ? result.subscription
                : expected_subscription;
            if (!actual_subscription.valid() ||
                actual_subscription.provider_id != expected_subscription.provider_id ||
                actual_subscription.id != expected_subscription.id) {
                return false;
            }

            it->second->unsubscribe_completion = result;
            if (!it->second->unsubscribe_completion.subscription.valid()) {
                it->second->unsubscribe_completion.subscription = expected_subscription;
            }
            it->second->unsubscribe_completion_received = true;
            shutdown_requested = m_shutdown;
            return true;
        }

        inline void MarketDataRouterState::dispatch_unsubscribe_completion(
                RoutedSubscriptionId router_id,
                MarketDataSubscriptionHandle expected_subscription,
                MarketDataSubscriptionResult result,
                subscription_callback_t callback) {
            bool shutdown_requested = false;
            if (!record_unsubscribe_completion(
                    router_id,
                    expected_subscription,
                    result,
                    shutdown_requested)) {
                return;
            }

            if (shutdown_requested) {
                request_process();
                return;
            }

            auto pending = std::make_shared<MarketDataSubscriptionResult>(
                std::move(result));
            const auto state = shared_from_this();
            dispatch_or_run(
                [state,
                 router_id,
                 expected_subscription,
                 callback = std::move(callback),
                 pending]() mutable {
                    state->complete_unsubscribe(
                        router_id,
                        expected_subscription,
                        std::move(*pending),
                        std::move(callback));
                });
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
            bool notify_callback = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto it = m_entries.find(router_id);
                if (it != m_entries.end() &&
                    it->second->phase == EntryPhase::UNSUBSCRIBING &&
                    (!expected_subscription.valid() ||
                     it->second->unsubscribe_completion_received)) {
                    it->second->unsubscribe_completion_received = false;
                    it->second->unsubscribe_completion = {};
                    if (result.success()) {
                        set_control_released(it->second->control);
                        unbind = remove_entry_no_lock(router_id);
                    } else {
                        it->second->phase = EntryPhase::CLEANUP_FAILED;
                    }
                    handled = true;
                    notify_callback = !m_shutdown;
                }
            }

            if (unbind) unbind_provider(*unbind);
            if (handled && notify_callback) {
                dispatch_result(std::move(callback), std::move(result));
            }
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

        inline void MarketDataRouterState::apply_stream_status_to_entry_no_lock(
                const std::shared_ptr<Entry>& entry,
                const MarketDataStatusUpdate& update,
                std::vector<std::pair<
                    std::shared_ptr<IMarketDataSubscriber>,
                    MarketDataContinuityUpdate>>& continuity_deliveries,
                std::vector<RoutedSubscriptionId>& prefill_routes,
                std::vector<RoutedSubscriptionId>& reconnect_routes) {
            auto subscriber = entry->subscriber.lock();
            auto& continuity = entry->continuity_state;
            const bool invalidates_continuity =
                update.status == MarketDataStreamStatus::DISCONNECTED ||
                update.status == MarketDataStreamStatus::RECONNECTING ||
                update.status == MarketDataStreamStatus::FAILED ||
                update.status == MarketDataStreamStatus::STOPPED;
            const bool transport_ready =
                update.status == MarketDataStreamStatus::READY;
            const bool needs_transport_recovery =
                continuity.initial_prefill_pending ||
                entry->continuity.recovers_gaps();

            if (entry->continuity.enabled() &&
                needs_transport_recovery &&
                invalidates_continuity) {
                const bool announce_stale =
                    continuity.phase != ContinuityPhase::WAITING_FOR_READY;
                ++continuity.generation;
                continuity.request_in_flight = false;
                continuity.retry_request.reset();
                continuity.retry_at_ms = 0;
                continuity.reconnect_target_time_ms = 0;

                if (entry->continuity.recovers_gaps()) {
                    const auto timeframe_ms = static_cast<std::uint64_t>(
                        entry->stream.timeframe) * 1000U;
                    if (continuity.initial_prefill_pending) {
                        if (continuity.initial_prefill_boundary_time_ms > 0 &&
                            timeframe_ms > 0 &&
                            continuity.initial_prefill_boundary_time_ms <=
                                std::numeric_limits<std::uint64_t>::max() -
                                    timeframe_ms) {
                            mark_unverified_no_lock(
                                entry,
                                continuity.initial_prefill_boundary_time_ms +
                                    timeframe_ms);
                        }
                    } else if (continuity.verified_through_time_ms > 0 &&
                               timeframe_ms > 0 &&
                               continuity.verified_through_time_ms <=
                                   std::numeric_limits<std::uint64_t>::max() -
                                       timeframe_ms) {
                        mark_unverified_no_lock(
                            entry,
                            continuity.verified_through_time_ms + timeframe_ms);
                    } else {
                        mark_unverified_no_lock(
                            entry,
                            continuity.last_observed_time_ms);
                    }
                }
                continuity.phase = ContinuityPhase::WAITING_FOR_READY;

                if (announce_stale && subscriber) {
                    continuity_deliveries.emplace_back(
                        subscriber,
                        make_continuity_update(
                            entry->control->provider_subscription,
                            MarketDataContinuityStatus::STALE,
                            0,
                            0,
                            0,
                            0,
                            "Transport loss invalidated market-data continuity."));
                }
            }

            if (entry->continuity.enabled() && transport_ready &&
                continuity.phase == ContinuityPhase::WAITING_FOR_READY &&
                !continuity.request_in_flight) {
                if (continuity.initial_prefill_pending) {
                    continuity.phase = ContinuityPhase::PREFILLING;
                    prefill_routes.push_back(entry->router_id);
                } else if (entry->continuity.recovers_gaps()) {
                    continuity.phase = ContinuityPhase::RECOVERING;
                    reconnect_routes.push_back(entry->router_id);
                }
            }
        }

        template <typename Batch, typename MatchesStream, typename Deliver>
        inline void MarketDataRouterState::route_batch(
                ProviderInstanceId provider_id,
                std::unique_ptr<Batch> batch,
                MatchesStream matches_stream,
                Deliver deliver) {
            if (!batch) return;
            std::vector<std::pair<std::shared_ptr<IMarketDataSubscriber>, Batch>> deliveries;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto provider_it = m_providers.find(provider_id);
                if (provider_it == m_providers.end()) return;

                auto add_delivery = [&](const std::shared_ptr<Entry>& entry) {
                    if (entry->phase != EntryPhase::ACTIVE ||
                        !matches_stream(*batch, entry->stream)) {
                        return;
                    }
                    auto subscriber = entry->subscriber.lock();
                    if (!subscriber) return;
                    auto routed = *batch;
                    routed.subscription = entry->control->provider_subscription;
                    deliveries.emplace_back(std::move(subscriber), std::move(routed));
                };

                if (batch->subscription.valid()) {
                    if (batch->subscription.provider_id != provider_id) return;
                    const auto route_it = provider_it->second.provider_routes.find(
                        batch->subscription.id);
                    if (route_it == provider_it->second.provider_routes.end()) return;
                    const auto entry_it = m_entries.find(route_it->second);
                    if (entry_it == m_entries.end()) return;
                    add_delivery(entry_it->second);
                } else {
                    for (const auto& [id, entry] : m_entries) {
                        (void)id;
                        if (entry->provider_id == provider_id) {
                            add_delivery(entry);
                        }
                    }
                }
            }

            for (auto& delivery : deliveries) {
                deliver(*delivery.first, delivery.second);
            }
        }

        inline void MarketDataRouterState::route_ticks(
                ProviderInstanceId provider_id,
                std::unique_ptr<TickDataBatch> batch) {
            route_batch(
                provider_id,
                std::move(batch),
                [this](const TickDataBatch& data, const StreamDescriptor& stream) {
                    return batch_matches_stream(data, stream);
                },
                [](IMarketDataSubscriber& subscriber, TickDataBatch& data) {
                    for (auto& tick : data.items) {
                        mark_live_payload(tick.flags);
                    }
                    subscriber.on_tick_data(data);
                });
        }

        inline MarketDataContinuityUpdate
        MarketDataRouterState::make_continuity_update(
                const MarketDataSubscriptionHandle& subscription,
                MarketDataContinuityStatus status,
                std::uint64_t from_time_ms,
                std::uint64_t to_time_ms,
                std::size_t requested_items,
                std::size_t delivered_items,
                std::string message) {
            MarketDataContinuityUpdate update;
            update.subscription = subscription;
            update.type = subscription.stream_type;
            update.symbol = subscription.symbol;
            update.timeframe = subscription.timeframe;
            update.status = status;
            update.from_time_ms = from_time_ms;
            update.to_time_ms = to_time_ms;
            update.requested_items = requested_items;
            update.delivered_items = delivered_items;
            update.message = std::move(message);
            return update;
        }

        inline void MarketDataRouterState::notify_continuity(
                RoutedSubscriptionId router_id,
                MarketDataContinuityUpdate update) {
            std::shared_ptr<IMarketDataSubscriber> subscriber;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto entry_it = m_entries.find(router_id);
                if (entry_it == m_entries.end() ||
                    entry_it->second->phase != EntryPhase::ACTIVE) {
                    return;
                }
                subscriber = entry_it->second->subscriber.lock();
            }
            if (subscriber) subscriber->on_market_data_continuity(update);
        }

        inline bool MarketDataRouterState::buffer_continuity_batch_no_lock(
                const std::shared_ptr<Entry>& entry,
                std::shared_ptr<IMarketDataSubscriber> subscriber,
                BarDataBatch batch,
                std::vector<std::pair<
                    std::shared_ptr<IMarketDataSubscriber>,
                    BarDataBatch>>& deliveries,
                std::vector<std::pair<
                    std::shared_ptr<IMarketDataSubscriber>,
                    MarketDataContinuityUpdate>>& continuity_deliveries,
                bool push_front) {
            for (auto& bar : batch.items) {
                mark_live_payload(bar.flags, true);
            }
            const auto batch_items = batch.items.size();
            const auto& options = entry->continuity;
            auto& continuity = entry->continuity_state;
            const bool exceeds_batch_limit =
                options.max_buffered_batches > 0 &&
                continuity.buffer.size() >= options.max_buffered_batches;
            const bool exceeds_item_limit = options.max_buffered_items > 0 &&
                (batch_items > options.max_buffered_items ||
                 continuity.buffered_items >
                     options.max_buffered_items - batch_items);

            if (!exceeds_batch_limit && !exceeds_item_limit) {
                if (push_front) {
                    continuity.buffer.push_front(std::move(batch));
                } else {
                    continuity.buffer.push_back(std::move(batch));
                }
                continuity.buffered_items += batch_items;
                return true;
            }

            while (!continuity.buffer.empty()) {
                auto buffered = std::move(continuity.buffer.front());
                continuity.buffer.pop_front();
                record_bar_progress_no_lock(entry, buffered.items);
                deliveries.emplace_back(subscriber, std::move(buffered));
            }
            continuity.buffered_items = 0;
            record_bar_progress_no_lock(entry, batch.items);
            deliveries.emplace_back(subscriber, std::move(batch));

            continuity.phase = ContinuityPhase::DEGRADED;
            continuity.initial_prefill_pending = false;
            continuity.request_in_flight = false;
            continuity.reconnect_target_time_ms = 0;
            continuity.retry_request.reset();
            continuity.retry_at = {};
            entry->continuity.mode = MarketDataContinuityMode::LIVE_ONLY;

            continuity_deliveries.emplace_back(
                subscriber,
                make_continuity_update(
                    entry->control->provider_subscription,
                    MarketDataContinuityStatus::FAILED,
                    0,
                    0,
                    0,
                    0,
                    "Continuity buffer limit exceeded; live delivery continues."));
            continuity_deliveries.emplace_back(
                subscriber,
                make_continuity_update(
                    entry->control->provider_subscription,
                    MarketDataContinuityStatus::DEGRADED,
                    0,
                    0,
                    0,
                    0,
                    "Live delivery resumed without verified continuity after buffer overflow."));
            return false;
        }

        inline bool MarketDataRouterState::route_bar_to_entry_no_lock(
                const std::shared_ptr<Entry>& entry,
                const BarDataBatch& batch,
                std::vector<std::pair<
                        std::shared_ptr<IMarketDataSubscriber>,
                        BarDataBatch>>& deliveries,
                std::vector<PendingContinuityRequest>& continuity_requests,
                std::vector<std::pair<
                        std::shared_ptr<IMarketDataSubscriber>,
                        MarketDataContinuityUpdate>>& continuity_deliveries,
                bool process_buffered,
                bool allow_gap_recovery,
                std::uint64_t confirmed_through_time_ms) {
            if (!entry || entry->phase != EntryPhase::ACTIVE ||
                !batch_matches_stream(batch, entry->stream)) {
                return false;
            }

            auto subscriber = entry->subscriber.lock();
            if (!subscriber) return false;

            auto routed = batch;
            routed.subscription = entry->control->provider_subscription;
            if (routed.items.empty()) return false;

            for (auto& bar : routed.items) {
                mark_live_payload(bar.flags, process_buffered);
            }

            if (process_buffered && confirmed_through_time_ms > 0) {
                // A successful history overlap is authoritative for already
                // finalized slots. Drop their buffered live snapshots so a
                // reconnect cannot deliver the same candle twice.
                auto item = routed.items.begin();
                while (item != routed.items.end()) {
                    if (item->time_ms != 0 &&
                        item->time_ms <= confirmed_through_time_ms) {
                        item = routed.items.erase(item);
                    } else {
                        ++item;
                    }
                }
            }
            if (routed.items.empty()) return false;

            auto& continuity = entry->continuity_state;
            if (entry->continuity.enabled() && !process_buffered &&
                continuity.buffers_live_data()) {
                buffer_continuity_batch_no_lock(
                    entry,
                    std::move(subscriber),
                    std::move(routed),
                    deliveries,
                    continuity_deliveries,
                    false);
                return false;
            }

            if (allow_gap_recovery && entry->continuity.recovers_gaps() &&
                !continuity.request_in_flight &&
                entry->stream.timeframe > 0 &&
                continuity.last_observed_time_ms > 0) {
                const auto timeframe_ms =
                    static_cast<std::uint64_t>(entry->stream.timeframe) * 1000U;
                std::uint64_t previous_time_ms =
                    continuity.last_observed_time_ms;
                for (std::size_t index = 0; index < routed.items.size(); ++index) {
                    const auto& bar = routed.items[index];
                    if (bar.time_ms == 0) continue;
                    const auto expected_time_ms = previous_time_ms >
                            std::numeric_limits<std::uint64_t>::max() - timeframe_ms
                        ? std::numeric_limits<std::uint64_t>::max()
                        : previous_time_ms + timeframe_ms;
                    if (bar.time_ms > expected_time_ms) {
                        const auto gap_from_ms = expected_time_ms;
                        const auto gap_to_ms = bar.time_ms - timeframe_ms;
                        if (gap_to_ms >= gap_from_ms) {
                            BarSubscriptionRequest request(
                                entry->stream.symbol,
                                entry->stream.timeframe,
                                entry->stream.price_source,
                                entry->stream.transport);
                            request.continuity = entry->continuity;

                            const auto history_request =
                                MarketDataContinuityService::make_gap_request(
                                    request,
                                    gap_from_ms,
                                    gap_to_ms,
                                    entry->continuity.max_backfill_bars);
                            const auto request_from_time_ms =
                                MarketDataContinuityService::seconds_to_milliseconds(
                                    history_request.from_ts);
                            const auto request_to_time_ms =
                                MarketDataContinuityService::seconds_to_milliseconds(
                                    history_request.to_ts);
                            if (request_from_time_ms == 0 ||
                                request_to_time_ms < request_from_time_ms) {
                                return false;
                            }

                            const auto gap_items =
                                ((request_to_time_ms - request_from_time_ms) /
                                    timeframe_ms) + 1U;
                            const auto requested_items = gap_items >
                                    static_cast<std::uint64_t>(
                                        std::numeric_limits<std::size_t>::max())
                                ? std::numeric_limits<std::size_t>::max()
                                : static_cast<std::size_t>(gap_items);

                            if (index > 0) {
                                BarDataBatch prefix = routed;
                                prefix.items.resize(index);
                                record_bar_progress_no_lock(
                                    entry,
                                    prefix.items);
                                deliveries.emplace_back(subscriber, std::move(prefix));
                            }

                            routed.items.erase(
                                routed.items.begin(),
                                routed.items.begin() + static_cast<std::ptrdiff_t>(index));
                            if (!buffer_continuity_batch_no_lock(
                                    entry,
                                    subscriber,
                                    std::move(routed),
                                    deliveries,
                                    continuity_deliveries,
                                    process_buffered)) {
                                return false;
                            }
                            mark_unverified_no_lock(entry, request_from_time_ms);
                            continuity.phase = ContinuityPhase::RECOVERING;
                            continuity.request_in_flight = true;
                            continuity_requests.push_back(PendingContinuityRequest{
                                entry->router_id,
                                entry->control->provider_subscription,
                                history_request,
                                ContinuityRequestKind::GAP_BACKFILL,
                                true,
                                request_from_time_ms,
                                request_to_time_ms,
                                requested_items,
                                1});
                            return false;
                        }
                    }
                    if (bar.time_ms > previous_time_ms) {
                        previous_time_ms = bar.time_ms;
                    }
                }
            }

            record_bar_progress_no_lock(entry, routed.items);
            deliveries.emplace_back(std::move(subscriber), std::move(routed));
            return true;
        }

        inline void MarketDataRouterState::route_bars(
                ProviderInstanceId provider_id,
                std::unique_ptr<BarDataBatch> batch) {
            if (!batch) return;

            std::vector<std::pair<
                std::shared_ptr<IMarketDataSubscriber>,
                BarDataBatch>> deliveries;
            std::vector<std::pair<
                std::shared_ptr<IMarketDataSubscriber>,
                MarketDataContinuityUpdate>> continuity_deliveries;
            std::vector<PendingContinuityRequest> continuity_requests;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto provider_it = m_providers.find(provider_id);
                if (provider_it == m_providers.end()) return;

                auto route_one = [&](const std::shared_ptr<Entry>& entry) {
                    route_bar_to_entry_no_lock(
                        entry,
                        *batch,
                        deliveries,
                        continuity_requests,
                        continuity_deliveries,
                        false);
                };

                if (batch->subscription.valid()) {
                    if (batch->subscription.provider_id != provider_id) return;
                    const auto route_it = provider_it->second.provider_routes.find(
                        batch->subscription.id);
                    if (route_it == provider_it->second.provider_routes.end()) return;
                    const auto entry_it = m_entries.find(route_it->second);
                    if (entry_it != m_entries.end()) route_one(entry_it->second);
                } else {
                    for (const auto& [id, entry] : m_entries) {
                        (void)id;
                        if (entry->provider_id == provider_id) route_one(entry);
                    }
                }
            }

            for (auto& continuity_delivery : continuity_deliveries) {
                continuity_delivery.first->on_market_data_continuity(
                    continuity_delivery.second);
            }
            for (auto& delivery : deliveries) {
                delivery.first->on_bar_data(delivery.second);
            }
            for (auto& request : continuity_requests) {
                request_continuity_history(
                    request.router_id,
                    std::move(request.subscription),
                    std::move(request.request),
                    request.kind,
                    request.announce_gap,
                    request.from_time_ms,
                    request.to_time_ms,
                    request.requested_items,
                    request.attempt);
            }
        }

        inline void MarketDataRouterState::route_status(
                ProviderInstanceId provider_id,
                MarketDataStatusUpdate update) {
            std::vector<std::pair<std::shared_ptr<IMarketDataSubscriber>, MarketDataStatusUpdate>>
                deliveries;
            std::vector<std::pair<
                std::shared_ptr<IMarketDataSubscriber>,
                MarketDataContinuityUpdate>> continuity_deliveries;
            std::vector<RoutedSubscriptionId> prefill_routes;
            std::vector<RoutedSubscriptionId> reconnect_routes;
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

                auto route_one = [&](const std::shared_ptr<Entry>& entry) {
                    if (!entry ||
                        entry->phase != EntryPhase::ACTIVE ||
                        !status_matches_stream(update, entry->stream)) {
                        return;
                    }

                    apply_stream_status_to_entry_no_lock(
                        entry,
                        update,
                        continuity_deliveries,
                        prefill_routes,
                        reconnect_routes);

                    auto subscriber = entry->subscriber.lock();
                    if (subscriber) {
                        auto routed = update;
                        routed.subscription = entry->control->provider_subscription;
                        deliveries.emplace_back(
                            std::move(subscriber),
                            std::move(routed));
                    }
                };

                if (update.subscription.valid()) {
                    const auto route_it = provider_it->second.provider_routes.find(
                        update.subscription.id);
                    if (route_it != provider_it->second.provider_routes.end()) {
                        const auto entry_it = m_entries.find(route_it->second);
                        if (entry_it != m_entries.end() &&
                            entry_it->second->phase == EntryPhase::ACTIVE) {
                            route_one(entry_it->second);
                        }
                    }
                } else {
                    for (const auto& [id, entry] : m_entries) {
                        (void)id;
                        if (entry->provider_id == provider_id) {
                            route_one(entry);
                        }
                    }
                }
            }

            for (auto& delivery : deliveries) {
                delivery.first->on_market_data_status(delivery.second);
            }
            for (auto& delivery : continuity_deliveries) {
                delivery.first->on_market_data_continuity(delivery.second);
            }
            for (const auto router_id : prefill_routes) {
                MarketDataSubscriptionHandle subscription;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto entry_it = m_entries.find(router_id);
                    if (entry_it != m_entries.end()) {
                        subscription =
                            entry_it->second->control->provider_subscription;
                    }
                }
                start_continuity(router_id, std::move(subscription));
            }
            for (const auto router_id : reconnect_routes) {
                start_reconnect_recovery(router_id);
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

        inline void MarketDataRouterState::process() {
            struct PendingCompletion {
                RoutedSubscriptionId router_id;
                MarketDataSubscriptionHandle subscription;
                MarketDataSubscriptionResult result;
            };
            struct CleanupRequest {
                std::shared_ptr<Entry> entry;
                MarketDataSubscriptionHandle subscription;
            };

            for (;;) {
                std::vector<PendingContinuityRequest> retry_requests;
                std::vector<RoutedSubscriptionId> reconnect_routes;
                std::vector<PendingCompletion> completions;
                bool shutting_down = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_shutdown_complete) return;
                    shutting_down = m_shutdown;

                    if (!shutting_down) {
                        const auto now = std::chrono::steady_clock::now();
                        for (const auto& [id, entry] : m_entries) {
                            (void)id;
                            auto& continuity = entry->continuity_state;
                            if (entry->phase != EntryPhase::ACTIVE ||
                                continuity.request_in_flight ||
                                !continuity.retry_request ||
                                continuity.retry_at > now) {
                                continue;
                            }
                            retry_requests.push_back(*continuity.retry_request);
                            continuity.phase =
                                continuity.retry_request->kind ==
                                        ContinuityRequestKind::PREFILL
                                    ? ContinuityPhase::PREFILLING
                                    : ContinuityPhase::RECOVERING;
                            continuity.retry_request.reset();
                            continuity.retry_at = {};
                            continuity.request_in_flight = true;
                        }
                        for (const auto& [id, entry] : m_entries) {
                            const auto& continuity = entry->continuity_state;
                            if (entry->phase == EntryPhase::ACTIVE &&
                                continuity.phase == ContinuityPhase::RECOVERING &&
                                !continuity.initial_prefill_pending &&
                                !continuity.request_in_flight &&
                                !continuity.retry_request) {
                                reconnect_routes.push_back(id);
                            }
                        }
                    }

                    if (shutting_down) {
                        completions.reserve(m_entries.size());
                        for (const auto& [id, entry] : m_entries) {
                            if (entry->phase != EntryPhase::UNSUBSCRIBING ||
                                !entry->unsubscribe_completion_received) {
                                continue;
                            }
                            completions.push_back(PendingCompletion{
                                id,
                                entry->unsubscribe_completion.subscription,
                                entry->unsubscribe_completion});
                        }
                    }
                }

                for (auto& retry : retry_requests) {
                    request_continuity_history(
                        retry.router_id,
                        std::move(retry.subscription),
                        std::move(retry.request),
                        retry.kind,
                        retry.announce_gap,
                        retry.from_time_ms,
                        retry.to_time_ms,
                        retry.requested_items,
                        retry.attempt);
                }

                for (const auto router_id : reconnect_routes) {
                    start_reconnect_recovery(router_id);
                }

                if (!shutting_down) return;

                for (auto& completion : completions) {
                    complete_unsubscribe(
                        completion.router_id,
                        std::move(completion.subscription),
                        std::move(completion.result),
                        {});
                }

                std::vector<RoutedSubscriptionId> failed_subscribes;
                std::vector<CleanupRequest> cleanup_requests;
                std::vector<BaseMarketDataProvider*> unbind_providers;
                bool shutdown_complete = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    failed_subscribes.reserve(m_entries.size());
                    cleanup_requests.reserve(m_entries.size());

                    for (const auto& [id, entry] : m_entries) {
                        MarketDataSubscriptionHandle subscription;
                        if (entry->phase == EntryPhase::PENDING &&
                            entry->subscribe_completion_received) {
                            if (!entry->retained_cleanup_subscription.valid()) {
                                failed_subscribes.push_back(id);
                                continue;
                            }
                            subscription = entry->retained_cleanup_subscription;
                        } else if (entry->phase == EntryPhase::ACTIVE) {
                            subscription = entry->control->provider_subscription;
                        } else {
                            continue;
                        }

                        if (!entry->provider || !subscription.valid()) {
                            entry->phase = EntryPhase::CLEANUP_FAILED;
                            continue;
                        }
                        entry->phase = EntryPhase::UNSUBSCRIBING;
                        const auto provider_it = m_providers.find(entry->provider_id);
                        if (provider_it != m_providers.end()) {
                            provider_it->second.provider_routes.erase(subscription.id);
                        }
                        cleanup_requests.push_back(CleanupRequest{
                            entry,
                            std::move(subscription)});
                    }

                    for (const auto id : failed_subscribes) {
                        const auto entry_it = m_entries.find(id);
                        if (entry_it == m_entries.end()) continue;
                        set_control_released(entry_it->second->control);
                        if (auto* provider = remove_entry_no_lock(id)) {
                            unbind_providers.push_back(provider);
                        }
                    }

                    if (m_entries.empty() &&
                        m_continuity_operations_in_flight == 0) {
                        m_registered_providers.clear();
                        m_provider_aliases.clear();
                        m_registered_provider_ids.clear();
                        m_shutdown_complete = true;
                        shutdown_complete = true;
                    }
                }

                for (auto* provider : unbind_providers) {
                    if (provider) unbind_provider(*provider);
                }
                for (auto& request : cleanup_requests) {
                    start_unsubscribe(
                        request.entry,
                        std::move(request.subscription),
                        {});
                }

                if (shutdown_complete) return;
                if (completions.empty() &&
                    failed_subscribes.empty() &&
                    cleanup_requests.empty()) {
                    return;
                }
            }
        }

        inline bool MarketDataRouterState::is_shutdown_complete() const noexcept {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_shutdown_complete;
        }

        inline void MarketDataRouterState::shutdown() noexcept {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_shutdown_complete) return;
                if (m_shutdown) return;
                m_shutdown = true;
                for (const auto& [id, entry] : m_entries) {
                    (void)id;
                    set_control_released(entry->control);
                    entry->subscriber.reset();
                    entry->release_callback = {};
                    if (entry->phase == EntryPhase::CLEANUP_FAILED) {
                        entry->phase = entry->retained_cleanup_subscription.valid()
                            ? EntryPhase::PENDING
                            : EntryPhase::ACTIVE;
                    }
                }
            }

            process();
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

    inline void MarketDataRouter::process() {
        if (m_state) m_state->process();
    }

    inline bool MarketDataRouter::is_shutdown_complete() const noexcept {
        return !m_state || m_state->is_shutdown_complete();
    }

    inline void MarketDataRouter::shutdown() noexcept {
        if (m_state) m_state->shutdown();
    }


} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_DETAIL_MARKET_DATA_ROUTER_IPP_INCLUDED
