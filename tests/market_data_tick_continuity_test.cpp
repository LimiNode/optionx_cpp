#include <gtest/gtest.h>

#include <cstdint>

namespace market_data_tick_continuity_test_clock {
    inline std::uint64_t now_ms = 3000ULL;
}

#ifndef OPTIONX_TIMESTAMP_MS
#define OPTIONX_TIMESTAMP_MS market_data_tick_continuity_test_clock::now_ms
#endif

#include <algorithm>
#include <deque>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <optionx_cpp/market_data.hpp>

using namespace optionx;
using namespace optionx::market_data;

namespace {

class ScopedTestClock {
public:
    explicit ScopedTestClock(std::uint64_t now_ms) {
        market_data_tick_continuity_test_clock::now_ms = now_ms;
    }

    ~ScopedTestClock() {
        market_data_tick_continuity_test_clock::now_ms = 3000ULL;
    }
};

class FakeTickHistoryProvider final : public BaseMarketDataProvider {
public:
    std::uint64_t provider_now_ms = 0;
    std::uint64_t history_interval_ms = 0;

    ticks_callback_t& on_tick_data() override {
        return m_tick_callback;
    }

    status_callback_t& on_market_data_status() override {
        return m_status_callback;
    }

    std::uint64_t provider_time_ms() const noexcept override {
        return provider_now_ms;
    }

    std::uint64_t tick_history_interval_ms() const noexcept override {
        return history_interval_ms;
    }

    bool subscribe_ticks(
            TickSubscriptionRequest request,
            subscription_callback_t callback) override {
        m_active_subscription = MarketDataSubscriptionHandle::from_tick_request(
            provider_id(),
            m_next_subscription_id++,
            request);
        if (callback) {
            callback(MarketDataSubscriptionResult::subscribed(
                m_active_subscription));
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

    bool fetch_tick_history(
            const TickHistoryRequest& request,
            tick_history_callback_t callback) override {
        history_requests.push_back(request);
        m_history_callbacks.push_back(std::move(callback));
        return true;
    }

    void complete_history(
            TickSequence sequence,
            bool range_complete = true) {
        ASSERT_FALSE(m_history_callbacks.empty());
        auto callback = std::move(m_history_callbacks.front());
        m_history_callbacks.pop_front();
        callback(TickHistoryResult::ok(
            std::move(sequence),
            range_complete));
    }

    void fail_history(std::string message) {
        ASSERT_FALSE(m_history_callbacks.empty());
        auto callback = std::move(m_history_callbacks.front());
        m_history_callbacks.pop_front();
        callback(TickHistoryResult::fail(std::move(message)));
    }

    void emit_ticks(std::vector<Tick> ticks) {
        ASSERT_TRUE(static_cast<bool>(m_tick_callback));
        auto batch = std::make_unique<TickDataBatch>();
        batch->subscription = m_active_subscription;
        batch->type = MarketDataType::TICKS;
        batch->symbol = m_active_subscription.symbol;
        batch->price_digits = 5;
        batch->items = std::move(ticks);
        for (auto& tick : batch->items) {
            mark_live_payload(tick.flags);
        }
        m_tick_callback(std::move(batch));
    }

    void emit_status(MarketDataStreamStatus status) {
        ASSERT_TRUE(static_cast<bool>(m_status_callback));
        MarketDataStatusUpdate update;
        update.subscription = m_active_subscription;
        update.type = MarketDataType::TICKS;
        update.symbol = m_active_subscription.symbol;
        update.transport = m_active_subscription.transport;
        update.status = status;
        m_status_callback(std::move(update));
    }

    const MarketDataSubscriptionHandle& active_subscription() const noexcept {
        return m_active_subscription;
    }

    std::vector<TickHistoryRequest> history_requests;

private:
    SubscriptionId m_next_subscription_id = 1;
    MarketDataSubscriptionHandle m_active_subscription;
    std::deque<tick_history_callback_t> m_history_callbacks;
    ticks_callback_t m_tick_callback;
    status_callback_t m_status_callback;
};

class RecordingSubscriber final : public IMarketDataSubscriber {
public:
    void on_tick_data(const TickDataBatch& batch) override {
        ticks.push_back(batch);
    }

    void on_market_data_continuity(
            const MarketDataContinuityUpdate& update) override {
        continuity.push_back(update);
    }

    std::vector<TickDataBatch> ticks;
    std::vector<MarketDataContinuityUpdate> continuity;
};

Tick make_tick(
        std::uint64_t time_ms,
        double price = 1.0,
        std::uint64_t received_ms = 0) {
    Tick tick;
    tick.ask = price + 0.01;
    tick.bid = price;
    tick.last = price;
    tick.volume = 1.0;
    tick.time_ms = time_ms;
    tick.received_ms = received_ms;
    return tick;
}

TickSequence make_history(std::initializer_list<std::uint64_t> times) {
    TickSequence sequence;
    sequence.symbol = "EURUSD";
    sequence.provider = "INTRADE_BAR";
    sequence.price_digits = 5;
    for (const auto time_ms : times) {
        sequence.ticks.push_back(make_tick(time_ms));
    }
    return sequence;
}

TickSubscriptionRequest continuity_request(
        MarketDataContinuityMode mode =
            MarketDataContinuityMode::PREFILL_AND_RECOVER,
        std::uint64_t prefill_lookback_ms = 2000,
        std::uint64_t max_backfill_ms = 60000) {
    TickSubscriptionRequest request("EURUSD");
    request.continuity.mode = mode;
    request.continuity.prefill_lookback_ms = prefill_lookback_ms;
    request.continuity.expected_interval_ms = 1000;
    request.continuity.max_backfill_ms = max_backfill_ms;
    request.continuity.retry.max_attempts = 2;
    request.continuity.retry.initial_backoff_ms = 0;
    return request;
}

std::size_t count_status(
        const RecordingSubscriber& subscriber,
        MarketDataContinuityStatus status) {
    return static_cast<std::size_t>(std::count_if(
        subscriber.continuity.begin(),
        subscriber.continuity.end(),
        [status](const MarketDataContinuityUpdate& update) {
            return update.status == status;
        }));
}

const Tick& only_tick(const TickDataBatch& batch) {
    EXPECT_EQ(batch.items.size(), 1U);
    return batch.items.front();
}

TEST(MarketDataTickContinuity, CompletesPrefillAndDeliversDirectRealtime) {
    ScopedTestClock clock(3000);
    FakeTickHistoryProvider provider;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request());
    ASSERT_TRUE(route.valid());
    ASSERT_EQ(provider.history_requests.size(), 1U);
    EXPECT_EQ(provider.history_requests.front().from_time_ms, 1000U);
    EXPECT_EQ(provider.history_requests.front().to_time_ms, 3000U);

    provider.complete_history(make_history({1000, 2000, 3000}));
    ASSERT_EQ(subscriber->ticks.size(), 1U);
    ASSERT_EQ(subscriber->ticks.front().items.size(), 3U);
    for (const auto& tick : subscriber->ticks.front().items) {
        EXPECT_TRUE(tick.has_flag(MarketDataFlags::HISTORICAL));
        EXPECT_FALSE(tick.has_flag(MarketDataFlags::LIVE_SOURCE));
    }
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::LIVE), 1U);

