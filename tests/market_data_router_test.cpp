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
    enum class UnsubscribeMode {
        SUCCEED,
        REJECT,
        COMPLETE_FAILED,
        DEFER
    };

    bars_callback_t bars_callback;
    ticks_callback_t ticks_callback;
    status_callback_t status_callback;
    bool defer_subscribe = false;
    UnsubscribeMode unsubscribe_mode = UnsubscribeMode::SUCCEED;
    std::size_t unsubscribe_calls = 0;
    std::vector<MarketDataSubscriptionHandle> active_subscriptions;

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
        auto handle = MarketDataSubscriptionHandle::from_tick_request(
            provider_id(),
            m_next_subscription_id++,
            request);
        return finish_or_defer(std::move(handle), std::move(callback));
    }

    bool subscribe_bars(
            BarSubscriptionRequest request,
            subscription_callback_t callback) override {
        auto handle = MarketDataSubscriptionHandle::from_bar_request(
            provider_id(),
            m_next_subscription_id++,
            request);
        return finish_or_defer(std::move(handle), std::move(callback));
    }

    bool unsubscribe(
            MarketDataSubscriptionHandle subscription,
            subscription_callback_t callback) override {
        ++unsubscribe_calls;
        if (unsubscribe_mode == UnsubscribeMode::REJECT) return false;
        if (unsubscribe_mode == UnsubscribeMode::COMPLETE_FAILED) {
            if (callback) {
                callback(MarketDataSubscriptionResult::failed(
                    std::move(subscription),
                    MarketDataSubscriptionStatus::FAILED,
                    "Simulated provider unsubscribe failure."));
            }
            return true;
        }
        if (unsubscribe_mode == UnsubscribeMode::DEFER) {
            m_pending_unsubscribe = std::move(subscription);
            m_pending_unsubscribe_callback = std::move(callback);
            return true;
        }

        active_subscriptions.erase(
            std::remove_if(
                active_subscriptions.begin(),
                active_subscriptions.end(),
                [&subscription](const MarketDataSubscriptionHandle& active) {
                    return active.id == subscription.id;
                }),
            active_subscriptions.end());
        if (callback) {
            callback(MarketDataSubscriptionResult::unsubscribed(std::move(subscription)));
        }
        return true;
    }

    void complete_pending_subscribe() {
        ASSERT_TRUE(static_cast<bool>(m_pending_callback));
        auto callback = std::move(m_pending_callback);
        auto handle = std::move(m_pending_subscription);
        active_subscriptions.push_back(handle);
        callback(MarketDataSubscriptionResult::subscribed(std::move(handle)));
    }

    void complete_pending_unsubscribe() {
        ASSERT_TRUE(static_cast<bool>(m_pending_unsubscribe_callback));
        auto callback = std::move(m_pending_unsubscribe_callback);
        auto subscription = std::move(m_pending_unsubscribe);
        active_subscriptions.erase(
            std::remove_if(
                active_subscriptions.begin(),
                active_subscriptions.end(),
                [&subscription](const MarketDataSubscriptionHandle& active) {
                    return active.id == subscription.id;
                }),
            active_subscriptions.end());
        callback(MarketDataSubscriptionResult::unsubscribed(
            std::move(subscription)));
    }

    void emit_status(MarketDataStatusUpdate update) {
        ASSERT_TRUE(static_cast<bool>(status_callback));
        status_callback(std::move(update));
    }

    void emit_ticks(MarketDataSubscriptionHandle subscription) {
        ASSERT_TRUE(static_cast<bool>(ticks_callback));
        auto batch = std::make_unique<TickDataBatch>();
        batch->subscription = std::move(subscription);
        batch->type = MarketDataType::TICKS;
        batch->symbol = batch->subscription.symbol;
        batch->price_digits = 5;
        batch->items.emplace_back(1.10000, 1.10002, 0.0, 1.0, 1000, 0, 0);
        ticks_callback(std::move(batch));
    }

    void emit_unscoped_ticks(std::string symbol) {
        ASSERT_TRUE(static_cast<bool>(ticks_callback));
        auto batch = std::make_unique<TickDataBatch>();
        batch->type = MarketDataType::TICKS;
        batch->symbol = std::move(symbol);
        batch->price_digits = 5;
        batch->items.emplace_back(1.10000, 1.10002, 0.0, 1.0, 1000, 0, 0);
        ticks_callback(std::move(batch));
    }

    void emit_unscoped_bars(std::string symbol, BarTimeframe timeframe) {
        ASSERT_TRUE(static_cast<bool>(bars_callback));
        auto batch = std::make_unique<BarDataBatch>();
        batch->type = MarketDataType::BARS;
        batch->symbol = std::move(symbol);
        batch->timeframe = timeframe;
        batch->price_digits = 5;
        batch->items.emplace_back(1.0, 1.2, 0.9, 1.1, 10.0, 1000);
        bars_callback(std::move(batch));
    }

