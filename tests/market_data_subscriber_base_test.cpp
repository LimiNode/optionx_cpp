#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <optionx_cpp/market_data.hpp>
#include <optionx_cpp/platforms.hpp>

using namespace optionx;
using namespace optionx::market_data;

namespace {

class ManualOwnerLoop {
public:
    bool post(MarketDataRouter::owner_task_t task) {
        if (!task) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_accepting) return false;
        m_tasks.push_back(std::move(task));
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accepting = false;
    }

    void process_all() {
        while (true) {
            MarketDataRouter::owner_task_t task;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_tasks.empty()) return;
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            task();
        }
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tasks.size();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<MarketDataRouter::owner_task_t> m_tasks;
    bool m_accepting = true;
};

class TestOwnerPlatform final : public platforms::BaseTradingPlatform {
public:
    TestOwnerPlatform()
        : platforms::BaseTradingPlatform(
              std::make_shared<platforms::intrade_bar::AccountInfoData>()) {}

    PlatformType platform_type() const override {
        return PlatformType::INTRADE_BAR;
    }
};

class FakeMarketDataProvider final : public BaseMarketDataProvider {
public:
    bars_callback_t bars_callback;
    ticks_callback_t ticks_callback;
    status_callback_t status_callback;
    bool defer_ticks = false;
    bool defer_bars = false;
    std::size_t unsubscribe_calls = 0;
    std::vector<MarketDataSubscriptionHandle> active_subscriptions;
    std::thread::id subscribe_thread;
    std::thread::id unsubscribe_thread;

    bars_callback_t& on_bar_data() override {
        return bars_callback;
    }

    ticks_callback_t& on_tick_data() override {
        return ticks_callback;
    }

    status_callback_t& on_market_data_status() override {
        return status_callback;
    }

    bool subscribe_ticks(
            TickSubscriptionRequest request,
            subscription_callback_t callback) override {
        subscribe_thread = std::this_thread::get_id();
        auto subscription = MarketDataSubscriptionHandle::from_tick_request(
            provider_id(),
            m_next_subscription_id++,
            request);
        if (defer_ticks) {
            m_pending_tick_subscription = std::move(subscription);
            m_pending_tick_callback = std::move(callback);
            return true;
        }
        active_subscriptions.push_back(subscription);
        if (callback) {
            callback(MarketDataSubscriptionResult::subscribed(
                std::move(subscription)));
        }
        return true;
    }

    bool subscribe_bars(
            BarSubscriptionRequest request,
            subscription_callback_t callback) override {
        subscribe_thread = std::this_thread::get_id();
        auto subscription = MarketDataSubscriptionHandle::from_bar_request(
            provider_id(),
            m_next_subscription_id++,
            request);
        if (defer_bars) {
            m_pending_bar_subscription = std::move(subscription);
            m_pending_bar_callback = std::move(callback);
            return true;
        }
        active_subscriptions.push_back(subscription);
        if (callback) {
            callback(MarketDataSubscriptionResult::subscribed(
                std::move(subscription)));
        }
        return true;
    }

    bool unsubscribe(
            MarketDataSubscriptionHandle subscription,
            subscription_callback_t callback) override {
        unsubscribe_thread = std::this_thread::get_id();
        ++unsubscribe_calls;
        const auto active = std::find_if(
            active_subscriptions.begin(),
            active_subscriptions.end(),
            [&subscription](const MarketDataSubscriptionHandle& candidate) {
                return candidate.provider_id == subscription.provider_id &&
                       candidate.id == subscription.id;
            });
        if (active != active_subscriptions.end()) {
            active_subscriptions.erase(active);
        }
        if (callback) {
            callback(MarketDataSubscriptionResult::unsubscribed(
                std::move(subscription)));
        }
        return true;
    }

    void complete_pending_tick() {
        ASSERT_TRUE(static_cast<bool>(m_pending_tick_callback));
        active_subscriptions.push_back(m_pending_tick_subscription);
        auto callback = std::move(m_pending_tick_callback);
        callback(MarketDataSubscriptionResult::subscribed(
            std::move(m_pending_tick_subscription)));
    }

    void complete_pending_bar() {
        ASSERT_TRUE(static_cast<bool>(m_pending_bar_callback));
        active_subscriptions.push_back(m_pending_bar_subscription);
        auto callback = std::move(m_pending_bar_callback);
        callback(MarketDataSubscriptionResult::subscribed(
            std::move(m_pending_bar_subscription)));
    }