    provider.emit_ticks({make_tick(4000)});
    ASSERT_EQ(subscriber->ticks.size(), 2U);
    const auto& live_tick = only_tick(subscriber->ticks.back());
    EXPECT_EQ(live_tick.time_ms, 4000U);
    EXPECT_TRUE(live_tick.has_flag(MarketDataFlags::LIVE_SOURCE));
    EXPECT_TRUE(live_tick.has_flag(MarketDataFlags::REALTIME));
    EXPECT_FALSE(live_tick.has_flag(MarketDataFlags::CATCHUP));

    const auto snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->phase, MarketDataContinuityPhase::LIVE);
    EXPECT_EQ(snapshot->verified_through_time_ms, 4000U);
    EXPECT_EQ(snapshot->unverified_from_time_ms, 0U);
}

TEST(MarketDataTickContinuity, UsesProviderAlignedClockForPrefill) {
    ScopedTestClock clock(3123);
    FakeTickHistoryProvider provider;
    provider.provider_now_ms = 3000;
    provider.history_interval_ms = 1000;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request());
    ASSERT_TRUE(route.valid());
    ASSERT_EQ(provider.history_requests.size(), 1U);
    EXPECT_EQ(provider.history_requests.front().from_time_ms, 1000U);
    EXPECT_EQ(provider.history_requests.front().to_time_ms, 3000U);

    provider.complete_history(make_history({1000, 2000, 3000}));
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::LIVE), 1U);
}

