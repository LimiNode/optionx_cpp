#include <gtest/gtest.h>

#include <cstdint>

namespace market_data_continuity_test_clock {
    inline std::uint64_t now_ms = 240000ULL;
}

#ifndef OPTIONX_TIMESTAMP_MS
#define OPTIONX_TIMESTAMP_MS market_data_continuity_test_clock::now_ms
#endif

#include <algorithm>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <optionx_cpp/market_data.hpp>

using namespace optionx;
using namespace optionx::market_data;

namespace {

class OwnerQueue {
public:
    bool post(MarketDataRouter::owner_task_t task) {
        if (!m_accepting || !task) return false;
        m_tasks.push_back(std::move(task));
        return true;
    }

    void drain() {
        while (!m_tasks.empty()) {
            auto task = std::move(m_tasks.front());
            m_tasks.pop_front();
            task();
        }
    }

    void stop_accepting() noexcept {
        m_accepting = false;
    }

private:
    std::deque<MarketDataRouter::owner_task_t> m_tasks;
    bool m_accepting = true;
};

class ScopedTestClock {
public:
    explicit ScopedTestClock(std::uint64_t now_ms) {
        market_data_continuity_test_clock::now_ms = now_ms;
    }

    ~ScopedTestClock() {
        market_data_continuity_test_clock::now_ms = 240000ULL;
    }
};

class FakeHistoryProvider final : public BaseMarketDataProvider {
public:
    bars_callback_t& on_bar_data() override {
        return m_bar_callback;
    }

    ticks_callback_t& on_tick_data() override {
        return m_tick_callback;
    }

    status_callback_t& on_market_data_status() override {
        return m_status_callback;
    }

    bool subscribe_bars(
            BarSubscriptionRequest request,
            subscription_callback_t callback) override {
        auto subscription = MarketDataSubscriptionHandle::from_bar_request(
            provider_id(),
            m_next_subscription_id++,
            request);
        m_active_subscription = subscription;
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

    bool fetch_bar_history(
            const BarHistoryRequest& request,
            bar_history_callback_t callback) override {
        history_requests.push_back(request);
        if (reject_history) return false;
        m_history_callbacks.push_back(std::move(callback));
        return true;
    }

    void complete_history(BarSequence sequence) {
        ASSERT_FALSE(m_history_callbacks.empty());
        auto callback = std::move(m_history_callbacks.front());
        m_history_callbacks.pop_front();
        callback(BarHistoryResult::ok(std::move(sequence)));
    }

    void fail_history(std::string message) {
        ASSERT_FALSE(m_history_callbacks.empty());
        auto callback = std::move(m_history_callbacks.front());
        m_history_callbacks.pop_front();
        callback(BarHistoryResult::fail(std::move(message)));
    }

    void emit_status(MarketDataStreamStatus status) {
        if (!m_status_callback) return;
        MarketDataStatusUpdate update;
        update.subscription = m_active_subscription;
        update.type = MarketDataType::BARS;
        update.symbol = m_active_subscription.symbol;
        update.timeframe = m_active_subscription.timeframe;
        update.transport = m_active_subscription.transport;
        update.status = status;
        m_status_callback(std::move(update));
    }

    void emit_live_bar(std::uint64_t time_ms) {
        emit_live_bars({time_ms});
    }

    void emit_live_bars(std::initializer_list<std::uint64_t> times) {
        auto batch = std::make_unique<BarDataBatch>();
        batch->subscription = m_active_subscription;
        batch->type = MarketDataType::BARS;
        batch->symbol = m_active_subscription.symbol;
        batch->timeframe = m_active_subscription.timeframe;
        for (const auto time_ms : times) {
            batch->items.emplace_back(1.0, 2.0, 0.5, 1.5, 1.0, time_ms);
            mark_live_payload(batch->items.back().flags);
        }
        if (m_bar_callback) m_bar_callback(std::move(batch));
    }

    void emit_unscoped_live_bar(std::uint64_t time_ms) {
        auto batch = std::make_unique<BarDataBatch>();
        batch->type = MarketDataType::BARS;
        batch->symbol = "EURUSD";
        batch->timeframe = 60;
        batch->items.emplace_back(1.0, 2.0, 0.5, 1.5, 1.0, time_ms);
        mark_live_payload(batch->items.back().flags);
        if (m_bar_callback) m_bar_callback(std::move(batch));
    }

    std::vector<BarHistoryRequest> history_requests;
    bool reject_history = false;

private:
    SubscriptionId m_next_subscription_id = 1;
    MarketDataSubscriptionHandle m_active_subscription;
    std::deque<bar_history_callback_t> m_history_callbacks;
    bars_callback_t m_bar_callback;
    ticks_callback_t m_tick_callback;
    status_callback_t m_status_callback;
};

class RecordingSubscriber final : public IMarketDataSubscriber {
public:
    void on_bar_data(const BarDataBatch& batch) override {
        bars.push_back(batch);
    }

    void on_market_data_continuity(
            const MarketDataContinuityUpdate& update) override {
        continuity.push_back(update);
    }

    void on_market_data_status(
            const MarketDataStatusUpdate& update) override {
        statuses.push_back(update);
    }