    void fail_pending_tick() {
        ASSERT_TRUE(static_cast<bool>(m_pending_tick_callback));
        auto callback = std::move(m_pending_tick_callback);
        callback(MarketDataSubscriptionResult::failed(
            std::move(m_pending_tick_subscription),
            MarketDataSubscriptionStatus::FAILED,
            "Deferred test failure."));
    }

    void emit_tick(const MarketDataSubscriptionHandle& subscription) {
        ASSERT_TRUE(static_cast<bool>(ticks_callback));
        auto batch = std::make_unique<TickDataBatch>();
        batch->subscription = subscription;
        batch->type = MarketDataType::TICKS;
        batch->symbol = subscription.symbol;
        batch->price_digits = 5;
        batch->items.emplace_back(1.10000, 1.10002, 0.0, 1.0, 1000, 0, 0);
        ticks_callback(std::move(batch));
    }

    void emit_ready(std::string symbol) {
        ASSERT_TRUE(static_cast<bool>(status_callback));
        MarketDataStatusUpdate update;
        update.provider_id = provider_id();
        update.type = MarketDataType::TICKS;
        update.symbol = std::move(symbol);
        update.transport = MarketDataTransport::WEBSOCKET;
        update.status = MarketDataStreamStatus::READY;
        status_callback(std::move(update));
    }

private:
    SubscriptionId m_next_subscription_id = 1;
    MarketDataSubscriptionHandle m_pending_tick_subscription;
    MarketDataSubscriptionHandle m_pending_bar_subscription;
    subscription_callback_t m_pending_tick_callback;
    subscription_callback_t m_pending_bar_callback;
};

class TestBot final : public MarketDataSubscriberBase {
public:
    explicit TestBot(MarketDataRouter& router)
            : MarketDataSubscriberBase(router) {}

    std::vector<TickDataBatch> ticks;
    std::vector<MarketDataStatusUpdate> statuses;
    std::thread::id tick_thread;
    std::thread::id status_thread;

    RoutedSubscriptionId start_ticks(
            BaseMarketDataProvider& provider,
            std::string symbol,
            subscription_callback_t callback = {}) {
        return subscribe_ticks(
            provider,
            TickSubscriptionRequest(
                std::move(symbol),
                MarketDataTransport::WEBSOCKET),
            std::move(callback));
    }

    RoutedSubscriptionId start_ticks(
            MarketDataProviderId provider_id,
            std::string symbol,
            subscription_callback_t callback = {}) {
        return subscribe_ticks(
            provider_id,
            TickSubscriptionRequest(
                std::move(symbol),
                MarketDataTransport::WEBSOCKET),
            std::move(callback));
    }

    RoutedSubscriptionId start_ticks(
            std::string_view provider_alias,
            std::string symbol,
            subscription_callback_t callback = {}) {
        return subscribe_ticks(
            provider_alias,
            TickSubscriptionRequest(
                std::move(symbol),
                MarketDataTransport::WEBSOCKET),
            std::move(callback));
    }

    RoutedSubscriptionId start_bars(
            BaseMarketDataProvider& provider,
            std::string symbol,
            BarTimeframe timeframe,
            subscription_callback_t callback = {}) {
        return subscribe_bars(
            provider,
            BarSubscriptionRequest(
                std::move(symbol),
                timeframe,
                BarPriceSource::MID,
                MarketDataTransport::WEBSOCKET),
            std::move(callback));
    }

    RoutedSubscriptionId start_bars(
            MarketDataProviderId provider_id,
            std::string symbol,
            BarTimeframe timeframe) {
        return subscribe_bars(
            provider_id,
            BarSubscriptionRequest(
                std::move(symbol),
                timeframe,
                BarPriceSource::MID,
                MarketDataTransport::WEBSOCKET));
    }

    RoutedSubscriptionId start_bars(
            std::string_view provider_alias,
            std::string symbol,
            BarTimeframe timeframe) {
        return subscribe_bars(
            provider_alias,
            BarSubscriptionRequest(
                std::move(symbol),
                timeframe,
                BarPriceSource::MID,
                MarketDataTransport::WEBSOCKET));
    }

