#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

#include <optionx_cpp/market_data.hpp>

using namespace optionx;
using namespace optionx::market_data;

namespace {

class FakeHistoryProvider final : public BaseMarketDataProvider {
public:
    BarSequence next_sequence;
    BarHistoryResult next_result = BarHistoryResult::fail("not configured");
    BarHistoryRequest last_request;

    bool fetch_bar_history(
            const BarHistoryRequest& request,
            bar_history_callback_t callback) override {
        last_request = request;
        if (callback) {
            if (next_result) {
                callback(BarHistoryResult::ok(next_sequence, next_result.status_code));
            } else {
                callback(next_result);
            }
        }
        return true;
    }
};

} // namespace

TEST(TickSubscriptionRequest, BuildsAndValidatesTickRequests) {
    const TickSubscriptionRequest request("EUR/USD", MarketDataTransport::WEBSOCKET);

    EXPECT_TRUE(request.valid());
    EXPECT_EQ(request.symbol, "EUR/USD");
    EXPECT_EQ(request.transport, MarketDataTransport::WEBSOCKET);
    EXPECT_EQ(to_str(request.transport), std::string("WEBSOCKET"));

    EXPECT_FALSE(TickSubscriptionRequest("").valid());
}

TEST(BarSubscriptionRequest, BuildsAndValidatesBarRequests) {
    const BarSubscriptionRequest request(
        "BTCUSDT",
        86400,
        BarPriceSource::LAST,
        MarketDataTransport::HYBRID);

    EXPECT_TRUE(request.valid());
    EXPECT_EQ(request.symbol, "BTCUSDT");
    EXPECT_EQ(request.timeframe, 86400);
    EXPECT_EQ(request.price_source, BarPriceSource::LAST);
    EXPECT_EQ(request.transport, MarketDataTransport::HYBRID);

    EXPECT_FALSE(BarSubscriptionRequest("EUR/USD", 0).valid());
    EXPECT_FALSE(BarSubscriptionRequest("EUR/USD", -16).valid());
    EXPECT_FALSE(BarSubscriptionRequest("", 60).valid());
}

TEST(MarketDataSubscriptionHandle, BuildsTickAndBarHandlesAndReportsValidity) {
    const TickSubscriptionRequest tick_request("EUR/USD", MarketDataTransport::POLLING);
    const auto tick_handle =
        MarketDataSubscriptionHandle::from_tick_request(11, 42, tick_request);

    EXPECT_TRUE(tick_handle.valid());
    EXPECT_EQ(tick_handle.provider_id, 11u);
    EXPECT_EQ(tick_handle.id, 42u);
    EXPECT_EQ(tick_handle.symbol, tick_request.symbol);
    EXPECT_EQ(tick_handle.stream_type, MarketDataType::TICKS);
    EXPECT_EQ(tick_handle.timeframe, 0);
    EXPECT_EQ(tick_handle.transport, tick_request.transport);

    const BarSubscriptionRequest bar_request("BTCUSDT", 86400, BarPriceSource::LAST);
    const auto bar_handle =
        MarketDataSubscriptionHandle::from_bar_request(11, 43, bar_request);

    EXPECT_TRUE(bar_handle.valid());
    EXPECT_EQ(bar_handle.provider_id, 11u);
    EXPECT_EQ(bar_handle.id, 43u);
    EXPECT_EQ(bar_handle.symbol, bar_request.symbol);
    EXPECT_EQ(bar_handle.stream_type, MarketDataType::BARS);
    EXPECT_EQ(bar_handle.timeframe, 86400);
    EXPECT_EQ(bar_handle.price_source, bar_request.price_source);

    EXPECT_FALSE(MarketDataSubscriptionHandle{}.valid());
}

TEST(MarketDataContinuityStatus, ConvertsReconnectStatusesToStableText) {
    EXPECT_STREQ(to_str(MarketDataContinuityStatus::STALE), "STALE");
    EXPECT_STREQ(to_str(MarketDataContinuityStatus::DEGRADED), "DEGRADED");
}

