#include <gtest/gtest.h>

#include <functional>
#include <initializer_list>
#include <utility>

#include <optionx_cpp/market_data.hpp>

using namespace optionx;
using namespace optionx::market_data;

namespace {

class FakeTickHistoryProvider final : public BaseMarketDataProvider {
public:
    bool fetch_tick_history(
            const TickHistoryRequest& request,
            tick_history_callback_t callback) override {
        last_request = request;
        if (!callback) return false;
        callback(TickHistoryResult::ok(
            std::move(next_sequence),
            next_range_complete,
            next_status_code));
        return true;
    }

    TickHistoryRequest last_request;
    TickSequence next_sequence;
    bool next_range_complete = true;
    long next_status_code = TickHistoryResult::NO_HTTP_STATUS;
};

TickSequence make_ticks(std::initializer_list<std::uint64_t> times) {
    TickSequence sequence;
    sequence.symbol = "EURUSD";
    sequence.provider = "fake";
    sequence.price_digits = 5;
    sequence.volume_digits = 0;
    for (const auto time_ms : times) {
        Tick tick;
        tick.ask = 1.2;
        tick.bid = 1.1;
        tick.time_ms = time_ms;
        sequence.ticks.push_back(tick);
    }
    return sequence;
}

TEST(MarketDataTickHistoryContract, ValidatesInclusiveMillisecondRange) {
    EXPECT_TRUE(TickHistoryRequest("EURUSD", 1000, 2000).valid());
    EXPECT_FALSE(TickHistoryRequest("", 1000, 2000).valid());
    EXPECT_FALSE(TickHistoryRequest("EURUSD", 0, 2000).valid());
    EXPECT_FALSE(TickHistoryRequest("EURUSD", 2000, 1000).valid());
}

TEST(MarketDataTickHistoryContract, ProviderHookReturnsTypedResult) {
    FakeTickHistoryProvider provider;
    provider.next_sequence = make_ticks({1000, 2000});
    TickHistoryResult result;

    const auto request = TickHistoryRequest("EURUSD", 1000, 2000);
    ASSERT_TRUE(provider.fetch_tick_history(
        request,
        [&result](TickHistoryResult update) {
            result = std::move(update);
        }));

    EXPECT_EQ(provider.last_request.symbol, "EURUSD");
    EXPECT_EQ(provider.last_request.from_time_ms, 1000U);
    EXPECT_EQ(provider.last_request.to_time_ms, 2000U);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.range_complete);
    EXPECT_EQ(result.sequence.ticks.size(), 2U);
}

TEST(MarketDataTickHistoryContract, ServiceBuildsHistoricalTickBatch) {
    FakeTickHistoryProvider provider;
    provider.next_sequence = make_ticks({1000, 2000});
    MarketDataContinuityService service(provider);
    const auto request = TickHistoryRequest("EURUSD", 1000, 2000);
    const auto subscription = MarketDataSubscriptionHandle::from_tick_request(
        1,
        7,
        TickSubscriptionRequest("EURUSD"));
    std::unique_ptr<TickDataBatch> batch;
    TickHistoryResult failure;

    ASSERT_TRUE(service.request_tick_history_batch(
        request,
        subscription,
        [&batch](std::unique_ptr<TickDataBatch> update) {
            batch = std::move(update);
        },
        [&failure](TickHistoryResult result) {
            failure = std::move(result);
        },
        true));

    ASSERT_TRUE(batch);
    EXPECT_TRUE(failure.error_desc.empty());
    EXPECT_EQ(batch->type, MarketDataType::TICKS);
    EXPECT_EQ(batch->subscription.id, 7U);
    EXPECT_EQ(batch->symbol, "EURUSD");
    ASSERT_EQ(batch->items.size(), 2U);
    for (const auto& tick : batch->items) {
        EXPECT_TRUE(tick.has_flag(MarketDataFlags::HISTORICAL));
        EXPECT_TRUE(tick.has_flag(MarketDataFlags::BACKFILL));
        EXPECT_FALSE(tick.has_flag(MarketDataFlags::LIVE_SOURCE));
        EXPECT_TRUE(market_data_flags_valid(tick.flags));
    }
}