    bool request_ticks(
            MarketDataProviderId provider_id,
            std::string symbol,
            route_callback_t route_callback = {},
            subscription_callback_t callback = {}) {
        return post_subscribe_ticks(
            provider_id,
            TickSubscriptionRequest(
                std::move(symbol),
                MarketDataTransport::WEBSOCKET),
            std::move(route_callback),
            std::move(callback));
    }

    bool request_ticks(
            std::string_view provider_alias,
            std::string symbol,
            route_callback_t route_callback = {},
            subscription_callback_t callback = {}) {
        return post_subscribe_ticks(
            provider_alias,
            TickSubscriptionRequest(
                std::move(symbol),
                MarketDataTransport::WEBSOCKET),
            std::move(route_callback),
            std::move(callback));
    }

    bool request_bars(
            MarketDataProviderId provider_id,
            std::string symbol,
            BarTimeframe timeframe,
            route_callback_t route_callback = {},
            subscription_callback_t callback = {}) {
        return post_subscribe_bars(
            provider_id,
            BarSubscriptionRequest(
                std::move(symbol),
                timeframe,
                BarPriceSource::MID,
                MarketDataTransport::WEBSOCKET),
            std::move(route_callback),
            std::move(callback));
    }

    bool request_bars(
            std::string_view provider_alias,
            std::string symbol,
            BarTimeframe timeframe,
            route_callback_t route_callback = {},
            subscription_callback_t callback = {}) {
        return post_subscribe_bars(
            provider_alias,
            BarSubscriptionRequest(
                std::move(symbol),
                timeframe,
                BarPriceSource::MID,
                MarketDataTransport::WEBSOCKET),
            std::move(route_callback),
            std::move(callback));
    }

    bool request_stop(
            RoutedSubscriptionId router_id,
            subscription_callback_t callback = {}) {
        return post_unsubscribe(router_id, std::move(callback));
    }

    bool request_stop_all() {
        return post_unsubscribe_all();
    }

    bool stop(
            RoutedSubscriptionId router_id,
            subscription_callback_t callback = {}) {
        return unsubscribe(router_id, std::move(callback));
    }

    void stop_all() {
        unsubscribe_all();
    }

    std::size_t route_count() const {
        return subscription_count();
    }

    bool owns(RoutedSubscriptionId router_id) const {
        return has_subscription(router_id);
    }

    MarketDataSubscriptionHandle subscription(RoutedSubscriptionId router_id) const {
        return provider_subscription(router_id);
    }

    void on_tick_data(const TickDataBatch& batch) override {
        tick_thread = std::this_thread::get_id();
        ticks.push_back(batch);
    }

    void on_market_data_status(const MarketDataStatusUpdate& update) override {
        status_thread = std::this_thread::get_id();
        statuses.push_back(update);
    }
};

} // namespace

TEST(MarketDataSubscriberBase, SubscribesFromBotMethodsAndOwnsRouteLifetime) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto bot = std::make_shared<TestBot>(router);

    const auto eur = bot->start_ticks(provider, "EURUSD");
    ASSERT_TRUE(eur.valid());
    ASSERT_TRUE(bot->owns(eur));
    ASSERT_EQ(bot->route_count(), 1u);
    const auto subscription = bot->subscription(eur);
    ASSERT_TRUE(subscription.valid());

    provider.emit_tick(subscription);
    ASSERT_EQ(bot->ticks.size(), 1u);
    EXPECT_EQ(bot->ticks.front().subscription.id, subscription.id);

    bot.reset();
    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    EXPECT_EQ(router.subscription_count(), 0u);
}

TEST(MarketDataSubscriberBase, SubscribesByStableProviderIdAndAliasFromBotMethods) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    const MarketDataProviderId provider_id{1001};
    ASSERT_TRUE(router.register_provider(provider_id, provider, {"demo"}));
    auto bot = std::make_shared<TestBot>(router);

    const auto tick_by_id = bot->start_ticks(provider_id, "EURUSD");
    const auto tick_by_alias = bot->start_ticks("demo", "BTCUSDT");
    const auto bar_by_id = bot->start_bars(provider_id, "EURUSD", 60);
    const auto bar_by_alias = bot->start_bars("demo", "BTCUSDT", 300);

    EXPECT_TRUE(tick_by_id.valid());
    EXPECT_TRUE(tick_by_alias.valid());
    EXPECT_TRUE(bar_by_id.valid());
    EXPECT_TRUE(bar_by_alias.valid());
    EXPECT_EQ(bot->route_count(), 4u);
    EXPECT_EQ(router.registered_provider_id(provider.provider_id()), provider_id);

    bot->stop_all();
    EXPECT_EQ(provider.unsubscribe_calls, 4u);
    EXPECT_EQ(router.subscription_count(), 0u);
}