TEST(MarketDataSubscriptionResult, DerivesSuccessFromStatus) {
    const TickSubscriptionRequest request("EUR/USD");
    const auto handle =
        MarketDataSubscriptionHandle::from_tick_request(3, 7, request);

    const auto subscribed = MarketDataSubscriptionResult::subscribed(handle);
    EXPECT_TRUE(subscribed);
    EXPECT_TRUE(subscribed.success());
    EXPECT_EQ(subscribed.status, MarketDataSubscriptionStatus::SUBSCRIBED);

    const auto failed = MarketDataSubscriptionResult::failed(
        handle,
        MarketDataSubscriptionStatus::FAILED,
        "network error");
    EXPECT_FALSE(failed);
    EXPECT_FALSE(failed.success());
    EXPECT_EQ(failed.status, MarketDataSubscriptionStatus::FAILED);
}

TEST(MarketDataSubscriptionBatchResult, AppliesAllOrFailsAsOneResult) {
    const TickSubscriptionRequest request("EUR/USD");
    const auto handle =
        MarketDataSubscriptionHandle::from_tick_request(3, 7, request);
    std::vector<MarketDataSubscriptionResult> results;
    results.push_back(MarketDataSubscriptionResult::subscribed(handle));

    const auto applied = MarketDataSubscriptionBatchResult::applied(std::move(results));
    EXPECT_TRUE(applied);
    EXPECT_TRUE(applied.success());
    EXPECT_EQ(applied.status, MarketDataSubscriptionStatus::APPLIED);
    EXPECT_EQ(applied.results.size(), 1u);

    const auto failed = MarketDataSubscriptionBatchResult::failed(
        MarketDataSubscriptionStatus::SUBSCRIBED,
        "not actually applied");
    EXPECT_FALSE(failed);
    EXPECT_EQ(failed.status, MarketDataSubscriptionStatus::FAILED);
}

TEST(MarketDataSubscriptionResult, CannotSubscribeWithInvalidHandle) {
    const auto result =
        MarketDataSubscriptionResult::subscribed(MarketDataSubscriptionHandle{});

    EXPECT_FALSE(result);
    EXPECT_EQ(result.status, MarketDataSubscriptionStatus::INVALID_REQUEST);
    EXPECT_NE(result.error_message.find("invalid"), std::string::npos);
}

TEST(MarketDataSubscriptionResult, FailureFactoryDoesNotAcceptSuccessStatus) {
    const TickSubscriptionRequest request("EUR/USD");
    const auto result = MarketDataSubscriptionResult::failed(
        request,
        MarketDataSubscriptionStatus::SUBSCRIBED,
        "not actually subscribed");

    EXPECT_FALSE(result);
    EXPECT_EQ(result.status, MarketDataSubscriptionStatus::FAILED);
}

TEST(BaseMarketDataProvider, DefaultProviderRejectsTickSubscriptionsWithTypedResult) {
    BaseMarketDataProvider provider;
    MarketDataSubscriptionResult result;
    int callback_count = 0;

    const bool accepted = provider.subscribe_ticks(
        TickSubscriptionRequest("EUR/USD"),
        [&result, &callback_count](MarketDataSubscriptionResult subscription_result) {
            result = std::move(subscription_result);
            ++callback_count;
        });

    EXPECT_FALSE(accepted);
    EXPECT_EQ(callback_count, 1);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.status, MarketDataSubscriptionStatus::UNSUPPORTED);
    EXPECT_EQ(result.subscription.stream_type, MarketDataType::TICKS);
    EXPECT_EQ(result.subscription.symbol, "EUR/USD");
    EXPECT_NE(result.error_message.find("Tick subscriptions"), std::string::npos);
}

TEST(BaseMarketDataProvider, DefaultProviderRejectsInvalidBarSubscriptions) {
    BaseMarketDataProvider provider;
    MarketDataSubscriptionResult result;
    int callback_count = 0;

    const bool accepted = provider.subscribe_bars(
        BarSubscriptionRequest("EUR/USD", 0),
        [&result, &callback_count](MarketDataSubscriptionResult subscription_result) {
            result = std::move(subscription_result);
            ++callback_count;
        });

    EXPECT_FALSE(accepted);
    EXPECT_EQ(callback_count, 1);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.status, MarketDataSubscriptionStatus::INVALID_REQUEST);
    EXPECT_EQ(result.subscription.stream_type, MarketDataType::BARS);
    EXPECT_EQ(to_str(result.status), std::string("INVALID_REQUEST"));
}