TEST(MarketDataTickContinuity, IncompletePrefillStaysDegraded) {
    ScopedTestClock clock(3000);
    FakeTickHistoryProvider provider;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request());
    ASSERT_TRUE(route.valid());
    provider.complete_history(make_history({1000, 3000}), false);

    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::LIVE), 0U);
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::FAILED), 1U);
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::DEGRADED), 1U);

    const auto snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->phase, MarketDataContinuityPhase::DEGRADED);
    EXPECT_EQ(snapshot->unverified_from_time_ms, 1000U);
    EXPECT_TRUE(snapshot->enabled);
}

TEST(MarketDataTickContinuity, RestartsInterruptedPrefillFromOriginalRange) {
    ScopedTestClock clock(3000);
    FakeTickHistoryProvider provider;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request());
    ASSERT_TRUE(route.valid());
    ASSERT_EQ(provider.history_requests.size(), 1U);
    EXPECT_EQ(provider.history_requests.front().from_time_ms, 1000U);
    EXPECT_EQ(provider.history_requests.front().to_time_ms, 3000U);

    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::STALE), 1U);

    market_data_tick_continuity_test_clock::now_ms = 6000;
    provider.emit_status(MarketDataStreamStatus::READY);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 1000U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 6000U);

    // The completion from before the transport loss is obsolete and must not
    // satisfy the restarted request.
    provider.complete_history(make_history({1000, 2000, 3000}));
    EXPECT_TRUE(subscriber->ticks.empty());
    auto snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->request_in_flight);

    provider.complete_history(make_history({1000, 2000, 3000, 4000, 5000, 6000}));
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::LIVE), 1U);
    snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->phase, MarketDataContinuityPhase::LIVE);
    EXPECT_EQ(snapshot->unverified_from_time_ms, 0U);
}

TEST(MarketDataTickContinuity, ChecksGapsInsidePrefillBacklog) {
    ScopedTestClock clock(3000);
    FakeTickHistoryProvider provider;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request());
    ASSERT_TRUE(route.valid());
    provider.emit_ticks({make_tick(4000), make_tick(6000)});
    ASSERT_EQ(provider.history_requests.size(), 1U);

    provider.complete_history(make_history({1000, 2000, 3000}));
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 4000U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 6000U);
    provider.complete_history(make_history({4000, 5000}));

    ASSERT_FALSE(subscriber->ticks.empty());
    const auto& prefix = subscriber->ticks[1];
    ASSERT_EQ(prefix.items.size(), 1U);
    EXPECT_EQ(prefix.items.front().time_ms, 4000U);
    EXPECT_TRUE(prefix.items.front().has_flag(MarketDataFlags::CATCHUP));
    const auto& tail = subscriber->ticks.back();
    ASSERT_EQ(tail.items.size(), 1U);
    EXPECT_EQ(tail.items.front().time_ms, 6000U);
    EXPECT_TRUE(tail.items.front().has_flag(MarketDataFlags::CATCHUP));

    const auto snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->phase, MarketDataContinuityPhase::LIVE);
    EXPECT_EQ(snapshot->unverified_from_time_ms, 0U);
}

TEST(MarketDataTickContinuity, RepairsGapAndPreservesDistinctSameSecondTicks) {
    ScopedTestClock clock(3000);
    FakeTickHistoryProvider provider;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request());
    ASSERT_TRUE(route.valid());
    provider.complete_history(make_history({1000, 2000, 3000}));

    provider.emit_ticks({make_tick(4000)});
    Tick first = make_tick(6000, 1.0);
    Tick second = make_tick(6000, 1.1);
    provider.emit_ticks({first, second});

    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 4000U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 6000U);
    const auto before_repair_ticks = subscriber->ticks.size();

    provider.complete_history(make_history({4000, 5000}));
    ASSERT_EQ(subscriber->ticks.size(), before_repair_ticks + 2U);
    const auto& history_batch = subscriber->ticks[before_repair_ticks];
    ASSERT_EQ(history_batch.items.size(), 2U);
    EXPECT_TRUE(history_batch.items[0].has_flag(MarketDataFlags::HISTORICAL));
    EXPECT_TRUE(history_batch.items[1].has_flag(MarketDataFlags::HISTORICAL));
    const auto& catchup_batch = subscriber->ticks[before_repair_ticks + 1U];
    ASSERT_EQ(catchup_batch.items.size(), 2U);
    EXPECT_EQ(catchup_batch.items[0].time_ms, 6000U);
    EXPECT_EQ(catchup_batch.items[1].time_ms, 6000U);
    EXPECT_NE(catchup_batch.items[0].bid, catchup_batch.items[1].bid);
    EXPECT_TRUE(catchup_batch.items[0].has_flag(MarketDataFlags::CATCHUP));
    EXPECT_TRUE(catchup_batch.items[1].has_flag(MarketDataFlags::CATCHUP));
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::LIVE), 2U);
}