private:
    SubscriptionId m_next_subscription_id = 1;
    MarketDataSubscriptionHandle m_pending_subscription;
    subscription_callback_t m_pending_callback;
    MarketDataSubscriptionHandle m_pending_unsubscribe;
    subscription_callback_t m_pending_unsubscribe_callback;

    bool finish_or_defer(
            MarketDataSubscriptionHandle handle,
            subscription_callback_t callback) {
        if (defer_subscribe) {
            m_pending_subscription = std::move(handle);
            m_pending_callback = std::move(callback);
            return true;
        }

        active_subscriptions.push_back(handle);
        if (callback) {
            callback(MarketDataSubscriptionResult::subscribed(std::move(handle)));
        }
        return true;
    }
};

class TickOnlyProvider final : public BaseMarketDataProvider {
public:
    ticks_callback_t& on_tick_data() override {
        return m_ticks_callback;
    }

    bool subscribe_ticks(
            TickSubscriptionRequest request,
            subscription_callback_t callback) override {
        auto subscription = MarketDataSubscriptionHandle::from_tick_request(
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
        if (callback) {
            callback(MarketDataSubscriptionResult::unsubscribed(
                std::move(subscription)));
        }
        return true;
    }

    void emit_ticks(const MarketDataSubscriptionHandle& subscription) {
        ASSERT_TRUE(static_cast<bool>(m_ticks_callback));
        auto batch = std::make_unique<TickDataBatch>();
        batch->subscription = subscription;
        batch->type = MarketDataType::TICKS;
        batch->symbol = subscription.symbol;
        batch->price_digits = 5;
        batch->items.emplace_back(1.10000, 1.10002, 0.0, 1.0, 1000, 0, 0);
        m_ticks_callback(std::move(batch));
    }

private:
    SubscriptionId m_next_subscription_id = 1;
    ticks_callback_t m_ticks_callback;
};

class RecordingSubscriber final : public IMarketDataSubscriber {
public:
    std::vector<TickDataBatch> ticks;
    std::vector<BarDataBatch> bars;
    std::vector<MarketDataStatusUpdate> statuses;

    void on_tick_data(const TickDataBatch& batch) override {
        ticks.push_back(batch);
    }

    void on_bar_data(const BarDataBatch& batch) override {
        bars.push_back(batch);
    }

    void on_market_data_status(const MarketDataStatusUpdate& update) override {
        statuses.push_back(update);
    }
};

class CountingSubscriber final : public IMarketDataSubscriber {
public:
    explicit CountingSubscriber(std::shared_ptr<std::size_t> calls)
            : m_calls(std::move(calls)) {}

    void on_tick_data(const TickDataBatch&) override {
        ++*m_calls;
    }

private:
    std::shared_ptr<std::size_t> m_calls;
};

MarketDataStatusUpdate ready_status(
        const FakeMarketDataProvider& provider,
        std::string symbol,
        MarketDataType type = MarketDataType::TICKS,
        BarTimeframe timeframe = 0) {
    MarketDataStatusUpdate update;
    update.provider_id = provider.provider_id();
    update.type = type;
    update.symbol = std::move(symbol);
    update.timeframe = timeframe;
    update.transport = MarketDataTransport::WEBSOCKET;
    update.status = MarketDataStreamStatus::READY;
    return update;
}

} // namespace