TEST(BaseMarketDataProvider, DefaultProviderRejectsUnsupportedUnsubscribe) {
    BaseMarketDataProvider provider;
    MarketDataSubscriptionResult result;
    int callback_count = 0;

    const TickSubscriptionRequest request("BTCUSDT");
    const auto handle =
        MarketDataSubscriptionHandle::from_tick_request(provider.provider_id(), 7, request);

    const bool accepted = provider.unsubscribe(
        handle,
        [&result, &callback_count](MarketDataSubscriptionResult subscription_result) {
            result = std::move(subscription_result);
            ++callback_count;
        });

    EXPECT_FALSE(accepted);
    EXPECT_EQ(callback_count, 1);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.status, MarketDataSubscriptionStatus::UNSUPPORTED);
    EXPECT_EQ(result.subscription.provider_id, handle.provider_id);
    EXPECT_EQ(result.subscription.id, handle.id);
}

TEST(BaseMarketDataProvider, RejectsHandleFromAnotherProvider) {
    BaseMarketDataProvider provider_a;
    BaseMarketDataProvider provider_b;
    MarketDataSubscriptionResult result;

    const TickSubscriptionRequest request("BTCUSDT");
    const auto handle_from_a =
        MarketDataSubscriptionHandle::from_tick_request(provider_a.provider_id(), 9, request);

    const bool accepted = provider_b.unsubscribe(
        handle_from_a,
        [&result](MarketDataSubscriptionResult subscription_result) {
            result = std::move(subscription_result);
        });

    EXPECT_FALSE(accepted);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.status, MarketDataSubscriptionStatus::WRONG_PROVIDER);
    EXPECT_EQ(to_str(result.status), std::string("WRONG_PROVIDER"));
}

TEST(BaseMarketDataProvider, DefaultProviderRejectsBatchSubscriptions) {
    BaseMarketDataProvider provider;
    MarketDataSubscriptionBatchResult result;
    int callback_count = 0;

    MarketDataSubscriptionBatch batch;
    batch.subscribe_ticks(TickSubscriptionRequest("EUR/USD"));
    batch.subscribe_bars(BarSubscriptionRequest("EUR/USD", 60));

    const bool accepted = provider.apply_subscriptions(
        std::move(batch),
        [&result, &callback_count](MarketDataSubscriptionBatchResult batch_result) {
            result = std::move(batch_result);
            ++callback_count;
        });

    EXPECT_FALSE(accepted);
    EXPECT_EQ(callback_count, 1);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.status, MarketDataSubscriptionStatus::FAILED);
    EXPECT_EQ(result.results.size(), 2u);
    EXPECT_EQ(result.results[0].status, MarketDataSubscriptionStatus::UNSUPPORTED);
    EXPECT_EQ(result.results[1].status, MarketDataSubscriptionStatus::UNSUPPORTED);
}

TEST(BaseMarketDataProvider, DefaultCallbackSlotsArePerInstance) {
    BaseMarketDataProvider provider_a;
    BaseMarketDataProvider provider_b;

    EXPECT_NE(&provider_a.on_tick_data(), &provider_b.on_tick_data());
    EXPECT_NE(&provider_a.on_bar_data(), &provider_b.on_bar_data());
    EXPECT_NE(
        &provider_a.on_market_data_status(),
        &provider_b.on_market_data_status());

    provider_a.on_tick_data() = [](std::unique_ptr<TickDataBatch>) {};
    provider_a.on_bar_data() = [](std::unique_ptr<BarDataBatch>) {};
    provider_a.on_market_data_status() = [](MarketDataStatusUpdate) {};

    EXPECT_TRUE(static_cast<bool>(provider_a.on_tick_data()));
    EXPECT_TRUE(static_cast<bool>(provider_a.on_bar_data()));
    EXPECT_TRUE(static_cast<bool>(provider_a.on_market_data_status()));
    EXPECT_FALSE(static_cast<bool>(provider_b.on_tick_data()));
    EXPECT_FALSE(static_cast<bool>(provider_b.on_bar_data()));
    EXPECT_FALSE(static_cast<bool>(provider_b.on_market_data_status()));
}

