#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_HUB_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_HUB_HPP_INCLUDED

/// \file MarketDataHub.hpp
/// \brief Defines subscriber fan-out utilities for market-data providers.

namespace optionx::market_data {

    /// \struct MarketDataSubscriberOptions
    /// \brief Options used when adding a subscriber to MarketDataHub.
    struct MarketDataSubscriberOptions {
        bool replay_last_status = true; ///< Replay cached stream statuses to the new subscriber.
    };

    /// \class MarketDataHub
    /// \brief Fan-out adapter from provider callbacks to many subscribers.
    /// \details The hub is intentionally thin: it does not own provider
    ///          subscriptions and does not replay ticks or bars. It stores
    ///          subscriber slots as weak references, routes live batches to
    ///          current subscribers, and caches the last status per stream key
    ///          so late subscriber objects can learn current stream readiness.
    ///
    ///          Status replay is stream-level and happens when a subscriber
    ///          slot is added to the hub. It is not a per-subscription status
    ///          API; MarketDataRouter adds concrete subscription ownership and
    ///          replays status for newly created subscription handles.
    ///
    ///          When MarketDataStatusUpdate::subscription is valid, status
    ///          caching keeps that subscription context distinct from other
    ///          subscriptions that may share the same physical stream.
    ///
    ///          The hub synchronizes subscriber and status-cache containers and
    ///          never invokes subscriber callbacks while holding its mutex. For
    ///          deterministic replay/live ordering, marshal add/publish calls
    ///          through the provider owner loop, such as platform process().
    ///          Concurrent add/publish calls may interleave callback delivery.
    class MarketDataHub {
    public:
        /// \brief Runtime identifier of a subscriber slot in the hub.
        using SubscriberId = std::uint64_t;

        /// \brief Invalid subscriber slot identifier.
        static constexpr SubscriberId INVALID_SUBSCRIBER_ID = 0;

        /// \brief Adds a weak subscriber and optionally replays cached status updates.
        /// \param subscriber Subscriber object. The hub stores this weak reference only.
        /// \param options Subscriber delivery options.
        /// \return Non-zero subscriber ID, or INVALID_SUBSCRIBER_ID for null/expired input.
        SubscriberId add_weak_subscriber(
                std::weak_ptr<IMarketDataSubscriber> subscriber,
                MarketDataSubscriberOptions options = {});

        /// \brief Adds a subscriber and optionally replays cached status updates.
        /// \details The hub still stores only a weak reference; the caller must
        ///          keep the shared subscriber alive while it should receive
        ///          market-data callbacks.
        /// \param subscriber Subscriber object.
        /// \param options Subscriber delivery options.
        /// \return Non-zero subscriber ID, or INVALID_SUBSCRIBER_ID for null input.
        SubscriberId add_subscriber(
                const std::shared_ptr<IMarketDataSubscriber>& subscriber,
                MarketDataSubscriberOptions options = {}) {
            return add_weak_subscriber(std::weak_ptr<IMarketDataSubscriber>(subscriber), options);
        }

        /// \brief Removes a subscriber by ID.
        /// \param id Subscriber ID returned by add_subscriber().
        /// \return True if a slot was removed.
        bool remove_subscriber(SubscriberId id);

        /// \brief Removes all subscriber slots.
        void clear_subscribers();

        /// \brief Returns number of live subscriber slots after pruning expired ones.
        [[nodiscard]] std::size_t subscriber_count() const;

        /// \brief Binds provider callbacks to this hub.
        /// \details The provider must not outlive the hub unless unbind_from()
        ///          is called first; callbacks capture this hub by pointer.
        /// \param provider Provider whose callbacks should route into the hub.
        void bind_to(BaseMarketDataProvider& provider);

        /// \brief Clears provider callbacks previously bound to a hub.
        /// \param provider Provider whose callbacks should be reset.
        void unbind_from(BaseMarketDataProvider& provider) const;

        /// \brief Routes a tick batch to current subscribers.
        /// \param batch Tick batch owned by the provider callback.
        void publish_ticks(std::unique_ptr<TickDataBatch> batch);

        /// \brief Routes a bar batch to current subscribers.
        /// \param batch Bar batch owned by the provider callback.
        void publish_bars(std::unique_ptr<BarDataBatch> batch);

        /// \brief Caches and routes a status update to current subscribers.
        /// \param update Stream status update.
        void publish_status(MarketDataStatusUpdate update);

    private:
        struct SubscriberSlot {
            SubscriberId id = INVALID_SUBSCRIBER_ID;
            std::weak_ptr<IMarketDataSubscriber> subscriber;
            MarketDataSubscriberOptions options;
        };

        mutable std::mutex m_mutex; ///< Protects subscribers and status cache.
        mutable std::vector<SubscriberSlot> m_subscribers; ///< Registered subscribers.
        std::vector<MarketDataStatusUpdate> m_last_statuses; ///< Last status per stream key.
        SubscriberId m_next_subscriber_id = 1; ///< Next subscriber ID.

        /// \brief Returns true when two status updates describe the same stream.
        static bool same_status_key(
                const MarketDataStatusUpdate& lhs,
                const MarketDataStatusUpdate& rhs) noexcept;

        /// \brief Returns current live subscribers and prunes expired slots.
        std::vector<std::shared_ptr<IMarketDataSubscriber>> live_subscribers();

        /// \brief Stores a status update in the last-status cache.
        void cache_status_no_lock(MarketDataStatusUpdate update);
    };