    std::vector<BarDataBatch> bars;
    std::vector<MarketDataContinuityUpdate> continuity;
    std::vector<MarketDataStatusUpdate> statuses;
};

BarSequence make_history(std::initializer_list<std::uint64_t> times) {
    BarSequence sequence;
    sequence.symbol = "EURUSD";
    sequence.provider = "fake";
    sequence.timeframe = 60;
    sequence.price_digits = 5;
    sequence.volume_digits = 0;
    sequence.price_source = BarPriceSource::MID;
    for (const auto time_ms : times) {
        sequence.bars.emplace_back(1.0, 2.0, 0.5, 1.5, 1.0, time_ms);
    }
    return sequence;
}

void expect_live_delivery(const Bar& bar, bool catchup) {
    EXPECT_TRUE(bar.has_flag(MarketDataFlags::LIVE_SOURCE));
    EXPECT_EQ(bar.has_flag(MarketDataFlags::REALTIME), !catchup);
    EXPECT_EQ(bar.has_flag(MarketDataFlags::CATCHUP), catchup);
    EXPECT_FALSE(bar.has_flag(MarketDataFlags::HISTORICAL));
    EXPECT_FALSE(bar.has_flag(MarketDataFlags::BACKFILL));
    EXPECT_TRUE(market_data_flags_valid(bar.flags));
}

void expect_history_delivery(const Bar& bar, bool backfill) {
    EXPECT_FALSE(bar.has_flag(MarketDataFlags::LIVE_SOURCE));
    EXPECT_FALSE(bar.has_flag(MarketDataFlags::REALTIME));
    EXPECT_FALSE(bar.has_flag(MarketDataFlags::CATCHUP));
    EXPECT_TRUE(bar.has_flag(MarketDataFlags::HISTORICAL));
    EXPECT_EQ(bar.has_flag(MarketDataFlags::BACKFILL), backfill);
    EXPECT_TRUE(market_data_flags_valid(bar.flags));
}

BarSubscriptionRequest continuity_request(MarketDataContinuityMode mode) {
    BarSubscriptionRequest request(
        "EURUSD",
        60,
        BarPriceSource::MID,
        MarketDataTransport::WEBSOCKET);
    request.continuity.mode = mode;
    request.continuity.prefill_bars = 1;
    request.continuity.max_backfill_bars = 10;
    return request;
}

std::size_t continuity_status_count(
        const RecordingSubscriber& subscriber,
        MarketDataContinuityStatus status) {
    return static_cast<std::size_t>(std::count_if(
        subscriber.continuity.begin(),
        subscriber.continuity.end(),
        [status](const MarketDataContinuityUpdate& update) {
            return update.status == status;
        }));
}

TEST(MarketDataContinuity, PrefillDeliversHistoryBeforeBufferedLiveBars) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL));
    ASSERT_TRUE(route.active());
    ASSERT_EQ(provider.history_requests.size(), 1U);

    provider.emit_live_bar(200000);
    EXPECT_TRUE(subscriber->bars.empty());

    provider.complete_history(make_history({100000}));

    ASSERT_EQ(subscriber->bars.size(), 2U);
    expect_history_delivery(subscriber->bars[0].items[0], false);
    expect_live_delivery(subscriber->bars[1].items[0], true);
    ASSERT_EQ(subscriber->continuity.size(), 2U);
    EXPECT_EQ(subscriber->continuity[0].status, MarketDataContinuityStatus::PREFILLING);
    EXPECT_EQ(subscriber->continuity[1].status, MarketDataContinuityStatus::LIVE);
    EXPECT_EQ(
        subscriber->continuity[0].subscription.id,
        route.provider_subscription().id);
    EXPECT_EQ(
        subscriber->continuity[1].subscription.id,
        route.provider_subscription().id);
}

TEST(MarketDataContinuity, DeliveryFlagsAreScopedToEachRoute) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto realtime_subscriber = std::make_shared<RecordingSubscriber>();
    auto catchup_subscriber = std::make_shared<RecordingSubscriber>();

    const auto live_route = router.subscribe_bars(
        provider,
        realtime_subscriber,
        BarSubscriptionRequest("EURUSD", 60, BarPriceSource::MID));
    const auto catchup_route = router.subscribe_bars(
        provider,
        catchup_subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL));
    ASSERT_TRUE(live_route.active());
    ASSERT_TRUE(catchup_route.active());

    provider.emit_unscoped_live_bar(200000);
    ASSERT_EQ(realtime_subscriber->bars.size(), 1U);
    EXPECT_TRUE(realtime_subscriber->bars.front().items.front().has_flag(
        MarketDataFlags::REALTIME));
    EXPECT_FALSE(realtime_subscriber->bars.front().items.front().has_flag(
        MarketDataFlags::CATCHUP));
    EXPECT_TRUE(catchup_subscriber->bars.empty());

    provider.complete_history(make_history({100000}));
    ASSERT_EQ(catchup_subscriber->bars.size(), 2U);
    expect_history_delivery(catchup_subscriber->bars[0].items.front(), false);
    expect_live_delivery(catchup_subscriber->bars[1].items.front(), true);
}