TEST(BaseMarketDataProvider, IsNotCopyableOrMovable) {
    static_assert(!std::is_copy_constructible<BaseMarketDataProvider>::value,
                  "provider identity must not be copy-constructible");
    static_assert(!std::is_copy_assignable<BaseMarketDataProvider>::value,
                  "provider identity must not be copy-assignable");
    static_assert(!std::is_move_constructible<BaseMarketDataProvider>::value,
                  "provider identity must not be move-constructible");
    static_assert(!std::is_move_assignable<BaseMarketDataProvider>::value,
                  "provider identity must not be move-assignable");
    SUCCEED();
}

TEST(BarTimeframe, SupportsDailyTimeframeWithoutTruncation) {
    const Bar bar(1.0, 2.0, 0.5, 1.5, 10.0, 1000);
    const SingleBar single_bar(
        bar,
        "EURUSD",
        "test",
        86400,
        5,
        0);

    EXPECT_EQ(single_bar.timeframe, 86400);

    BarSequence sequence;
    sequence.timeframe = 86400;
    EXPECT_EQ(sequence.timeframe, 86400);

    const BarHistoryRequest history_request("EURUSD", 86400, 1000, 2000);
    EXPECT_EQ(history_request.timeframe, 86400);
}

TEST(MarketDataPayloadFlags, TickAndBarEncodeOriginAndPriceType) {
    Tick tick;
    mark_live_payload(tick.flags);
    tick.set_flag(TickUpdateFlags::BID_UPDATED);
    EXPECT_TRUE(tick.has_flag(MarketDataFlags::LIVE_SOURCE));
    EXPECT_TRUE(tick.has_flag(MarketDataFlags::REALTIME));
    EXPECT_FALSE(tick.has_flag(MarketDataFlags::CATCHUP));
    EXPECT_FALSE(tick.has_flag(MarketDataFlags::HISTORICAL));
    EXPECT_TRUE(tick.has_flag(TickUpdateFlags::BID_UPDATED));
    EXPECT_TRUE(market_data_flags_valid(tick.flags));

    Tick trade_tick;
    trade_tick.last = 61521.34;
    EXPECT_DOUBLE_EQ(trade_tick.mid_price(), 61521.34);

    Bar bar;
    mark_historical_payload(bar.flags, true);
    bar.set_price_type(MarketPriceType::MID);
    EXPECT_TRUE(bar.has_flag(MarketDataFlags::HISTORICAL));
    EXPECT_TRUE(bar.has_flag(MarketDataFlags::BACKFILL));
    EXPECT_FALSE(bar.has_flag(MarketDataFlags::LIVE_SOURCE));
    EXPECT_FALSE(bar.has_flag(MarketDataFlags::REALTIME));
    EXPECT_FALSE(bar.has_flag(MarketDataFlags::CATCHUP));
    EXPECT_EQ(bar.price_type(), MarketPriceType::MID);
    EXPECT_TRUE(market_data_flags_valid(bar.flags));
}

TEST(MarketDataPayloadFlags, FormatsFlagsAndPriceTypes) {
    std::uint32_t flags = 0;
    mark_live_payload(flags);
    set_flag_in_place(flags, MarketDataFlags::INCOMPLETE);
    set_market_price_type_in_place(flags, MarketPriceType::MID);

    EXPECT_STREQ(to_str(MarketPriceType::MID), "MID");
    EXPECT_STREQ(optionx::market_data::to_str(MarketDataType::TICKS), "TICKS");
    EXPECT_STREQ(optionx::market_data::to_str(MarketDataStreamStatus::READY), "READY");
    EXPECT_EQ(market_price_type(flags), MarketPriceType::MID);
    EXPECT_EQ(
        market_data_flags_to_string(flags),
        "LIVE_SOURCE|REALTIME|INCOMPLETE");
    EXPECT_EQ(market_data_flags_to_string(0), "NONE");
}