TEST(MarketDataSubscriberBase, MarshalsBotThreadCommandsAndEventsToOwnerLoop) {
    FakeMarketDataProvider provider;
    ManualOwnerLoop owner_loop;
    MarketDataRouter router(
        [&owner_loop](MarketDataRouter::owner_task_t task) {
            return owner_loop.post(std::move(task));
        });
    const MarketDataProviderId provider_id{1001};
    ASSERT_TRUE(router.register_provider(provider_id, provider, {"demo"}));
    ASSERT_TRUE(router.has_owner_dispatcher());
    auto bot = std::make_shared<TestBot>(router);

    RoutedSubscriptionId route;
    MarketDataSubscriptionStatus subscribe_status =
        MarketDataSubscriptionStatus::UNKNOWN;
    std::thread::id route_callback_thread;
    std::thread::id subscribe_callback_thread;
    bool subscribe_command_accepted = false;
    std::thread bot_thread([&]() {
        subscribe_command_accepted = bot->request_ticks(
            "demo",
            "EURUSD",
            [&](RoutedSubscriptionId update) {
                route = update;
                route_callback_thread = std::this_thread::get_id();
            },
            [&](MarketDataSubscriptionResult result) {
                subscribe_status = result.status;
                subscribe_callback_thread = std::this_thread::get_id();
            });
    });
    bot_thread.join();

    EXPECT_TRUE(subscribe_command_accepted);
    EXPECT_FALSE(route.valid());
    EXPECT_EQ(provider.subscribe_thread, std::thread::id{});
    EXPECT_EQ(owner_loop.size(), 1u);

    const auto owner_thread = std::this_thread::get_id();
    owner_loop.process_all();

    ASSERT_TRUE(route.valid());
    EXPECT_TRUE(bot->owns(route));
    EXPECT_EQ(provider.subscribe_thread, owner_thread);
    EXPECT_EQ(route_callback_thread, owner_thread);
    EXPECT_EQ(subscribe_callback_thread, owner_thread);
    EXPECT_EQ(subscribe_status, MarketDataSubscriptionStatus::SUBSCRIBED);

    const auto subscription = bot->subscription(route);
    std::thread source_thread([&]() {
        provider.emit_tick(subscription);
        provider.emit_ready("EURUSD");
    });
    source_thread.join();

    EXPECT_TRUE(bot->ticks.empty());
    EXPECT_TRUE(bot->statuses.empty());
    owner_loop.process_all();

    ASSERT_EQ(bot->ticks.size(), 1u);
    ASSERT_EQ(bot->statuses.size(), 1u);
    EXPECT_EQ(bot->tick_thread, owner_thread);
    EXPECT_EQ(bot->status_thread, owner_thread);

    MarketDataSubscriptionStatus unsubscribe_status =
        MarketDataSubscriptionStatus::UNKNOWN;
    std::thread::id unsubscribe_callback_thread;
    bool unsubscribe_command_accepted = false;
    std::thread stop_thread([&]() {
        unsubscribe_command_accepted = bot->request_stop(
            route,
            [&](MarketDataSubscriptionResult result) {
                unsubscribe_status = result.status;
                unsubscribe_callback_thread = std::this_thread::get_id();
            });
    });
    stop_thread.join();

    EXPECT_TRUE(unsubscribe_command_accepted);
    EXPECT_FALSE(bot->owns(route));
    EXPECT_EQ(provider.unsubscribe_calls, 0u);
    owner_loop.process_all();

    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    EXPECT_EQ(provider.unsubscribe_thread, owner_thread);
    EXPECT_EQ(unsubscribe_callback_thread, owner_thread);
    EXPECT_EQ(unsubscribe_status, MarketDataSubscriptionStatus::UNSUBSCRIBED);
    EXPECT_EQ(router.subscription_count(), 0u);
}