TEST(MarketDataRouter, UsesStrongRoutedSubscriptionIds) {
    const RoutedSubscriptionId empty;
    EXPECT_FALSE(empty.valid());
    EXPECT_FALSE(static_cast<bool>(empty));

    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto eur = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"));
    auto btc = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("BTCUSDT"));

    ASSERT_TRUE(eur.router_id().valid());
    ASSERT_TRUE(btc.router_id().valid());
    EXPECT_NE(eur.router_id(), btc.router_id());
    EXPECT_NE(eur.router_id().value(), 0u);
}

TEST(MarketDataRouter, RoutesRegisteredProvidersByStableIdAndAlias) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    const MarketDataProviderId provider_id{1001};
    auto tick_subscriber = std::make_shared<RecordingSubscriber>();
    auto bar_subscriber = std::make_shared<RecordingSubscriber>();

    ASSERT_TRUE(router.register_provider(
        provider_id,
        provider,
        {"intrade", "intrade-bar"}));
    EXPECT_EQ(router.registered_provider_count(), 1u);
    EXPECT_FALSE(static_cast<bool>(provider.on_tick_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_bar_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_market_data_status()));

    auto ticks = router.subscribe_ticks(
        provider_id,
        tick_subscriber,
        TickSubscriptionRequest("EURUSD"));
    auto bars = router.subscribe_bars(
        "intrade-bar",
        bar_subscriber,
        BarSubscriptionRequest("EURUSD", 60));

    ASSERT_TRUE(ticks.active());
    ASSERT_TRUE(bars.active());
    EXPECT_EQ(ticks.registered_provider_id(), provider_id);
    EXPECT_EQ(bars.registered_provider_id(), provider_id);
    EXPECT_EQ(
        router.registered_provider_id(provider.provider_id()),
        provider_id);
    EXPECT_EQ(router.provider_aliases(provider_id).size(), 2u);

    provider.emit_ticks(ticks.provider_subscription());
    provider.emit_unscoped_bars("EURUSD", 60);
    ASSERT_EQ(tick_subscriber->ticks.size(), 1u);
    ASSERT_EQ(bar_subscriber->bars.size(), 1u);
}

TEST(MarketDataRouter, RejectsProviderRegistrationCollisions) {
    FakeMarketDataProvider first;
    FakeMarketDataProvider second;
    MarketDataRouter router;
    const MarketDataProviderId first_id{1001};
    const MarketDataProviderId second_id{2001};

    ASSERT_TRUE(router.register_provider(first_id, first, {"primary"}));
    EXPECT_FALSE(router.register_provider(first_id, second, {"secondary"}));
    EXPECT_FALSE(router.register_provider(second_id, first, {"secondary"}));
    EXPECT_FALSE(router.register_provider(second_id, second, {"primary"}));
    EXPECT_FALSE(router.register_provider(second_id, second, {"", "secondary"}));
    EXPECT_FALSE(router.register_provider(
        second_id,
        second,
        {"secondary", "secondary"}));
    EXPECT_EQ(router.registered_provider_count(), 1u);

    EXPECT_TRUE(router.add_provider_alias(first_id, "main"));
    EXPECT_TRUE(router.add_provider_alias(first_id, "main"));
    EXPECT_FALSE(router.add_provider_alias(second_id, "secondary"));
    EXPECT_EQ(router.provider_aliases(first_id).size(), 2u);
}

