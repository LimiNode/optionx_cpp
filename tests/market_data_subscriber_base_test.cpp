#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include <optionx_cpp/market_data.hpp>

using namespace optionx;
using namespace optionx::market_data;

namespace {

class FakeMarketDataProvider final : public BaseMarketDataProvider {
public:
    bars_callback_t bars_callback;
    ticks_callback_t ticks_callback;
    status_callback_t status_callback;
    bool defer_ticks = false;
    std::size_t unsubscribe_calls = 0;

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
        auto subscription = MarketDataSubscriptionHandle::from_tick_request(
            provider_id(),
            m_next_subscription_id++,
            request);
        if (defer_ticks) {
            m_pending_subscription = std::move(subscription);
            m_pending_callback = std::move(callback);
            return true;
        }
        if (callback) {
            callback(MarketDataSubscriptionResult::subscribed(
                std::move(subscription)));
        }
        return true;
    }

    bool subscribe_bars(
            BarSubscriptionRequest request,
            subscription_callback_t callback) override {
        auto subscription = MarketDataSubscriptionHandle::from_bar_request(
            provider_id(),
            m_next_subscription_id++,
            request);
        if (callback) {
            callback(MarketDataSubscriptionResult::subscribed(
                std::move(subscription)));
        }
        return true;
    }

    bool unsubscribe(
            MarketDataSubscriptionHandle subscription,
            subscription_callback_t callback) override {
        ++unsubscribe_calls;
        if (callback) {
            callback(MarketDataSubscriptionResult::unsubscribed(
                std::move(subscription)));
        }
        return true;
    }

    void complete_pending_tick() {
        ASSERT_TRUE(static_cast<bool>(m_pending_callback));
        auto callback = std::move(m_pending_callback);
        callback(MarketDataSubscriptionResult::subscribed(
            std::move(m_pending_subscription)));
    }

    void fail_pending_tick() {
        ASSERT_TRUE(static_cast<bool>(m_pending_callback));
        auto callback = std::move(m_pending_callback);
        callback(MarketDataSubscriptionResult::failed(
            std::move(m_pending_subscription),
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
    MarketDataSubscriptionHandle m_pending_subscription;
    subscription_callback_t m_pending_callback;
};

class TestBot final : public MarketDataSubscriberBase {
public:
    explicit TestBot(MarketDataRouter& router)
            : MarketDataSubscriberBase(router) {}

    std::vector<TickDataBatch> ticks;
    std::vector<MarketDataStatusUpdate> statuses;

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
            BarTimeframe timeframe) {
        return subscribe_bars(
            provider,
            BarSubscriptionRequest(
                std::move(symbol),
                timeframe,
                BarPriceSource::MID,
                MarketDataTransport::WEBSOCKET));
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
        ticks.push_back(batch);
    }

    void on_market_data_status(const MarketDataStatusUpdate& update) override {
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