TEST(MarketDataPayloadFlags, NormalizesCatchupAndHistoryDeliveryModes) {
    std::uint32_t flags = 0;
    set_flag_in_place(flags, MarketDataFlags::INCOMPLETE);

    mark_live_payload(flags, true);
    EXPECT_TRUE(has_flag(flags, MarketDataFlags::LIVE_SOURCE));
    EXPECT_TRUE(has_flag(flags, MarketDataFlags::CATCHUP));
    EXPECT_FALSE(has_flag(flags, MarketDataFlags::REALTIME));
    EXPECT_TRUE(has_flag(flags, MarketDataFlags::INCOMPLETE));
    EXPECT_TRUE(market_data_flags_valid(flags));

    mark_historical_payload(flags, true);
    EXPECT_FALSE(has_flag(flags, MarketDataFlags::LIVE_SOURCE));
    EXPECT_FALSE(has_flag(flags, MarketDataFlags::CATCHUP));
    EXPECT_FALSE(has_flag(flags, MarketDataFlags::REALTIME));
    EXPECT_TRUE(has_flag(flags, MarketDataFlags::HISTORICAL));
    EXPECT_TRUE(has_flag(flags, MarketDataFlags::BACKFILL));
    EXPECT_TRUE(has_flag(flags, MarketDataFlags::INCOMPLETE));
    EXPECT_TRUE(market_data_flags_valid(flags));
}

TEST(MarketDataPayloadFlags, RejectsContradictoryOriginAndDeliveryFlags) {
    EXPECT_EQ(
        static_cast<std::uint32_t>(MarketDataFlags::LIVE_SOURCE),
        1U << 16);

    std::uint32_t flags = 0;
    set_flag_in_place(flags, MarketDataFlags::REALTIME);
    EXPECT_FALSE(market_data_flags_valid(flags));

    mark_live_payload(flags);
    set_flag_in_place(flags, MarketDataFlags::CATCHUP);
    EXPECT_FALSE(market_data_flags_valid(flags));

    flags = 0;
    set_flag_in_place(flags, MarketDataFlags::BACKFILL);
    EXPECT_FALSE(market_data_flags_valid(flags));
}

TEST(MarketDataPayloadFlags, ParsesPriceSourcesAndTransports) {
    EXPECT_EQ(bar_price_source_from_string("bid"), BarPriceSource::BID);
    EXPECT_EQ(bar_price_source_from_string(" avg "), BarPriceSource::MID);
    EXPECT_EQ(bar_price_source_from_string("bad", BarPriceSource::LAST), BarPriceSource::LAST);
    EXPECT_STREQ(to_str(BarPriceSource::LAST), "LAST");
    EXPECT_EQ(
        market_price_type_from_bar_price_source(BarPriceSource::ASK),
        MarketPriceType::ASK);
    EXPECT_EQ(
        market_price_type_from_bar_price_source(BarPriceSource::UNKNOWN),
        MarketPriceType::UNKNOWN);

    EXPECT_EQ(
        optionx::market_data::market_data_transport_from_string("ws"),
        market_data::MarketDataTransport::WEBSOCKET);
    EXPECT_EQ(
        optionx::market_data::market_data_transport_from_string("poll"),
        market_data::MarketDataTransport::POLLING);
}

TEST(MarketDataPayloadFlags, TickCanClearMarketDataFlag) {
    Tick tick;
    mark_live_payload(tick.flags);
    ASSERT_TRUE(tick.has_flag(MarketDataFlags::REALTIME));

    tick.set_flag(MarketDataFlags::REALTIME, false);
    EXPECT_FALSE(tick.has_flag(MarketDataFlags::REALTIME));
}