TEST(MarketDataRouter, ReportsUnknownRegisteredProviderSelections) {
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataSubscriptionResult numeric_result;
    MarketDataSubscriptionResult alias_result;

    auto numeric = router.subscribe_ticks(
        MarketDataProviderId{1001},
        subscriber,
        TickSubscriptionRequest("EURUSD"),
        [&numeric_result](MarketDataSubscriptionResult result) {
            numeric_result = std::move(result);
        });
    auto alias = router.subscribe_ticks(
        "missing",
        subscriber,
        TickSubscriptionRequest("BTCUSDT"),
        [&alias_result](MarketDataSubscriptionResult result) {
            alias_result = std::move(result);
        });

    EXPECT_FALSE(numeric.valid());
    EXPECT_FALSE(alias.valid());
    EXPECT_EQ(numeric_result.status, MarketDataSubscriptionStatus::FAILED);
    EXPECT_EQ(alias_result.status, MarketDataSubscriptionStatus::FAILED);
    EXPECT_NE(numeric_result.error_message.find("ID"), std::string::npos);
    EXPECT_NE(alias_result.error_message.find("alias"), std::string::npos);
}

TEST(MarketDataRouter, UnregistersOnlyIdleProvidersAndReleasesAliases) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    const MarketDataProviderId provider_id{1001};
    auto subscriber = std::make_shared<RecordingSubscriber>();

    ASSERT_TRUE(router.register_provider(provider_id, provider, {"intrade"}));
    auto route = router.subscribe_ticks(
        "intrade",
        subscriber,
        TickSubscriptionRequest("EURUSD"));
    ASSERT_TRUE(route.active());
    EXPECT_FALSE(router.unregister_provider(provider_id));

    route.reset();
    EXPECT_TRUE(router.unregister_provider(provider_id));
    EXPECT_EQ(router.registered_provider_count(), 0u);
    EXPECT_FALSE(router.registered_provider_id(provider.provider_id()).valid());
    EXPECT_TRUE(router.provider_aliases(provider_id).empty());

    MarketDataSubscriptionResult result;
    auto missing = router.subscribe_ticks(
        "intrade",
        subscriber,
        TickSubscriptionRequest("EURUSD"),
        [&result](MarketDataSubscriptionResult update) {
            result = std::move(update);
        });
    EXPECT_FALSE(missing.valid());
    EXPECT_EQ(result.status, MarketDataSubscriptionStatus::FAILED);
}

TEST(MarketDataRouter, RoutesEventsToTheirConcreteSubscriptions) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto eur_subscriber = std::make_shared<RecordingSubscriber>();
    auto btc_subscriber = std::make_shared<RecordingSubscriber>();

    auto eur = router.subscribe_ticks(
        provider,
        eur_subscriber,
        TickSubscriptionRequest("EURUSD", MarketDataTransport::WEBSOCKET));
    auto btc = router.subscribe_ticks(
        provider,
        btc_subscriber,
        TickSubscriptionRequest("BTCUSDT", MarketDataTransport::WEBSOCKET));

    ASSERT_TRUE(eur.active());
    ASSERT_TRUE(btc.active());
    ASSERT_NE(eur.router_id(), btc.router_id());
    const auto eur_provider_subscription = eur.provider_subscription();
    const auto btc_provider_subscription = btc.provider_subscription();
    ASSERT_NE(eur_provider_subscription.id, btc_provider_subscription.id);

    provider.emit_ticks(eur_provider_subscription);
    ASSERT_EQ(eur_subscriber->ticks.size(), 1u);
    EXPECT_TRUE(btc_subscriber->ticks.empty());
    EXPECT_EQ(
        eur_subscriber->ticks.front().subscription.id,
        eur_provider_subscription.id);

    provider.emit_status(ready_status(provider, "BTCUSDT"));
    EXPECT_TRUE(eur_subscriber->statuses.empty());
    ASSERT_EQ(btc_subscriber->statuses.size(), 1u);
    EXPECT_EQ(
        btc_subscriber->statuses.front().subscription.id,
        btc_provider_subscription.id);
}