TEST(MarketDataSubscriberBase, PostsBarCommandsByStableIdAndAlias) {
    FakeMarketDataProvider provider;
    ManualOwnerLoop owner_loop;
    MarketDataRouter router(
        [&owner_loop](MarketDataRouter::owner_task_t task) {
            return owner_loop.post(std::move(task));
        });
    const MarketDataProviderId provider_id{1001};
    ASSERT_TRUE(router.register_provider(provider_id, provider, {"demo"}));
    auto bot = std::make_shared<TestBot>(router);
    RoutedSubscriptionId by_id;
    RoutedSubscriptionId by_alias;
    bool commands_accepted = false;

    std::thread bot_thread([&]() {
        const bool first = bot->request_bars(
            provider_id,
            "EURUSD",
            60,
            [&](RoutedSubscriptionId route) {
                by_id = route;
            });
        const bool second = bot->request_bars(
            "demo",
            "BTCUSDT",
            300,
            [&](RoutedSubscriptionId route) {
                by_alias = route;
            });
        commands_accepted = first && second;
    });
    bot_thread.join();

    EXPECT_TRUE(commands_accepted);
    EXPECT_FALSE(by_id.valid());
    EXPECT_FALSE(by_alias.valid());
    owner_loop.process_all();

    EXPECT_TRUE(by_id.valid());
    EXPECT_TRUE(by_alias.valid());
    EXPECT_EQ(bot->route_count(), 2u);

    bool stop_accepted = false;
    bot_thread = std::thread([&]() {
        stop_accepted = bot->request_stop_all();
    });
    bot_thread.join();
    EXPECT_TRUE(stop_accepted);
    EXPECT_EQ(provider.unsubscribe_calls, 0u);

    owner_loop.process_all();
    EXPECT_EQ(provider.unsubscribe_calls, 2u);
    EXPECT_EQ(router.subscription_count(), 0u);
}

TEST(MarketDataSubscriberBase, DestructionPostsHandleCleanupToOwnerLoop) {
    FakeMarketDataProvider provider;
    ManualOwnerLoop owner_loop;
    MarketDataRouter router(
        [&owner_loop](MarketDataRouter::owner_task_t task) {
            return owner_loop.post(std::move(task));
        });
    const MarketDataProviderId provider_id{1001};
    ASSERT_TRUE(router.register_provider(provider_id, provider));
    auto bot = std::make_shared<TestBot>(router);

    RoutedSubscriptionId route;
    ASSERT_TRUE(bot->request_ticks(
        provider_id,
        "BTCUSDT",
        [&](RoutedSubscriptionId update) {
            route = update;
        }));
    owner_loop.process_all();
    ASSERT_TRUE(route.valid());

    std::thread bot_thread([bot = std::move(bot)]() mutable {
        bot.reset();
    });
    bot_thread.join();

    EXPECT_EQ(provider.unsubscribe_calls, 0u);
    EXPECT_EQ(owner_loop.size(), 1u);

    const auto owner_thread = std::this_thread::get_id();
    owner_loop.process_all();

    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    EXPECT_EQ(provider.unsubscribe_thread, owner_thread);
    EXPECT_EQ(router.subscription_count(), 0u);
}

TEST(MarketDataSubscriberBase, DoesNotDeliverEventsInlineWhenOwnerLoopRejectsWork) {
    FakeMarketDataProvider provider;
    ManualOwnerLoop owner_loop;
    MarketDataRouter router(
        [&owner_loop](MarketDataRouter::owner_task_t task) {
            return owner_loop.post(std::move(task));
        });
    const MarketDataProviderId provider_id{1001};
    ASSERT_TRUE(router.register_provider(provider_id, provider, {"demo"}));
    auto bot = std::make_shared<TestBot>(router);

    RoutedSubscriptionId route;
    ASSERT_TRUE(bot->request_ticks(
        "demo",
        "EURUSD",
        [&route](RoutedSubscriptionId update) {
            route = update;
        }));
    owner_loop.process_all();
    ASSERT_TRUE(route.valid());
    ASSERT_EQ(bot->route_count(), 1u);
    const auto subscription = bot->subscription(route);
    ASSERT_TRUE(subscription.valid());

    owner_loop.close();
    std::thread source_thread([&]() {
        provider.emit_ready("EURUSD");
        provider.emit_tick(subscription);
    });
    source_thread.join();

    EXPECT_TRUE(bot->statuses.empty());
    EXPECT_TRUE(bot->ticks.empty());

    router.shutdown();
}