TEST(MarketDataContinuity, RecoversTimestampGapBeforeReleasingLiveBatch) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({100000}));
    ASSERT_EQ(subscriber->bars.size(), 1U);

    provider.emit_live_bar(280000);
    EXPECT_EQ(provider.history_requests.size(), 2U);
    ASSERT_EQ(subscriber->continuity.size(), 4U);
    EXPECT_EQ(subscriber->continuity[2].status, MarketDataContinuityStatus::GAP_DETECTED);
    EXPECT_EQ(subscriber->continuity[3].status, MarketDataContinuityStatus::BACKFILLING);
    EXPECT_EQ(subscriber->bars.size(), 1U);

    provider.complete_history(make_history({160000, 220000}));

    ASSERT_EQ(subscriber->bars.size(), 3U);
    expect_history_delivery(subscriber->bars[1].items[0], true);
    expect_live_delivery(subscriber->bars[2].items[0], true);
    ASSERT_EQ(subscriber->continuity.size(), 5U);
    EXPECT_EQ(subscriber->continuity.back().status, MarketDataContinuityStatus::LIVE);
    EXPECT_EQ(
        subscriber->continuity[2].subscription.id,
        route.provider_subscription().id);
    EXPECT_EQ(
        subscriber->continuity[3].subscription.id,
        route.provider_subscription().id);
}

TEST(MarketDataContinuity, RejectsGapHistoryMissingRangeStart) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({100000}));

    provider.emit_live_bar(280000);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    provider.complete_history(make_history({220000}));

    ASSERT_EQ(subscriber->bars.size(), 2U);
    EXPECT_EQ(subscriber->bars[0].items.front().time_ms, 100000U);
    EXPECT_EQ(subscriber->bars[1].items.front().time_ms, 280000U);
    EXPECT_EQ(
        continuity_status_count(*subscriber, MarketDataContinuityStatus::LIVE),
        1U);
    ASSERT_GE(subscriber->continuity.size(), 2U);
    EXPECT_EQ(
        subscriber->continuity[subscriber->continuity.size() - 2].status,
        MarketDataContinuityStatus::FAILED);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);
}

TEST(MarketDataContinuity, RejectsGapHistoryMissingRangeEnd) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({100000}));

    provider.emit_live_bar(280000);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    provider.complete_history(make_history({160000}));

    ASSERT_EQ(subscriber->bars.size(), 2U);
    EXPECT_EQ(subscriber->bars[0].items.front().time_ms, 100000U);
    EXPECT_EQ(subscriber->bars[1].items.front().time_ms, 280000U);
    EXPECT_EQ(
        continuity_status_count(*subscriber, MarketDataContinuityStatus::LIVE),
        1U);
    ASSERT_GE(subscriber->continuity.size(), 2U);
    EXPECT_EQ(
        subscriber->continuity[subscriber->continuity.size() - 2].status,
        MarketDataContinuityStatus::FAILED);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);
}

TEST(MarketDataContinuity, LaterGapRepairDoesNotHideEarlierMissingRange) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({100000}));

    provider.emit_live_bar(280000);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    provider.complete_history(make_history({220000}));
    ASSERT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);

    const auto live_status_count = continuity_status_count(
        *subscriber,
        MarketDataContinuityStatus::LIVE);
    provider.emit_live_bar(400000);
    ASSERT_EQ(provider.history_requests.size(), 3U);
    EXPECT_EQ(provider.history_requests[2].from_ts, 340);
    EXPECT_EQ(provider.history_requests[2].to_ts, 340);

    provider.complete_history(make_history({340000}));

    EXPECT_EQ(
        continuity_status_count(
            *subscriber,
            MarketDataContinuityStatus::LIVE),
        live_status_count);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);
}

TEST(MarketDataContinuity, FailedPrefillKeepsLaterGapRecoveryDegraded) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.fail_history("initial history unavailable");
    ASSERT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);

    const auto live_status_count = continuity_status_count(
        *subscriber,
        MarketDataContinuityStatus::LIVE);
    provider.emit_live_bar(100000);
    provider.emit_live_bar(220000);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests[1].from_ts, 160);
    EXPECT_EQ(provider.history_requests[1].to_ts, 160);

    provider.complete_history(make_history({160000}));

    EXPECT_EQ(
        continuity_status_count(
            *subscriber,
            MarketDataContinuityStatus::LIVE),
        live_status_count);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);
}