TEST(MarketDataRouter, ReplaysMatchingReadyForEachLateConcreteSubscription) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto first = std::make_shared<RecordingSubscriber>();
    auto second = std::make_shared<RecordingSubscriber>();

    auto first_btc = router.subscribe_ticks(
        provider,
        first,
        TickSubscriptionRequest("BTCUSDT", MarketDataTransport::WEBSOCKET));
    provider.emit_status(ready_status(provider, "EURUSD"));
    provider.emit_status(ready_status(provider, "BTCUSDT"));

    auto second_btc = router.subscribe_ticks(
        provider,
        second,
        TickSubscriptionRequest("BTCUSDT", MarketDataTransport::WEBSOCKET));

    ASSERT_EQ(first->statuses.size(), 1u);
    ASSERT_EQ(second->statuses.size(), 1u);
    EXPECT_EQ(first->statuses.front().subscription.id, first_btc.provider_subscription().id);
    EXPECT_EQ(second->statuses.front().subscription.id, second_btc.provider_subscription().id);
    EXPECT_EQ(second->statuses.front().symbol, "BTCUSDT");
    EXPECT_EQ(second->statuses.front().status, MarketDataStreamStatus::READY);
}

TEST(MarketDataRouter, AddsConcreteContextToMatchingUnscopedBarBatches) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto one_minute = std::make_shared<RecordingSubscriber>();
    auto five_minutes = std::make_shared<RecordingSubscriber>();

    auto one_minute_route = router.subscribe_bars(
        provider,
        one_minute,
        BarSubscriptionRequest(
            "EURUSD",
            60,
            BarPriceSource::MID,
            MarketDataTransport::WEBSOCKET));
    auto five_minute_route = router.subscribe_bars(
        provider,
        five_minutes,
        BarSubscriptionRequest(
            "EURUSD",
            300,
            BarPriceSource::MID,
            MarketDataTransport::WEBSOCKET));

    provider.emit_unscoped_bars("EURUSD", 60);

    ASSERT_EQ(one_minute->bars.size(), 1u);
    EXPECT_TRUE(five_minutes->bars.empty());
    EXPECT_EQ(
        one_minute->bars.front().subscription.id,
        one_minute_route.provider_subscription().id);
    EXPECT_EQ(one_minute->bars.front().timeframe, 60);
    EXPECT_TRUE(five_minute_route.active());
}

TEST(MarketDataRouter, MoveAndResetUnsubscribeExactlyOnce) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto original = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"));
    const auto router_id = original.router_id();
    ASSERT_TRUE(original.active());

    MarketDataRouter::SubscriptionHandle moved(std::move(original));
    EXPECT_FALSE(original.valid());
    EXPECT_EQ(moved.router_id(), router_id);

    moved.reset();
    EXPECT_FALSE(moved.valid());
    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    EXPECT_EQ(router.subscription_count(), 0u);

    moved.reset();
    EXPECT_EQ(provider.unsubscribe_calls, 1u);
}

TEST(MarketDataRouter, RetainsCleanupWhenProviderRejectsUnsubscribe) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataSubscriptionResult unsubscribe_result;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"));
    const auto provider_subscription = route.provider_subscription();
    ASSERT_TRUE(route.active());
    ASSERT_EQ(provider.active_subscriptions.size(), 1u);

    provider.unsubscribe_mode = FakeMarketDataProvider::UnsubscribeMode::REJECT;
    EXPECT_FALSE(route.unsubscribe(
        [&unsubscribe_result](MarketDataSubscriptionResult result) {
            unsubscribe_result = std::move(result);
        }));

    EXPECT_FALSE(route.valid());
    EXPECT_FALSE(unsubscribe_result.success());
    EXPECT_EQ(provider.active_subscriptions.size(), 1u);
    EXPECT_EQ(router.subscription_count(), 1u);
    EXPECT_EQ(router.failed_unsubscribe_count(), 1u);
    EXPECT_TRUE(static_cast<bool>(provider.on_tick_data()));
    EXPECT_TRUE(static_cast<bool>(provider.on_bar_data()));
    EXPECT_TRUE(static_cast<bool>(provider.on_market_data_status()));

    provider.emit_ticks(provider_subscription);
    provider.emit_unscoped_ticks("EURUSD");
    EXPECT_TRUE(subscriber->ticks.empty());

    MarketDataSubscriptionResult rejected_result;
    auto rejected = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("BTCUSDT"),
        [&rejected_result](MarketDataSubscriptionResult result) {
            rejected_result = std::move(result);
        });
    EXPECT_FALSE(rejected.valid());
    EXPECT_FALSE(rejected_result.success());
    EXPECT_NE(rejected_result.error_message.find("retry"), std::string::npos);
    EXPECT_EQ(provider.active_subscriptions.size(), 1u);

    provider.unsubscribe_mode = FakeMarketDataProvider::UnsubscribeMode::SUCCEED;
    EXPECT_EQ(router.retry_failed_unsubscribes(), 1u);
    EXPECT_TRUE(provider.active_subscriptions.empty());
    EXPECT_EQ(router.subscription_count(), 0u);
    EXPECT_EQ(router.failed_unsubscribe_count(), 0u);
    EXPECT_FALSE(static_cast<bool>(provider.on_tick_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_bar_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_market_data_status()));
}