TEST(MarketDataSubscriberBase, RetainsRejectedSubscribeCompletionsForShutdownCleanup) {
    FakeMarketDataProvider provider;
    provider.defer_ticks = true;
    provider.defer_bars = true;
    ManualOwnerLoop owner_loop;
    MarketDataRouter router(
        [&owner_loop](MarketDataRouter::owner_task_t task) {
            return owner_loop.post(std::move(task));
        });
    auto bot = std::make_shared<TestBot>(router);
    std::size_t completion_callbacks = 0;

    const auto ticks = bot->start_ticks(
        provider,
        "EURUSD",
        [&completion_callbacks](MarketDataSubscriptionResult) {
            ++completion_callbacks;
        });
    const auto bars = bot->start_bars(
        provider,
        "BTCUSDT",
        60,
        [&completion_callbacks](MarketDataSubscriptionResult) {
            ++completion_callbacks;
        });

    ASSERT_TRUE(ticks.valid());
    ASSERT_TRUE(bars.valid());
    ASSERT_EQ(router.subscription_count(), 2u);
    ASSERT_TRUE(provider.active_subscriptions.empty());

    owner_loop.close();
    std::thread source_thread([&]() {
        provider.complete_pending_tick();
        provider.complete_pending_bar();
    });
    source_thread.join();

    EXPECT_EQ(completion_callbacks, 0u);
    EXPECT_EQ(owner_loop.size(), 0u);
    EXPECT_EQ(provider.active_subscriptions.size(), 2u);
    EXPECT_FALSE(bot->subscription(ticks).valid());
    EXPECT_FALSE(bot->subscription(bars).valid());

    MarketDataSubscriptionStatus quarantined_status =
        MarketDataSubscriptionStatus::UNKNOWN;
    const auto quarantined = bot->start_ticks(
        provider,
        "GBPUSD",
        [&quarantined_status](MarketDataSubscriptionResult result) {
            quarantined_status = result.status;
        });
    EXPECT_FALSE(quarantined.valid());
    EXPECT_EQ(quarantined_status, MarketDataSubscriptionStatus::FAILED);
    EXPECT_EQ(router.subscription_count(), 2u);
    EXPECT_EQ(provider.active_subscriptions.size(), 2u);

    const auto owner_thread = std::this_thread::get_id();
    router.shutdown();

    EXPECT_EQ(provider.unsubscribe_calls, 2u);
    EXPECT_TRUE(provider.active_subscriptions.empty());
    EXPECT_EQ(provider.unsubscribe_thread, owner_thread);
    EXPECT_EQ(completion_callbacks, 0u);
}

TEST(MarketDataSubscriberBase, RetainsAcceptedCompletionCancelledByPlatformShutdown) {
    FakeMarketDataProvider provider;
    provider.defer_ticks = true;
    TestOwnerPlatform platform;
    MarketDataRouter router(
        [&platform](MarketDataRouter::owner_task_t task) {
            return platform.post_task(std::move(task));
        });
    auto bot = std::make_shared<TestBot>(router);
    std::size_t completion_callbacks = 0;

    const auto route = bot->start_ticks(
        provider,
        "EURUSD",
        [&completion_callbacks](MarketDataSubscriptionResult) {
            ++completion_callbacks;
        });
    ASSERT_TRUE(route.valid());
    ASSERT_EQ(router.subscription_count(), 1u);

    std::thread source_thread([&]() {
        provider.complete_pending_tick();
    });
    source_thread.join();

    ASSERT_EQ(provider.active_subscriptions.size(), 1u);
    EXPECT_EQ(completion_callbacks, 0u);
    EXPECT_FALSE(bot->subscription(route).valid());

    const auto owner_thread = std::this_thread::get_id();
    router.shutdown();

    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    EXPECT_TRUE(provider.active_subscriptions.empty());
    EXPECT_EQ(provider.unsubscribe_thread, owner_thread);

    platform.shutdown();

    EXPECT_EQ(completion_callbacks, 0u);
    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    EXPECT_TRUE(bot->ticks.empty());
    EXPECT_TRUE(bot->statuses.empty());
}