TEST(MarketDataTickContinuity, PreservesNonGridTriggeringTickDuringRecovery) {
    ScopedTestClock clock(1000);
    FakeTickHistoryProvider provider;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request());
    ASSERT_TRUE(route.valid());
    provider.complete_history(make_history({1000}));

    provider.emit_ticks({make_tick(2501)});
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 1000U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 2501U);

    provider.complete_history(make_history({1000, 2000}));

    bool saw_triggering_tick = false;
    for (const auto& batch : subscriber->ticks) {
        for (const auto& tick : batch.items) {
            if (tick.time_ms == 2501U) saw_triggering_tick = true;
        }
    }
    EXPECT_TRUE(saw_triggering_tick);
}

TEST(MarketDataTickContinuity, StopsProviderGridRecoveryAtCompletedBoundary) {
    ScopedTestClock clock(1000);
    FakeTickHistoryProvider provider;
    provider.history_interval_ms = 1000;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request());
    ASSERT_TRUE(route.valid());
    ASSERT_EQ(provider.history_requests.size(), 1U);
    provider.complete_history(make_history({1000}));

    provider.emit_ticks({make_tick(2501)});
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 1000U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 2000U);

    provider.complete_history(make_history({1000, 2000}));
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::LIVE), 2U);
    bool saw_triggering_tick = false;
    for (const auto& batch : subscriber->ticks) {
        for (const auto& tick : batch.items) {
            if (tick.time_ms == 2501U) saw_triggering_tick = true;
        }
    }
    EXPECT_TRUE(saw_triggering_tick);
}

TEST(MarketDataTickContinuity, KeepsBoundedProviderGridChunksOverlapped) {
    ScopedTestClock clock(1000);
    FakeTickHistoryProvider provider;
    provider.history_interval_ms = 1000;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request(
            MarketDataContinuityMode::PREFILL_AND_RECOVER,
            1,
            1500));
    ASSERT_TRUE(route.valid());
    provider.complete_history(make_history({1000}));
    provider.emit_ticks({make_tick(4501)});

    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 1000U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 2000U);
    provider.complete_history(make_history({1000, 2000}));

    ASSERT_EQ(provider.history_requests.size(), 3U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 2000U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 3000U);
    provider.complete_history(make_history({2000, 3000}));

    ASSERT_EQ(provider.history_requests.size(), 4U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 3000U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 4000U);
    provider.complete_history(make_history({3000, 4000}));

    ASSERT_EQ(provider.history_requests.size(), 4U);
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::LIVE), 2U);

    bool saw_triggering_tick = false;
    for (const auto& batch : subscriber->ticks) {
        for (const auto& tick : batch.items) {
            if (tick.time_ms == 4501U) saw_triggering_tick = true;
        }
    }
    EXPECT_TRUE(saw_triggering_tick);
}

TEST(MarketDataTickContinuity, UsesBoundedGapRequests) {
    ScopedTestClock clock(10000);
    FakeTickHistoryProvider provider;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto request = continuity_request(
        MarketDataContinuityMode::PREFILL_AND_RECOVER,
        0,
        3000);
    auto route = router.subscribe_ticks(provider, subscriber, request);
    ASSERT_TRUE(route.valid());
    provider.emit_ticks({make_tick(1000)});
    provider.emit_ticks({make_tick(10000)});

    ASSERT_EQ(provider.history_requests.size(), 1U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 1000U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 3999U);
    provider.complete_history(make_history({1000, 2000, 3000, 3999}));
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 3999U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 6998U);
    provider.complete_history(make_history({3999, 5000, 6000, 6998}));
    ASSERT_EQ(provider.history_requests.size(), 3U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 6998U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 9997U);
    provider.complete_history(make_history({6998, 8000, 9000, 9997}));
    ASSERT_EQ(provider.history_requests.size(), 4U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 9997U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 10000U);
    provider.complete_history(make_history({9997, 10000}));

    const auto snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->phase, MarketDataContinuityPhase::LIVE);
    EXPECT_EQ(snapshot->unverified_from_time_ms, 0U);
    EXPECT_EQ(snapshot->history_request_count, 4U);

    for (std::size_t index = 1; index < provider.history_requests.size(); ++index) {
        EXPECT_EQ(
            provider.history_requests[index].from_time_ms,
            provider.history_requests[index - 1U].to_time_ms);
    }
}