TEST(MarketDataRouter, RetainsCleanupWhenAcceptedUnsubscribeCompletesFailed) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataSubscriptionResult unsubscribe_result;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"));
    ASSERT_TRUE(route.active());

    provider.unsubscribe_mode =
        FakeMarketDataProvider::UnsubscribeMode::COMPLETE_FAILED;
    EXPECT_TRUE(route.unsubscribe(
        [&unsubscribe_result](MarketDataSubscriptionResult result) {
            unsubscribe_result = std::move(result);
        }));

    EXPECT_FALSE(route.valid());
    EXPECT_FALSE(unsubscribe_result.success());
    EXPECT_EQ(provider.active_subscriptions.size(), 1u);
    EXPECT_EQ(router.subscription_count(), 1u);
    EXPECT_EQ(router.failed_unsubscribe_count(), 1u);
    EXPECT_TRUE(static_cast<bool>(provider.on_tick_data()));

    provider.unsubscribe_mode = FakeMarketDataProvider::UnsubscribeMode::SUCCEED;
    router.shutdown();

    EXPECT_TRUE(provider.active_subscriptions.empty());
    EXPECT_EQ(router.subscription_count(), 0u);
    EXPECT_EQ(router.failed_unsubscribe_count(), 0u);
    EXPECT_FALSE(static_cast<bool>(provider.on_tick_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_bar_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_market_data_status()));
}

TEST(MarketDataRouter, QuarantinesProviderWhilePhysicalUnsubscribeIsPending) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"));
    ASSERT_TRUE(route.active());

    provider.unsubscribe_mode = FakeMarketDataProvider::UnsubscribeMode::DEFER;
    EXPECT_TRUE(route.unsubscribe());
    EXPECT_FALSE(route.valid());
    EXPECT_EQ(router.subscription_count(), 1u);
    EXPECT_EQ(router.failed_unsubscribe_count(), 0u);
    ASSERT_EQ(provider.active_subscriptions.size(), 1u);

    MarketDataSubscriptionResult rejected_result;
    auto rejected = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("BTCUSDT"),
        [&rejected_result](MarketDataSubscriptionResult result) {
            rejected_result = std::move(result);
        });
    EXPECT_FALSE(rejected.valid());
    EXPECT_FALSE(rejected_result.success());
    EXPECT_NE(rejected_result.error_message.find("pending"), std::string::npos);
    EXPECT_EQ(provider.active_subscriptions.size(), 1u);

    provider.complete_pending_unsubscribe();
    EXPECT_TRUE(provider.active_subscriptions.empty());
    EXPECT_EQ(router.subscription_count(), 0u);
    EXPECT_FALSE(static_cast<bool>(provider.on_tick_data()));

    provider.unsubscribe_mode = FakeMarketDataProvider::UnsubscribeMode::SUCCEED;
    auto replacement = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("BTCUSDT"));
    EXPECT_TRUE(replacement.active());
}