TEST(MarketDataSubscriberBase, RejectsPostedCommandsWithoutAnOwnerDispatcher) {
    MarketDataRouter router;
    auto bot = std::make_shared<TestBot>(router);
    RoutedSubscriptionId route;

    EXPECT_FALSE(bot->request_ticks(
        MarketDataProviderId{1001},
        "EURUSD",
        [&](RoutedSubscriptionId update) {
            route = update;
        }));
    EXPECT_FALSE(route.valid());
    EXPECT_EQ(bot->route_count(), 0u);
}

TEST(MarketDataSubscriberBase, ExplicitStopAndStopAllReleaseStoredHandles) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto bot = std::make_shared<TestBot>(router);
    const auto ticks = bot->start_ticks(provider, "EURUSD");
    const auto bars = bot->start_bars(provider, "EURUSD", 60);
    MarketDataSubscriptionStatus stop_status = MarketDataSubscriptionStatus::UNKNOWN;
    std::size_t count_during_callback = 0;

    ASSERT_TRUE(bot->stop(
        ticks,
        [&bot, &stop_status, &count_during_callback](
                MarketDataSubscriptionResult result) {
            stop_status = result.status;
            count_during_callback = bot->route_count();
        }));
    EXPECT_EQ(stop_status, MarketDataSubscriptionStatus::UNSUBSCRIBED);
    EXPECT_EQ(count_during_callback, 1u);
    EXPECT_FALSE(bot->owns(ticks));
    EXPECT_TRUE(bot->owns(bars));
    EXPECT_EQ(bot->route_count(), 1u);

    bot->stop_all();
    EXPECT_EQ(bot->route_count(), 0u);
    EXPECT_EQ(provider.unsubscribe_calls, 2u);
}

TEST(MarketDataSubscriberBase, RequiresSharedOwnershipBeforeSubscribing) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    TestBot bot(router);
    MarketDataSubscriptionResult result;

    const auto route = bot.start_ticks(
        provider,
        "EURUSD",
        [&result](MarketDataSubscriptionResult update) {
            result = std::move(update);
        });

    EXPECT_FALSE(route.valid());
    EXPECT_FALSE(result.success());
    EXPECT_NE(result.error_message.find("expired"), std::string::npos);
    EXPECT_EQ(bot.route_count(), 0u);
    EXPECT_EQ(router.subscription_count(), 0u);
}

TEST(MarketDataSubscriberBase, PrunesADeferredSubscriptionFailure) {
    FakeMarketDataProvider provider;
    provider.defer_ticks = true;
    MarketDataRouter router;
    auto bot = std::make_shared<TestBot>(router);
    MarketDataSubscriptionStatus status = MarketDataSubscriptionStatus::UNKNOWN;

    const auto route = bot->start_ticks(
        provider,
        "BTCUSDT",
        [&status](MarketDataSubscriptionResult result) {
            status = result.status;
        });
    ASSERT_TRUE(route.valid());
    EXPECT_EQ(bot->route_count(), 1u);

    provider.fail_pending_tick();

    EXPECT_EQ(status, MarketDataSubscriptionStatus::FAILED);
    EXPECT_EQ(bot->route_count(), 0u);
    EXPECT_EQ(router.subscription_count(), 0u);
}

TEST(MarketDataSubscriberBase, DestructionCancelsADeferredSubscription) {
    FakeMarketDataProvider provider;
    provider.defer_ticks = true;
    MarketDataRouter router;
    auto bot = std::make_shared<TestBot>(router);

    ASSERT_TRUE(bot->start_ticks(provider, "BTCUSDT").valid());
    bot.reset();
    EXPECT_EQ(provider.unsubscribe_calls, 0u);

    provider.complete_pending_tick();

    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    EXPECT_EQ(router.subscription_count(), 0u);
}

TEST(MarketDataSubscriberBase, ReceivesReplayForTheStoredConcreteRoute) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto bot = std::make_shared<TestBot>(router);
    const auto eur = bot->start_ticks(provider, "EURUSD");
    ASSERT_TRUE(eur.valid());

    provider.emit_ready("BTCUSDT");
    const auto btc = bot->start_ticks(provider, "BTCUSDT");

    ASSERT_TRUE(btc.valid());
    ASSERT_EQ(bot->statuses.size(), 1u);
    EXPECT_EQ(bot->statuses.front().subscription.id, bot->subscription(btc).id);
    EXPECT_NE(bot->statuses.front().subscription.id, bot->subscription(eur).id);
    EXPECT_EQ(bot->statuses.front().status, MarketDataStreamStatus::READY);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