TEST(MarketDataContinuity, ClipsGapHistoryToRequestedRange) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({100000}));

    provider.emit_live_bar(280000);
    provider.complete_history(make_history({100000, 160000, 220000, 280000}));

    ASSERT_EQ(subscriber->bars.size(), 3U);
    ASSERT_EQ(subscriber->bars[1].items.size(), 2U);
    EXPECT_EQ(subscriber->bars[1].items[0].time_ms, 160000U);
    EXPECT_EQ(subscriber->bars[1].items[1].time_ms, 220000U);
    EXPECT_EQ(subscriber->bars[2].items.front().time_ms, 280000U);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, ReportsActualBoundedGapRange) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER);
    request.continuity.max_backfill_bars = 2;

    auto route = router.subscribe_bars(provider, subscriber, request);
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({100000}));

    provider.emit_live_bar(400000);

    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests[1].from_ts, 160);
    EXPECT_EQ(provider.history_requests[1].to_ts, 220);
    ASSERT_FALSE(subscriber->continuity.empty());
    EXPECT_EQ(subscriber->continuity.back().from_time_ms, 160000U);
    EXPECT_EQ(subscriber->continuity.back().to_time_ms, 220000U);
    EXPECT_EQ(subscriber->continuity.back().requested_items, 2U);

    provider.complete_history(make_history({160000, 220000}));
    ASSERT_EQ(provider.history_requests.size(), 3U);
    EXPECT_EQ(provider.history_requests[2].from_ts, 280);
    EXPECT_EQ(provider.history_requests[2].to_ts, 340);
    provider.complete_history(make_history({280000, 340000}));

    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, RecoversLargeGapInBoundedChunks) {
    ScopedTestClock clock(60000ULL);
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(
        MarketDataContinuityMode::PREFILL_AND_RECOVER);
    request.continuity.max_backfill_bars = 2;

    auto route = router.subscribe_bars(provider, subscriber, request);
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({60000}));

    provider.emit_live_bar(300000);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests[1].from_ts, 120);
    EXPECT_EQ(provider.history_requests[1].to_ts, 180);
    provider.complete_history(make_history({120000, 180000}));

    ASSERT_EQ(provider.history_requests.size(), 3U);
    EXPECT_EQ(provider.history_requests[2].from_ts, 240);
    EXPECT_EQ(provider.history_requests[2].to_ts, 240);
    provider.complete_history(make_history({240000}));

    ASSERT_EQ(subscriber->bars.size(), 4U);
    EXPECT_EQ(subscriber->bars[0].items.front().time_ms, 60000U);
    EXPECT_EQ(subscriber->bars[1].items.front().time_ms, 120000U);
    EXPECT_EQ(subscriber->bars[2].items.front().time_ms, 240000U);
    EXPECT_EQ(subscriber->bars[3].items.front().time_ms, 300000U);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, ChecksGapsInsideBufferedBatchesInOrder) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());

    provider.emit_live_bars({160000, 280000});
    provider.emit_live_bar(400000);
    provider.complete_history(make_history({100000}));

    ASSERT_EQ(provider.history_requests.size(), 2U);
    ASSERT_EQ(subscriber->bars.size(), 2U);
    EXPECT_EQ(subscriber->bars[0].items[0].time_ms, 100000U);
    EXPECT_EQ(subscriber->bars[1].items[0].time_ms, 160000U);

    provider.complete_history(make_history({220000}));

    ASSERT_EQ(provider.history_requests.size(), 3U);
    ASSERT_EQ(subscriber->bars.size(), 4U);
    EXPECT_EQ(subscriber->bars[2].items[0].time_ms, 220000U);
    EXPECT_EQ(subscriber->bars[3].items[0].time_ms, 280000U);

    provider.complete_history(make_history({340000}));

    ASSERT_EQ(subscriber->bars.size(), 6U);
    EXPECT_EQ(subscriber->bars[4].items[0].time_ms, 340000U);
    EXPECT_EQ(subscriber->bars[5].items[0].time_ms, 400000U);
    expect_history_delivery(subscriber->bars[2].items[0], true);
    expect_live_delivery(subscriber->bars[3].items[0], true);
    expect_history_delivery(subscriber->bars[4].items[0], true);
    expect_live_delivery(subscriber->bars[5].items[0], true);
}

TEST(MarketDataContinuity, EmptyBackfillFailsOnceAndReleasesLiveBatch) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER);
    request.continuity.retry.max_attempts = 3;

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        request);
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({100000}));

    provider.emit_live_bar(280000);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    provider.complete_history({});

    ASSERT_EQ(provider.history_requests.size(), 2U);
    ASSERT_EQ(subscriber->bars.size(), 2U);
    EXPECT_EQ(subscriber->bars.back().items.front().time_ms, 280000U);
    expect_live_delivery(subscriber->bars.back().items.front(), true);
    ASSERT_EQ(subscriber->continuity.size(), 6U);
    EXPECT_EQ(subscriber->continuity[4].status, MarketDataContinuityStatus::FAILED);
    EXPECT_EQ(subscriber->continuity[5].status, MarketDataContinuityStatus::DEGRADED);
}

TEST(MarketDataContinuity, HistoryFailureKeepsLiveRouteUsable) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL));
    ASSERT_TRUE(route.active());

    provider.emit_live_bar(200000);
    provider.fail_history("history endpoint unavailable");

    ASSERT_EQ(subscriber->bars.size(), 1U);
    expect_live_delivery(subscriber->bars.front().items.front(), true);
    ASSERT_EQ(subscriber->continuity.size(), 3U);
    EXPECT_EQ(subscriber->continuity[1].status, MarketDataContinuityStatus::FAILED);
    EXPECT_EQ(subscriber->continuity[1].message, "history endpoint unavailable");
    EXPECT_EQ(subscriber->continuity[2].status, MarketDataContinuityStatus::DEGRADED);

    provider.emit_live_bar(260000);
    ASSERT_EQ(subscriber->bars.size(), 2U);
    expect_live_delivery(subscriber->bars.back().items.front(), false);
}

TEST(MarketDataContinuity, ShutdownWaitsForDeferredHistoryOperation) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL));
    ASSERT_TRUE(route.active());
    ASSERT_EQ(provider.history_requests.size(), 1U);

    router.shutdown();
    EXPECT_FALSE(router.is_shutdown_complete());

    provider.complete_history(make_history({100000}));
    router.process();

    EXPECT_TRUE(router.is_shutdown_complete());
    EXPECT_TRUE(subscriber->bars.empty());
    ASSERT_EQ(subscriber->continuity.size(), 1U);
    EXPECT_EQ(
        subscriber->continuity.front().status,
        MarketDataContinuityStatus::PREFILLING);
}