TEST(MarketDataRouter, PendingHandleCancellationUnsubscribesAfterAcceptance) {
    FakeMarketDataProvider provider;
    provider.defer_subscribe = true;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataSubscriptionStatus subscribe_status = MarketDataSubscriptionStatus::UNKNOWN;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("BTCUSDT"),
        [&subscribe_status](MarketDataSubscriptionResult result) {
            subscribe_status = result.status;
        });
    ASSERT_TRUE(route.pending());

    route.reset();
    EXPECT_EQ(provider.unsubscribe_calls, 0u);
    EXPECT_EQ(router.subscription_count(), 1u);

    provider.complete_pending_subscribe();

    EXPECT_EQ(subscribe_status, MarketDataSubscriptionStatus::SUBSCRIBED);
    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    EXPECT_EQ(router.subscription_count(), 0u);
}

TEST(MarketDataRouter, ExpiredSubscriberDoesNotReceiveData) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto calls = std::make_shared<std::size_t>(0);
    auto subscriber = std::make_shared<CountingSubscriber>(calls);
    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"));
    const auto subscription = route.provider_subscription();

    subscriber.reset();
    provider.emit_ticks(subscription);

    EXPECT_EQ(*calls, 0u);
    EXPECT_TRUE(route.active());
}

TEST(MarketDataRouter, ShutdownUnsubscribesAndReleasesProviderCallbacks) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto ticks = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"));
    auto bars = router.subscribe_bars(
        provider,
        subscriber,
        BarSubscriptionRequest("EURUSD", 60));

    router.shutdown();

    EXPECT_EQ(provider.unsubscribe_calls, 2u);
    EXPECT_EQ(router.subscription_count(), 0u);
    EXPECT_FALSE(ticks.valid());
    EXPECT_FALSE(bars.valid());
    EXPECT_FALSE(static_cast<bool>(provider.on_tick_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_bar_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_market_data_status()));
}

TEST(MarketDataRouter, ProcessesLateSuccessfulSubscribeDuringShutdown) {
    FakeMarketDataProvider provider;
    provider.defer_subscribe = true;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    std::size_t completion_callbacks = 0;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"),
        [&completion_callbacks](MarketDataSubscriptionResult) {
            ++completion_callbacks;
        });
    ASSERT_TRUE(route.pending());

    router.shutdown();
    router.shutdown();

    EXPECT_FALSE(router.is_shutdown_complete());
    EXPECT_EQ(router.subscription_count(), 1u);
    EXPECT_EQ(provider.unsubscribe_calls, 0u);

    provider.complete_pending_subscribe();

    ASSERT_EQ(provider.active_subscriptions.size(), 1u);
    EXPECT_EQ(completion_callbacks, 0u);

    router.process();

    EXPECT_TRUE(router.is_shutdown_complete());
    EXPECT_EQ(router.subscription_count(), 0u);
    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    EXPECT_TRUE(provider.active_subscriptions.empty());
    EXPECT_FALSE(route.valid());
    EXPECT_EQ(completion_callbacks, 0u);
    EXPECT_TRUE(subscriber->ticks.empty());
    EXPECT_TRUE(subscriber->statuses.empty());
    EXPECT_FALSE(static_cast<bool>(provider.on_tick_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_bar_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_market_data_status()));

    router.process();
    router.shutdown();
    EXPECT_EQ(provider.unsubscribe_calls, 1u);
}

TEST(MarketDataRouter, ProcessesDeferredUnsubscribeDuringShutdown) {
    FakeMarketDataProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"));
    ASSERT_TRUE(route.active());
    provider.unsubscribe_mode = FakeMarketDataProvider::UnsubscribeMode::DEFER;

    router.shutdown();

    EXPECT_FALSE(router.is_shutdown_complete());
    EXPECT_EQ(router.subscription_count(), 1u);
    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    EXPECT_FALSE(route.valid());
    EXPECT_TRUE(static_cast<bool>(provider.on_tick_data()));

    provider.complete_pending_unsubscribe();
    EXPECT_FALSE(router.is_shutdown_complete());

    router.process();

    EXPECT_TRUE(router.is_shutdown_complete());
    EXPECT_EQ(router.subscription_count(), 0u);
    EXPECT_TRUE(provider.active_subscriptions.empty());
    EXPECT_FALSE(static_cast<bool>(provider.on_tick_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_bar_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_market_data_status()));
}