TEST(MarketDataBatch, CarriesSharedStreamMetadata) {
    TickDataBatch batch;
    batch.type = MarketDataType::TICKS;
    batch.symbol = "EURUSD";
    batch.price_digits = 5;
    batch.volume_digits = 0;
    batch.items.push_back(Tick{1.2, 1.1, 0.0, 1000, 1001, 0});

    EXPECT_FALSE(batch.empty());
    EXPECT_EQ(batch.size(), 1u);
    EXPECT_EQ(batch.type, MarketDataType::TICKS);
    EXPECT_EQ(batch.symbol, "EURUSD");
    EXPECT_EQ(batch.price_digits, 5u);
}

TEST(MarketDataContinuityService, RoutesHistoricalBarsAsBackfillBatch) {
    FakeHistoryProvider provider;
    provider.next_result = BarHistoryResult::ok(BarSequence{}, 200);
    provider.next_sequence.symbol = "EURUSD";
    provider.next_sequence.provider = "fake";
    provider.next_sequence.timeframe = 60;
    provider.next_sequence.price_digits = 5;
    provider.next_sequence.volume_digits = 0;
    provider.next_sequence.price_source = BarPriceSource::BID;
    provider.next_sequence.bars.push_back(Bar{
        1.0,
        1.2,
        0.9,
        1.1,
        10.0,
        1000,
        static_cast<std::uint32_t>(MarketDataFlags::LIVE_SOURCE) |
            static_cast<std::uint32_t>(MarketDataFlags::REALTIME) |
            static_cast<std::uint32_t>(MarketDataFlags::CATCHUP)});

    const auto handle = MarketDataSubscriptionHandle::from_bar_request(
        provider.provider_id(),
        12,
        BarSubscriptionRequest("EURUSD", 60, BarPriceSource::BID));

    std::unique_ptr<BarDataBatch> delivered;
    MarketDataContinuityService service(provider);
    const bool accepted = service.request_bar_history_batch(
        BarHistoryRequest("EUR/USD", 60, 1000, 2000, BarPriceSource::MID),
        handle,
        [&delivered](std::unique_ptr<BarDataBatch> batch) {
            delivered = std::move(batch);
        });

    EXPECT_TRUE(accepted);
    ASSERT_NE(delivered, nullptr);
    ASSERT_EQ(delivered->items.size(), 1u);
    EXPECT_EQ(delivered->type, MarketDataType::BARS);
    EXPECT_EQ(delivered->symbol, "EURUSD");
    EXPECT_EQ(delivered->timeframe, 60);
    EXPECT_EQ(delivered->price_digits, 5u);
    EXPECT_EQ(delivered->subscription.id, handle.id);
    EXPECT_TRUE(delivered->items[0].has_flag(MarketDataFlags::HISTORICAL));
    EXPECT_TRUE(delivered->items[0].has_flag(MarketDataFlags::BACKFILL));
    EXPECT_FALSE(delivered->items[0].has_flag(MarketDataFlags::LIVE_SOURCE));
    EXPECT_FALSE(delivered->items[0].has_flag(MarketDataFlags::REALTIME));
    EXPECT_FALSE(delivered->items[0].has_flag(MarketDataFlags::CATCHUP));
    EXPECT_TRUE(market_data_flags_valid(delivered->items[0].flags));
    EXPECT_EQ(delivered->items[0].price_type(), MarketPriceType::BID);
}

TEST(MarketDataContinuityService, ForwardsTypedHistoryFailure) {
    FakeHistoryProvider provider;
    provider.next_result = BarHistoryResult::fail("network down", BarHistoryResult::NO_RESPONSE_STATUS);

    bool data_callback_called = false;
    BarHistoryResult failure;
    MarketDataContinuityService service(provider);
    const bool accepted = service.request_bar_history_batch(
        BarHistoryRequest("EUR/USD", 60, 1000, 2000),
        {},
        [&data_callback_called](std::unique_ptr<BarDataBatch>) {
            data_callback_called = true;
        },
        [&failure](BarHistoryResult result) {
            failure = std::move(result);
        });

    EXPECT_TRUE(accepted);
    EXPECT_FALSE(data_callback_called);
    EXPECT_FALSE(failure);
    EXPECT_EQ(failure.error_desc, "network down");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