TEST(MarketDataContinuity, ShutdownWaitsForFailedHistoryOperation) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL));
    ASSERT_TRUE(route.active());

    router.shutdown();
    EXPECT_FALSE(router.is_shutdown_complete());

    provider.fail_history("history endpoint unavailable");
    router.process();

    EXPECT_TRUE(router.is_shutdown_complete());
    EXPECT_TRUE(subscriber->bars.empty());
    ASSERT_EQ(subscriber->continuity.size(), 1U);
    EXPECT_EQ(
        subscriber->continuity.front().status,
        MarketDataContinuityStatus::PREFILLING);
}

TEST(MarketDataContinuity, RejectsHistoryResponseFromAnotherStream) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());

    auto wrong_stream = make_history({100000});
    wrong_stream.symbol = "BTCUSDT";
    wrong_stream.timeframe = 300;
    provider.complete_history(std::move(wrong_stream));

    EXPECT_TRUE(subscriber->bars.empty());
    ASSERT_EQ(subscriber->continuity.size(), 3U);
    EXPECT_EQ(
        subscriber->continuity[1].status,
        MarketDataContinuityStatus::FAILED);
    EXPECT_EQ(
        subscriber->continuity[1].message,
        "Historical bar response does not match the subscribed stream.");
    EXPECT_EQ(
        subscriber->continuity[2].status,
        MarketDataContinuityStatus::DEGRADED);

    provider.emit_live_bar(280000);
    EXPECT_EQ(provider.history_requests.size(), 1U);
    ASSERT_EQ(subscriber->bars.size(), 1U);
    EXPECT_EQ(subscriber->bars.front().items.front().time_ms, 280000U);
}

TEST(MarketDataContinuity, ReconnectMarksRouteStale) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({60000}));

    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);

    ASSERT_FALSE(subscriber->statuses.empty());
    EXPECT_EQ(
        subscriber->statuses.back().status,
        MarketDataStreamStatus::DISCONNECTED);
    ASSERT_FALSE(subscriber->continuity.empty());
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::STALE);
    EXPECT_EQ(
        subscriber->continuity.back().subscription.id,
        route.provider_subscription().id);
}

TEST(MarketDataContinuity, ReconnectWaitsForReadyBeforeRequestingHistory) {
    ScopedTestClock clock(240000ULL);
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({240000}));

    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    market_data_continuity_test_clock::now_ms = 360000ULL;
    router.process();

    ASSERT_EQ(provider.history_requests.size(), 1U);
    ASSERT_FALSE(subscriber->continuity.empty());
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::STALE);

    provider.emit_status(MarketDataStreamStatus::READY);

    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests[1].from_ts, 300);
    EXPECT_EQ(provider.history_requests[1].to_ts, 300);
    provider.complete_history(make_history({300000}));
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, DisconnectDuringPrefillRestartsOriginalRange) {
    ScopedTestClock clock(240000ULL);
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(
        MarketDataContinuityMode::PREFILL_AND_RECOVER);
    request.continuity.prefill_bars = 3;

    auto route = router.subscribe_bars(provider, subscriber, request);
    ASSERT_TRUE(route.active());
    ASSERT_EQ(provider.history_requests.size(), 1U);
    EXPECT_EQ(provider.history_requests[0].from_ts, 120);
    EXPECT_EQ(provider.history_requests[0].to_ts, 240);

    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    router.process();
    EXPECT_EQ(provider.history_requests.size(), 1U);

    market_data_continuity_test_clock::now_ms = 360000ULL;
    provider.emit_status(MarketDataStreamStatus::READY);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests[1].from_ts, 120);
    EXPECT_EQ(provider.history_requests[1].to_ts, 240);

    provider.complete_history(make_history({120000, 180000, 240000}));
    EXPECT_TRUE(subscriber->bars.empty());
    EXPECT_NE(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);

    provider.complete_history(make_history({120000, 180000, 240000}));

    ASSERT_EQ(subscriber->bars.size(), 1U);
    ASSERT_EQ(subscriber->bars.front().items.size(), 3U);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, PrefillOnlyDoesNotRecoverAfterInitialization) {
    ScopedTestClock clock(60000ULL);
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({60000}));

    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    provider.emit_status(MarketDataStreamStatus::READY);
    router.process();

    EXPECT_EQ(provider.history_requests.size(), 1U);
    EXPECT_EQ(
        continuity_status_count(
            *subscriber,
            MarketDataContinuityStatus::STALE),
        0U);

    provider.emit_live_bar(120000);
    ASSERT_EQ(subscriber->bars.size(), 2U);
    expect_live_delivery(subscriber->bars.back().items.front(), false);
}