    inline MarketDataHub::SubscriberId MarketDataHub::add_weak_subscriber(
            std::weak_ptr<IMarketDataSubscriber> subscriber,
            MarketDataSubscriberOptions options) {
        auto live = subscriber.lock();
        if (!live) return INVALID_SUBSCRIBER_ID;

        SubscriberId id = INVALID_SUBSCRIBER_ID;
        std::vector<MarketDataStatusUpdate> replay;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            id = m_next_subscriber_id++;
            if (id == INVALID_SUBSCRIBER_ID) {
                id = m_next_subscriber_id++;
            }

            m_subscribers.push_back(SubscriberSlot{id, std::move(subscriber), options});
            if (options.replay_last_status) {
                replay = m_last_statuses;
            }
        }

        for (const auto& update : replay) {
            live->on_market_data_status(update);
        }
        return id;
    }

    inline bool MarketDataHub::remove_subscriber(SubscriberId id) {
        if (id == INVALID_SUBSCRIBER_ID) return false;

        std::lock_guard<std::mutex> lock(m_mutex);
        const auto old_size = m_subscribers.size();
        m_subscribers.erase(
            std::remove_if(
                m_subscribers.begin(),
                m_subscribers.end(),
                [id](const SubscriberSlot& slot) {
                    return slot.id == id;
                }),
            m_subscribers.end());
        return m_subscribers.size() != old_size;
    }

    inline void MarketDataHub::clear_subscribers() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribers.clear();
    }

    inline std::size_t MarketDataHub::subscriber_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribers.erase(
            std::remove_if(
                m_subscribers.begin(),
                m_subscribers.end(),
                [](const SubscriberSlot& slot) {
                    return slot.subscriber.expired();
                }),
            m_subscribers.end());
        return m_subscribers.size();
    }

    inline void MarketDataHub::bind_to(BaseMarketDataProvider& provider) {
        provider.on_tick_data() =
            [this](std::unique_ptr<TickDataBatch> batch) {
                publish_ticks(std::move(batch));
            };
        provider.on_bar_data() =
            [this](std::unique_ptr<BarDataBatch> batch) {
                publish_bars(std::move(batch));
            };
        provider.on_market_data_status() =
            [this](MarketDataStatusUpdate update) {
                publish_status(std::move(update));
            };
    }

    inline void MarketDataHub::unbind_from(BaseMarketDataProvider& provider) const {
        provider.on_tick_data() = BaseMarketDataProvider::ticks_callback_t{};
        provider.on_bar_data() = BaseMarketDataProvider::bars_callback_t{};
        provider.on_market_data_status() = BaseMarketDataProvider::status_callback_t{};
    }

    inline void MarketDataHub::publish_ticks(std::unique_ptr<TickDataBatch> batch) {
        if (!batch) return;
        for (auto& tick : batch->items) {
            mark_live_payload(tick.flags);
        }
        const auto subscribers = live_subscribers();
        for (const auto& subscriber : subscribers) {
            subscriber->on_tick_data(*batch);
        }
    }

    inline void MarketDataHub::publish_bars(std::unique_ptr<BarDataBatch> batch) {
        if (!batch) return;
        for (auto& bar : batch->items) {
            mark_live_payload(bar.flags);
        }
        const auto subscribers = live_subscribers();
        for (const auto& subscriber : subscribers) {
            subscriber->on_bar_data(*batch);
        }
    }

    inline void MarketDataHub::publish_status(MarketDataStatusUpdate update) {
        auto subscribers = std::vector<std::shared_ptr<IMarketDataSubscriber>>{};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cache_status_no_lock(update);
            subscribers.reserve(m_subscribers.size());
            m_subscribers.erase(
                std::remove_if(
                    m_subscribers.begin(),
                    m_subscribers.end(),
                    [&subscribers](const SubscriberSlot& slot) {
                        auto subscriber = slot.subscriber.lock();
                        if (!subscriber) return true;
                        subscribers.push_back(std::move(subscriber));
                        return false;
                    }),
                m_subscribers.end());
        }

        for (const auto& subscriber : subscribers) {
            subscriber->on_market_data_status(update);
        }
    }

    inline bool MarketDataHub::same_status_key(
            const MarketDataStatusUpdate& lhs,
            const MarketDataStatusUpdate& rhs) noexcept {
        if (lhs.subscription.valid() || rhs.subscription.valid()) {
            return lhs.subscription.valid() &&
                   rhs.subscription.valid() &&
                   lhs.subscription.provider_id == rhs.subscription.provider_id &&
                   lhs.subscription.id == rhs.subscription.id;
        }

        return lhs.provider_id == rhs.provider_id &&
               lhs.type == rhs.type &&
               lhs.symbol == rhs.symbol &&
               lhs.timeframe == rhs.timeframe &&
               lhs.transport == rhs.transport;
    }

    inline std::vector<std::shared_ptr<IMarketDataSubscriber>>
    MarketDataHub::live_subscribers() {
        std::vector<std::shared_ptr<IMarketDataSubscriber>> subscribers;
        std::lock_guard<std::mutex> lock(m_mutex);
        subscribers.reserve(m_subscribers.size());
        m_subscribers.erase(
            std::remove_if(
                m_subscribers.begin(),
                m_subscribers.end(),
                [&subscribers](const SubscriberSlot& slot) {
                    auto subscriber = slot.subscriber.lock();
                    if (!subscriber) return true;
                    subscribers.push_back(std::move(subscriber));
                    return false;
                }),
            m_subscribers.end());
        return subscribers;
    }

    inline void MarketDataHub::cache_status_no_lock(MarketDataStatusUpdate update) {
        for (auto& cached : m_last_statuses) {
            if (same_status_key(cached, update)) {
                cached = std::move(update);
                return;
            }
        }
        m_last_statuses.push_back(std::move(update));
    }

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_HUB_HPP_INCLUDED