TEST(MarketDataTickContinuity, RetriesFailedHistoryFromProcess) {
    ScopedTestClock clock(3000);
    FakeTickHistoryProvider provider;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request());
    ASSERT_TRUE(route.valid());
    provider.fail_history("temporary history failure");
    EXPECT_EQ(provider.history_requests.size(), 1U);

    auto snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->last_status, MarketDataContinuityStatus::RETRYING);
    EXPECT_EQ(snapshot->retry_count, 0U);

    router.process();
    ASSERT_EQ(provider.history_requests.size(), 2U);
    provider.complete_history(make_history({1000, 2000, 3000}));
    snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->phase, MarketDataContinuityPhase::LIVE);
    EXPECT_EQ(snapshot->retry_count, 1U);
}

TEST(MarketDataTickContinuity, ReconnectDeduplicatesOnlyExactOverlap) {
    ScopedTestClock clock(3000);
    FakeTickHistoryProvider provider;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        continuity_request());
    ASSERT_TRUE(route.valid());
    provider.complete_history(make_history({1000, 2000, 3000}));
    provider.emit_ticks({make_tick(3000)});
    provider.emit_status(MarketDataStreamStatus::DISCONNECTED);
    provider.emit_ticks({make_tick(5000, 1.0), make_tick(5000, 1.1)});

    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::STALE), 1U);
    auto stale_snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(stale_snapshot.has_value());
    EXPECT_EQ(stale_snapshot->phase, MarketDataContinuityPhase::WAITING_FOR_READY);
    EXPECT_FALSE(stale_snapshot->request_in_flight);
    EXPECT_EQ(provider.history_requests.size(), 1U);

    provider.emit_status(MarketDataStreamStatus::READY);
    ASSERT_EQ(provider.history_requests.size(), 2U);
    EXPECT_EQ(provider.history_requests.back().from_time_ms, 3000U);
    EXPECT_EQ(provider.history_requests.back().to_time_ms, 5000U);

    TickSequence history = make_history({3000, 4000});
    history.ticks.push_back(make_tick(5000, 1.0));
    provider.complete_history(std::move(history));

    ASSERT_FALSE(subscriber->ticks.empty());
    const auto& catchup_batch = subscriber->ticks.back();
    ASSERT_EQ(catchup_batch.items.size(), 1U);
    EXPECT_EQ(catchup_batch.items.front().time_ms, 5000U);
    EXPECT_DOUBLE_EQ(catchup_batch.items.front().bid, 1.1);
    EXPECT_TRUE(catchup_batch.items.front().has_flag(MarketDataFlags::CATCHUP));

    const auto snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->phase, MarketDataContinuityPhase::LIVE);
    EXPECT_EQ(snapshot->unverified_from_time_ms, 0U);
}

TEST(MarketDataTickContinuity, DisablesContinuityAfterBufferOverflow) {
    ScopedTestClock clock(3000);
    FakeTickHistoryProvider provider;
    auto subscriber = std::make_shared<RecordingSubscriber>();
    MarketDataRouter router;

    auto request = continuity_request();
    request.continuity.max_buffered_batches = 1;
    request.continuity.max_buffered_items = 2;
    auto route = router.subscribe_ticks(provider, subscriber, request);
    ASSERT_TRUE(route.valid());

    provider.emit_ticks({make_tick(1000)});
    provider.emit_ticks({make_tick(2000)});
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::FAILED), 1U);
    EXPECT_EQ(count_status(*subscriber, MarketDataContinuityStatus::DEGRADED), 1U);

    const auto snapshot = router.continuity_snapshot(route.router_id());
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_FALSE(snapshot->enabled);
    EXPECT_EQ(snapshot->phase, MarketDataContinuityPhase::DEGRADED);

    provider.complete_history(make_history({1000, 2000, 3000}));
    provider.emit_ticks({make_tick(4000)});
    ASSERT_FALSE(subscriber->ticks.empty());
    const auto& live_tick = only_tick(subscriber->ticks.back());
    EXPECT_TRUE(live_tick.has_flag(MarketDataFlags::REALTIME));
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