TEST(MarketDataContinuity, ReconnectRecoversOverlappingDirtyCurrentBar) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({60000}));

    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    provider.emit_live_bars({120000, 240000});
    ASSERT_EQ(subscriber->bars.size(), 1U);

    provider.emit_status(MarketDataStreamStatus::READY);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests[1].from_ts, 120);
    EXPECT_EQ(provider.history_requests[1].to_ts, 180);

    provider.complete_history(make_history({120000, 180000}));

    ASSERT_EQ(subscriber->bars.size(), 3U);
    EXPECT_EQ(subscriber->bars[0].items.front().time_ms, 60000U);
    EXPECT_EQ(subscriber->bars[1].items.front().time_ms, 120000U);
    ASSERT_EQ(subscriber->bars[1].items.size(), 2U);
    EXPECT_EQ(subscriber->bars[1].items[1].time_ms, 180000U);
    EXPECT_EQ(subscriber->bars[2].items.front().time_ms, 240000U);
    expect_history_delivery(subscriber->bars[1].items.front(), false);
    expect_history_delivery(subscriber->bars[1].items[1], false);
    expect_live_delivery(subscriber->bars[2].items.front(), true);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, ReconnectDoesNotTrustOldHistoryCompletion) {
    ScopedTestClock clock(60000ULL);
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.emit_live_bar(120000);

    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    market_data_continuity_test_clock::now_ms = 240000ULL;
    provider.emit_status(MarketDataStreamStatus::READY);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests[1].from_ts, 60);
    EXPECT_EQ(provider.history_requests[1].to_ts, 60);

    // This completes the pre-disconnect request. Its generation is obsolete.
    provider.complete_history(make_history({60000}));
    EXPECT_TRUE(subscriber->bars.empty());

    provider.complete_history(make_history({60000}));

    ASSERT_EQ(subscriber->bars.size(), 2U);
    EXPECT_EQ(subscriber->bars[0].items.front().time_ms, 60000U);
    EXPECT_EQ(subscriber->bars[1].items.front().time_ms, 120000U);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, ReconnectFailureLeavesRouteDegraded) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({60000}));

    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    provider.emit_live_bar(120000);
    provider.emit_status(MarketDataStreamStatus::READY);
    ASSERT_EQ(provider.history_requests.size(), 2U);

    provider.complete_history({});

    ASSERT_EQ(subscriber->bars.size(), 2U);
    expect_live_delivery(subscriber->bars.back().items.front(), true);
    ASSERT_GE(subscriber->continuity.size(), 2U);
    EXPECT_EQ(
        subscriber->continuity[subscriber->continuity.size() - 2].status,
        MarketDataContinuityStatus::FAILED);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);
    EXPECT_TRUE(route.active());

    provider.emit_live_bar(180000);
    ASSERT_EQ(subscriber->bars.size(), 3U);
    expect_live_delivery(subscriber->bars.back().items.front(), false);
}

TEST(MarketDataContinuity, PartialReconnectHistoryLeavesRouteDegraded) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({60000}));

    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    provider.emit_live_bar(120000);
    provider.emit_status(MarketDataStreamStatus::READY);
    ASSERT_EQ(provider.history_requests.size(), 2U);

    // The requested [120000, 180000] range is incomplete.
    provider.complete_history(make_history({120000}));

    ASSERT_EQ(subscriber->bars.size(), 2U);
    expect_history_delivery(subscriber->bars[0].items.front(), false);
    expect_live_delivery(subscriber->bars[1].items.front(), true);
    ASSERT_GE(subscriber->continuity.size(), 2U);
    EXPECT_EQ(
        subscriber->continuity[subscriber->continuity.size() - 2].status,
        MarketDataContinuityStatus::FAILED);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);
    EXPECT_TRUE(route.active());
}

TEST(MarketDataContinuity, PartialGapHistoryDoesNotRestoreContinuity) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({100000}));

    provider.emit_live_bar(280000);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests[1].from_ts, 160);
    EXPECT_EQ(provider.history_requests[1].to_ts, 220);

    provider.complete_history(make_history({160000}));

    ASSERT_EQ(subscriber->bars.size(), 2U);
    EXPECT_EQ(subscriber->bars.back().items.front().time_ms, 280000U);
    expect_live_delivery(subscriber->bars.back().items.front(), true);
    EXPECT_EQ(
        subscriber->continuity[subscriber->continuity.size() - 2].status,
        MarketDataContinuityStatus::FAILED);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);
}

