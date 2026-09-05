#include <gtest/gtest.h>

#include <memory>
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

    bars_callback_t& on_bar_data() override {
        return bars_callback;
    }

    ticks_callback_t& on_tick_data() override {
        return ticks_callback;
    }

    status_callback_t& on_market_data_status() override {
        return status_callback;
    }
};

class RecordingMarketDataSubscriber final : public IMarketDataSubscriber {
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

TickDataBatch make_tick_batch() {
    TickDataBatch batch;
    batch.type = MarketDataType::TICKS;
    batch.symbol = "BTCUSDT";
    batch.price_digits = 2;
    batch.items.push_back(Tick(0.0, 0.0, 61521.34, 0.00017, 1783028778697ULL, 0, 0));
    mark_live_payload(batch.items.front().flags);
    return batch;
}

BarDataBatch make_bar_batch() {
    BarDataBatch batch;
    batch.type = MarketDataType::BARS;
    batch.symbol = "EURUSD";
    batch.timeframe = 60;
    batch.price_digits = 5;
    batch.items.push_back(Bar(1.1, 1.2, 1.0, 1.15, 10.0, 1783028760000ULL));
    mark_live_payload(batch.items.front().flags);
    return batch;
}

MarketDataStatusUpdate make_ready_status(SubscriptionId subscription_id = 42) {
    const TickSubscriptionRequest request(
        "BTCUSDT",
        MarketDataTransport::WEBSOCKET);

    MarketDataStatusUpdate update;
    update.provider_id = 17;
    update.subscription = MarketDataSubscriptionHandle::from_tick_request(
        update.provider_id,
        subscription_id,
        request);
    update.type = MarketDataType::TICKS;
    update.symbol = request.symbol;
    update.transport = request.transport;
    update.status = MarketDataStreamStatus::READY;
    return update;
}

} // namespace

TEST(MarketDataHub, RoutesProviderCallbacksToSubscribers) {
    FakeMarketDataProvider provider;
    MarketDataHub hub;
    hub.bind_to(provider);

    auto first = std::make_shared<RecordingMarketDataSubscriber>();
    auto second = std::make_shared<RecordingMarketDataSubscriber>();
    const auto first_id = hub.add_subscriber(first);
    const auto second_id = hub.add_subscriber(second);

    ASSERT_NE(first_id, MarketDataHub::INVALID_SUBSCRIBER_ID);
    ASSERT_NE(second_id, MarketDataHub::INVALID_SUBSCRIBER_ID);
    EXPECT_EQ(hub.subscriber_count(), 2u);

    provider.on_market_data_status()(make_ready_status());
    provider.on_tick_data()(std::make_unique<TickDataBatch>(make_tick_batch()));
    provider.on_bar_data()(std::make_unique<BarDataBatch>(make_bar_batch()));

    ASSERT_EQ(first->statuses.size(), 1u);
    ASSERT_EQ(second->statuses.size(), 1u);
    EXPECT_EQ(first->statuses[0].status, MarketDataStreamStatus::READY);
    ASSERT_TRUE(first->statuses[0].subscription.valid());
    EXPECT_EQ(first->statuses[0].subscription.id, 42u);
    EXPECT_EQ(second->statuses[0].symbol, "BTCUSDT");

    ASSERT_EQ(first->ticks.size(), 1u);
    ASSERT_EQ(second->ticks.size(), 1u);
    EXPECT_EQ(first->ticks[0].symbol, "BTCUSDT");
    ASSERT_EQ(second->ticks[0].items.size(), 1u);
    EXPECT_DOUBLE_EQ(second->ticks[0].items[0].last, 61521.34);

    ASSERT_EQ(first->bars.size(), 1u);
    ASSERT_EQ(second->bars.size(), 1u);
    EXPECT_EQ(first->bars[0].symbol, "EURUSD");
    EXPECT_EQ(second->bars[0].timeframe, 60);

    EXPECT_TRUE(hub.remove_subscriber(first_id));
    EXPECT_EQ(hub.subscriber_count(), 1u);

    provider.on_tick_data()(std::make_unique<TickDataBatch>(make_tick_batch()));
    EXPECT_EQ(first->ticks.size(), 1u);
    EXPECT_EQ(second->ticks.size(), 2u);
}

TEST(MarketDataHub, ReplaysCachedStatusToLateSubscribers) {
    MarketDataHub hub;
    hub.publish_status(make_ready_status());

    auto replayed = std::make_shared<RecordingMarketDataSubscriber>();
    const auto replayed_id = hub.add_subscriber(replayed);

    ASSERT_NE(replayed_id, MarketDataHub::INVALID_SUBSCRIBER_ID);
    ASSERT_EQ(replayed->statuses.size(), 1u);
    EXPECT_EQ(replayed->statuses[0].status, MarketDataStreamStatus::READY);
    EXPECT_EQ(replayed->statuses[0].symbol, "BTCUSDT");

    auto quiet = std::make_shared<RecordingMarketDataSubscriber>();
    MarketDataSubscriberOptions options;
    options.replay_last_status = false;
    const auto quiet_id = hub.add_subscriber(quiet, options);

    ASSERT_NE(quiet_id, MarketDataHub::INVALID_SUBSCRIBER_ID);
    EXPECT_TRUE(quiet->statuses.empty());
}

TEST(MarketDataHub, ReplaysSubscriptionScopedStatusesWithoutCollapsingStreamKey) {
    MarketDataHub hub;
    hub.publish_status(make_ready_status(42));
    hub.publish_status(make_ready_status(43));

    auto replayed = std::make_shared<RecordingMarketDataSubscriber>();
    const auto replayed_id = hub.add_subscriber(replayed);

    ASSERT_NE(replayed_id, MarketDataHub::INVALID_SUBSCRIBER_ID);
    ASSERT_EQ(replayed->statuses.size(), 2u);
    EXPECT_EQ(replayed->statuses[0].subscription.id, 42u);
    EXPECT_EQ(replayed->statuses[1].subscription.id, 43u);
    EXPECT_EQ(replayed->statuses[0].symbol, "BTCUSDT");
    EXPECT_EQ(replayed->statuses[1].symbol, "BTCUSDT");
}

TEST(MarketDataHub, PrunesExpiredSubscribersAndUnbindsProviderCallbacks) {
    FakeMarketDataProvider provider;
    MarketDataHub hub;
    hub.bind_to(provider);

    auto subscriber = std::make_shared<RecordingMarketDataSubscriber>();
    ASSERT_NE(hub.add_subscriber(subscriber), MarketDataHub::INVALID_SUBSCRIBER_ID);
    EXPECT_EQ(hub.subscriber_count(), 1u);

    subscriber.reset();
    EXPECT_EQ(hub.subscriber_count(), 0u);

    ASSERT_TRUE(static_cast<bool>(provider.on_tick_data()));
    ASSERT_TRUE(static_cast<bool>(provider.on_bar_data()));
    ASSERT_TRUE(static_cast<bool>(provider.on_market_data_status()));

    hub.unbind_from(provider);

    EXPECT_FALSE(static_cast<bool>(provider.on_tick_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_bar_data()));
    EXPECT_FALSE(static_cast<bool>(provider.on_market_data_status()));
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