TEST(MarketDataRouter, KeepsLateShutdownCleanupFailureRetryable) {
    FakeMarketDataProvider provider;
    provider.defer_subscribe = true;
    provider.unsubscribe_mode =
        FakeMarketDataProvider::UnsubscribeMode::COMPLETE_FAILED;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    std::size_t completion_callbacks = 0;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"),
        [&completion_callbacks](MarketDataSubscriptionResult) {
            ++completion_callbacks;
        });
    ASSERT_TRUE(route.pending());

    router.shutdown();
    provider.complete_pending_subscribe();
    router.process();

    EXPECT_FALSE(router.is_shutdown_complete());
    EXPECT_EQ(router.subscription_count(), 1u);
    EXPECT_EQ(router.failed_unsubscribe_count(), 1u);
    EXPECT_EQ(provider.unsubscribe_calls, 1u);
    ASSERT_EQ(provider.active_subscriptions.size(), 1u);
    EXPECT_EQ(completion_callbacks, 0u);

    router.shutdown();
    EXPECT_EQ(router.failed_unsubscribe_count(), 1u);
    EXPECT_EQ(provider.unsubscribe_calls, 1u);

    provider.unsubscribe_mode = FakeMarketDataProvider::UnsubscribeMode::SUCCEED;
    EXPECT_EQ(router.retry_failed_unsubscribes(), 1u);
    EXPECT_TRUE(provider.active_subscriptions.empty());
    EXPECT_FALSE(router.is_shutdown_complete());

    router.process();

    EXPECT_TRUE(router.is_shutdown_complete());
    EXPECT_EQ(router.subscription_count(), 0u);
    EXPECT_EQ(router.failed_unsubscribe_count(), 0u);
    EXPECT_EQ(provider.unsubscribe_calls, 2u);
    EXPECT_FALSE(route.valid());
    EXPECT_EQ(completion_callbacks, 0u);
}

TEST(MarketDataRouter, RejectsProvidersWithAnExistingCallbackOwner) {
    FakeMarketDataProvider provider;
    provider.on_tick_data() = [](std::unique_ptr<TickDataBatch>) {};
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataSubscriptionResult result;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        TickSubscriptionRequest("EURUSD"),
        [&result](MarketDataSubscriptionResult update) {
            result = std::move(update);
        });

    EXPECT_FALSE(route.valid());
    EXPECT_FALSE(result.success());
    EXPECT_NE(result.error_message.find("already assigned"), std::string::npos);
    EXPECT_EQ(router.subscription_count(), 0u);
    EXPECT_TRUE(static_cast<bool>(provider.on_tick_data()));
}

TEST(MarketDataRouter, BindsIndependentTickOnlyProvidersWithDefaultUnusedChannels) {
    TickOnlyProvider provider_a;
    TickOnlyProvider provider_b;
    MarketDataRouter router_a;
    MarketDataRouter router_b;
    auto subscriber_a = std::make_shared<RecordingSubscriber>();
    auto subscriber_b = std::make_shared<RecordingSubscriber>();

    auto route_a = router_a.subscribe_ticks(
        provider_a,
        subscriber_a,
        TickSubscriptionRequest("EURUSD"));
    auto route_b = router_b.subscribe_ticks(
        provider_b,
        subscriber_b,
        TickSubscriptionRequest("BTCUSDT"));

    ASSERT_TRUE(route_a.active());
    ASSERT_TRUE(route_b.active());

    provider_a.emit_ticks(route_a.provider_subscription());
    provider_b.emit_ticks(route_b.provider_subscription());

    ASSERT_EQ(subscriber_a->ticks.size(), 1u);
    ASSERT_EQ(subscriber_b->ticks.size(), 1u);
    EXPECT_EQ(subscriber_a->ticks.front().symbol, "EURUSD");
    EXPECT_EQ(subscriber_b->ticks.front().symbol, "BTCUSDT");
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