TEST(MarketDataContinuity, LaterGapRepairDoesNotHideEarlierMissingRange) {
    ScopedTestClock clock(60000ULL);
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({60000}));

    provider.emit_live_bar(180000);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    provider.fail_history("missing 12:02 bar");
    ASSERT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);

    provider.emit_live_bar(240000);
    provider.emit_live_bar(360000);
    ASSERT_EQ(provider.history_requests.size(), 3U);
    EXPECT_EQ(provider.history_requests[2].from_ts, 300);
    EXPECT_EQ(provider.history_requests[2].to_ts, 300);

    const auto live_before_later_repair = continuity_status_count(
        *subscriber,
        MarketDataContinuityStatus::LIVE);
    provider.complete_history(make_history({300000}));

    EXPECT_EQ(
        continuity_status_count(
            *subscriber,
            MarketDataContinuityStatus::LIVE),
        live_before_later_repair);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::DEGRADED);

    market_data_continuity_test_clock::now_ms = 420000ULL;
    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    provider.emit_status(MarketDataStreamStatus::READY);
    ASSERT_EQ(provider.history_requests.size(), 4U);
    EXPECT_EQ(provider.history_requests[3].from_ts, 120);
    EXPECT_EQ(provider.history_requests[3].to_ts, 360);

    provider.complete_history(make_history(
        {120000, 180000, 240000, 300000, 360000}));
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, ReconnectClipsPaddedHistoryBeforeDeliveryAndDedup) {
    ScopedTestClock clock(60000ULL);
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({60000}));

    market_data_continuity_test_clock::now_ms = 240000ULL;
    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    provider.emit_live_bar(240000);
    provider.emit_status(MarketDataStreamStatus::READY);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests[1].from_ts, 120);
    EXPECT_EQ(provider.history_requests[1].to_ts, 180);

    provider.complete_history(
        make_history({60000, 120000, 180000, 240000}));

    ASSERT_EQ(subscriber->bars.size(), 3U);
    ASSERT_EQ(subscriber->bars[1].items.size(), 2U);
    EXPECT_EQ(subscriber->bars[1].items[0].time_ms, 120000U);
    EXPECT_EQ(subscriber->bars[1].items[1].time_ms, 180000U);
    EXPECT_EQ(subscriber->bars[2].items.front().time_ms, 240000U);
    expect_live_delivery(subscriber->bars[2].items.front(), true);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, ReconnectWaitsUntilDirtyCandleBoundary) {
    struct ClockReset {
        ~ClockReset() {
            market_data_continuity_test_clock::now_ms = 240000ULL;
        }
    } clock_reset;

    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER));
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({180000}));

    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    provider.emit_live_bar(240000);
    provider.emit_status(MarketDataStreamStatus::READY);

    EXPECT_EQ(provider.history_requests.size(), 1U);
    EXPECT_EQ(subscriber->bars.size(), 1U);

    router.process();
    EXPECT_EQ(provider.history_requests.size(), 1U);

    market_data_continuity_test_clock::now_ms = 300000ULL;
    router.process();
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests[1].from_ts, 240);
    EXPECT_EQ(provider.history_requests[1].to_ts, 240);

    provider.complete_history(make_history({240000}));

    ASSERT_EQ(subscriber->bars.size(), 2U);
    EXPECT_EQ(subscriber->bars.back().items.front().time_ms, 240000U);
    expect_history_delivery(subscriber->bars.back().items.front(), false);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, PrefillRequestUsesInclusiveBarCountRange) {
    BarSubscriptionRequest request(
        "EURUSD",
        60,
        BarPriceSource::MID,
        MarketDataTransport::WEBSOCKET);

    const auto history = MarketDataContinuityService::make_prefill_request(
        request,
        600013,
        3);

    EXPECT_EQ(history.from_ts, 480);
    EXPECT_EQ(history.to_ts, 600);
}

TEST(MarketDataContinuity, RetriesFailedHistoryBeforeReleasingLive) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(MarketDataContinuityMode::PREFILL);
    request.continuity.retry.max_attempts = 2;

    auto route = router.subscribe_bars(provider, subscriber, request);
    ASSERT_TRUE(route.active());

    provider.emit_live_bar(200000);
    provider.fail_history("temporary history failure");

    ASSERT_EQ(provider.history_requests.size(), 1U);
    EXPECT_TRUE(subscriber->bars.empty());
    ASSERT_EQ(subscriber->continuity.size(), 2U);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::RETRYING);

    router.process();
    ASSERT_EQ(provider.history_requests.size(), 2U);

    provider.complete_history(make_history({100000}));

    ASSERT_EQ(subscriber->bars.size(), 2U);
    expect_history_delivery(subscriber->bars[0].items[0], false);
    expect_live_delivery(subscriber->bars[1].items[0], true);
    ASSERT_EQ(subscriber->continuity.size(), 3U);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, RetriesRejectedHistoryBeforeReleasingLive) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(MarketDataContinuityMode::PREFILL);
    request.continuity.retry.max_attempts = 2;

    provider.reject_history = true;
    auto route = router.subscribe_bars(provider, subscriber, request);
    ASSERT_TRUE(route.active());

    provider.emit_live_bar(200000);
    ASSERT_EQ(provider.history_requests.size(), 1U);
    ASSERT_EQ(subscriber->continuity.size(), 2U);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::RETRYING);
    EXPECT_TRUE(subscriber->bars.empty());

    provider.reject_history = false;
    router.process();
    ASSERT_EQ(provider.history_requests.size(), 2U);
    provider.complete_history(make_history({100000}));

    ASSERT_EQ(subscriber->bars.size(), 2U);
    expect_history_delivery(subscriber->bars[0].items[0], false);
    expect_live_delivery(subscriber->bars[1].items[0], true);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, ProcessRunsOnlyOneRetryPass) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(MarketDataContinuityMode::PREFILL);
    request.continuity.retry.max_attempts = 3;

    provider.reject_history = true;
    auto route = router.subscribe_bars(provider, subscriber, request);
    ASSERT_TRUE(route.active());
    ASSERT_EQ(provider.history_requests.size(), 1U);

    router.process();

    EXPECT_EQ(provider.history_requests.size(), 2U);
    ASSERT_FALSE(subscriber->continuity.empty());
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::RETRYING);
}

TEST(MarketDataContinuity, GapRetryDoesNotRepeatGapDetectedStatus) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(MarketDataContinuityMode::PREFILL_AND_RECOVER);
    request.continuity.retry.max_attempts = 2;

    auto route = router.subscribe_bars(provider, subscriber, request);
    ASSERT_TRUE(route.active());
    provider.complete_history(make_history({100000}));

    provider.emit_live_bar(280000);
    provider.fail_history("temporary backfill failure");
    ASSERT_EQ(subscriber->continuity.size(), 5U);
    EXPECT_EQ(
        subscriber->continuity[2].status,
        MarketDataContinuityStatus::GAP_DETECTED);
    EXPECT_EQ(
        subscriber->continuity[4].status,
        MarketDataContinuityStatus::RETRYING);

    router.process();
    ASSERT_EQ(provider.history_requests.size(), 3U);
    ASSERT_EQ(subscriber->continuity.size(), 6U);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::BACKFILLING);

    provider.complete_history(make_history({160000, 220000}));

    const auto gap_detected_count = std::count_if(
        subscriber->continuity.begin(),
        subscriber->continuity.end(),
        [](const MarketDataContinuityUpdate& update) {
            return update.status == MarketDataContinuityStatus::GAP_DETECTED;
        });
    EXPECT_EQ(gap_detected_count, 1);
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::LIVE);
}