TEST(MarketDataTickHistoryContract, ServiceRejectsIncompleteRangeByDefault) {
    FakeTickHistoryProvider provider;
    provider.next_sequence = make_ticks({1500});
    provider.next_range_complete = false;
    MarketDataContinuityService service(provider);
    std::unique_ptr<TickDataBatch> batch;
    TickHistoryResult failure;

    ASSERT_TRUE(service.request_tick_history_batch(
        TickHistoryRequest("EURUSD", 1000, 2000),
        {},
        [&batch](std::unique_ptr<TickDataBatch> update) {
            batch = std::move(update);
        },
        [&failure](TickHistoryResult result) {
            failure = std::move(result);
        }));

    EXPECT_FALSE(batch);
    EXPECT_FALSE(failure.success);
    EXPECT_NE(failure.error_desc.find("complete"), std::string::npos);
}

TEST(MarketDataTickHistoryContract, ServiceDeliversIncompleteRangeAsObservations) {
    FakeTickHistoryProvider provider;
    provider.next_sequence = make_ticks({1500});
    provider.next_range_complete = false;
    MarketDataContinuityService service(provider);
    std::unique_ptr<TickDataBatch> batch;
    TickHistoryResult failure;

    ASSERT_TRUE(service.request_tick_history_batch(
        TickHistoryRequest("EURUSD", 1000, 2000),
        {},
        [&batch](std::unique_ptr<TickDataBatch> update) {
            batch = std::move(update);
        },
        [&failure](TickHistoryResult result) {
            failure = std::move(result);
        },
        false,
        false));

    ASSERT_TRUE(batch);
    ASSERT_EQ(batch->items.size(), 1U);
    EXPECT_EQ(batch->items.front().time_ms, 1500U);
    EXPECT_TRUE(failure.error_desc.empty());
}

TEST(MarketDataTickHistoryContract, ServiceAllowsAuthoritativeEmptyRange) {
    FakeTickHistoryProvider provider;
    provider.next_sequence = make_ticks({});
    provider.next_range_complete = true;
    MarketDataContinuityService service(provider);
    std::unique_ptr<TickDataBatch> batch;
    TickHistoryResult failure;

    ASSERT_TRUE(service.request_tick_history_batch(
        TickHistoryRequest("EURUSD", 1000, 2000),
        {},
        [&batch](std::unique_ptr<TickDataBatch> update) {
            batch = std::move(update);
        },
        [&failure](TickHistoryResult result) {
            failure = std::move(result);
        }));

    ASSERT_TRUE(batch);
    EXPECT_TRUE(batch->empty());
    EXPECT_TRUE(failure.error_desc.empty());
}

TEST(MarketDataTickHistoryContract, ServiceRejectsForeignSymbol) {
    FakeTickHistoryProvider provider;
    provider.next_sequence = make_ticks({1000, 2000});
    provider.next_sequence.symbol = "GBPUSD";
    provider.next_status_code = 422;
    MarketDataContinuityService service(provider);
    std::unique_ptr<TickDataBatch> batch;
    TickHistoryResult failure;

    ASSERT_TRUE(service.request_tick_history_batch(
        TickHistoryRequest("EURUSD", 1000, 2000),
        {},
        [&batch](std::unique_ptr<TickDataBatch> update) {
            batch = std::move(update);
        },
        [&failure](TickHistoryResult result) {
            failure = std::move(result);
        }));

    EXPECT_FALSE(batch);
    EXPECT_FALSE(failure.success);
    EXPECT_EQ(failure.status_code, 422);
    EXPECT_NE(failure.error_desc.find("symbol"), std::string::npos);
}

TEST(MarketDataTickHistoryContract, ServiceRejectsUnorderedOrOutOfRangeTicks) {
    FakeTickHistoryProvider provider;
    provider.next_sequence = make_ticks({2000, 1000});
    MarketDataContinuityService service(provider);
    std::unique_ptr<TickDataBatch> batch;
    TickHistoryResult failure;

    ASSERT_TRUE(service.request_tick_history_batch(
        TickHistoryRequest("EURUSD", 1000, 2000),
        {},
        [&batch](std::unique_ptr<TickDataBatch> update) {
            batch = std::move(update);
        },
        [&failure](TickHistoryResult result) {
            failure = std::move(result);
        }));

    EXPECT_FALSE(batch);
    EXPECT_FALSE(failure.success);
    EXPECT_NE(failure.error_desc.find("outside"), std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