TEST(MarketDataContinuity, ExhaustedRetriesReleaseBufferedLiveOnce) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(MarketDataContinuityMode::PREFILL);
    request.continuity.retry.max_attempts = 2;

    auto route = router.subscribe_bars(provider, subscriber, request);
    ASSERT_TRUE(route.active());

    provider.emit_live_bar(200000);
    provider.fail_history("first failure");
    router.process();
    provider.fail_history("second failure");

    ASSERT_EQ(subscriber->bars.size(), 1U);
    expect_live_delivery(subscriber->bars.front().items.front(), true);
    ASSERT_EQ(subscriber->continuity.size(), 4U);
    EXPECT_EQ(
        subscriber->continuity[2].status,
        MarketDataContinuityStatus::FAILED);
    EXPECT_EQ(
        subscriber->continuity[3].status,
        MarketDataContinuityStatus::DEGRADED);
}

TEST(MarketDataContinuity, RetryBackoffDefersProviderRequest) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(MarketDataContinuityMode::PREFILL);
    request.continuity.retry.max_attempts = 2;
    request.continuity.retry.initial_backoff_ms = 60000;
    request.continuity.retry.max_backoff_ms = 60000;

    auto route = router.subscribe_bars(provider, subscriber, request);
    ASSERT_TRUE(route.active());
    provider.fail_history("temporary history failure");

    router.process();

    EXPECT_EQ(provider.history_requests.size(), 1U);
    ASSERT_FALSE(subscriber->continuity.empty());
    EXPECT_EQ(
        subscriber->continuity.back().status,
        MarketDataContinuityStatus::RETRYING);
}

TEST(MarketDataContinuity, BufferLimitReleasesLiveDataAndDisablesContinuity) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    auto request = continuity_request(MarketDataContinuityMode::PREFILL);
    request.continuity.max_buffered_batches = 1;
    request.continuity.max_buffered_items = 1;

    auto route = router.subscribe_bars(provider, subscriber, request);
    ASSERT_TRUE(route.active());

    provider.emit_live_bar(200000);
    EXPECT_TRUE(subscriber->bars.empty());
    provider.emit_live_bar(260000);

    ASSERT_EQ(subscriber->bars.size(), 2U);
    EXPECT_EQ(subscriber->bars[0].items.front().time_ms, 200000U);
    EXPECT_EQ(subscriber->bars[1].items.front().time_ms, 260000U);
    expect_live_delivery(subscriber->bars[0].items.front(), true);
    expect_live_delivery(subscriber->bars[1].items.front(), true);
    ASSERT_EQ(subscriber->continuity.size(), 3U);
    EXPECT_EQ(
        subscriber->continuity[1].status,
        MarketDataContinuityStatus::FAILED);
    EXPECT_EQ(
        subscriber->continuity[2].status,
        MarketDataContinuityStatus::DEGRADED);

    provider.complete_history(make_history({100000}));
    EXPECT_EQ(subscriber->bars.size(), 2U);

    provider.emit_live_bar(320000);
    ASSERT_EQ(subscriber->bars.size(), 3U);
    EXPECT_EQ(subscriber->bars.back().items.front().time_ms, 320000U);
    expect_live_delivery(subscriber->bars.back().items.front(), false);
}

TEST(MarketDataContinuity, UnsubscribeDuringHistoryDropsLateDelivery) {
    FakeHistoryProvider provider;
    MarketDataRouter router;
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL));
    ASSERT_TRUE(route.active());
    route.reset();

    provider.complete_history(make_history({100000}));

    EXPECT_TRUE(subscriber->bars.empty());
    EXPECT_EQ(router.subscription_count(), 0U);
}

TEST(MarketDataContinuity, RejectedOwnerDispatchDoesNotDeliverOnProviderThread) {
    FakeHistoryProvider provider;
    OwnerQueue owner_queue;
    MarketDataRouter router(
        [&owner_queue](MarketDataRouter::owner_task_t task) {
            return owner_queue.post(std::move(task));
        });
    auto subscriber = std::make_shared<RecordingSubscriber>();

    auto route = router.subscribe_bars(
        provider,
        subscriber,
        continuity_request(MarketDataContinuityMode::PREFILL));
    EXPECT_TRUE(route.valid());
    EXPECT_FALSE(route.active());

    owner_queue.drain();
    ASSERT_TRUE(route.active());
    provider.emit_live_bar(200000);
    owner_queue.drain();

    owner_queue.stop_accepting();
    provider.complete_history(make_history({100000}));

    EXPECT_TRUE(subscriber->bars.empty());
    ASSERT_EQ(subscriber->continuity.size(), 1U);
    EXPECT_EQ(
        subscriber->continuity.front().status,
        MarketDataContinuityStatus::PREFILLING);

    router.shutdown();
    EXPECT_TRUE(router.is_shutdown_complete());
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
