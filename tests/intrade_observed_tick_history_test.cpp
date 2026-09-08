#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <optionx_cpp/utils/fixed_point.hpp>
#include <optionx_cpp/data/ticks.hpp>
#include <optionx_cpp/utils/pubsub.hpp>
#include <optionx_cpp/data/events/PriceUpdateEvent.hpp>
#include <optionx_cpp/platforms/IntradeBarPlatform/ObservedTickHistory.hpp>

using namespace optionx;
using namespace optionx::events;
using namespace optionx::platforms::intrade_bar;

namespace {

Tick make_tick(
        double bid,
        double ask,
        std::uint64_t time_ms,
        std::uint64_t received_ms = 0) {
    return Tick(
        ask,
        bid,
        0.0,
        0.0,
        time_ms,
        received_ms,
        0);
}

TickUpdateBatch make_batch(
        std::string symbol,
        std::initializer_list<Tick> ticks) {
    TickUpdateBatch batch;
    batch.symbol = std::move(symbol);
    batch.provider = "INTRADE_BAR";
    batch.price_digits = 5;
    batch.volume_digits = 0;
    batch.items.assign(ticks.begin(), ticks.end());
    return batch;
}

} // namespace

TEST(IntradeObservedTickHistory, ReturnsSortedInclusiveObservations) {
    IntradeObservedTickHistory archive;
    archive.record({make_batch(
        "EURUSD",
        {make_tick(1.1002, 1.1004, 3000),
         make_tick(1.1003, 1.1005, 2000),
         make_tick(1.1000, 1.1002, 1000),
         make_tick(1.1001, 1.1003, 2000)})});

    const auto result = archive.fetch(TickHistoryRequest("EURUSD", 1000, 3000));

    ASSERT_TRUE(result);
    EXPECT_TRUE(result.range_complete);
    ASSERT_EQ(result.sequence.ticks.size(), 4U);
    EXPECT_EQ(result.sequence.ticks[0].time_ms, 1000U);
    EXPECT_EQ(result.sequence.ticks[1].time_ms, 2000U);
    EXPECT_EQ(result.sequence.ticks[2].time_ms, 2000U);
    EXPECT_EQ(result.sequence.ticks[3].time_ms, 3000U);
    EXPECT_EQ(result.sequence.symbol, "EURUSD");
    EXPECT_EQ(result.sequence.provider, "INTRADE_BAR");
}

TEST(IntradeObservedTickHistory, DeduplicatesOnlyIdenticalMarketObservations) {
    IntradeObservedTickHistory archive;
    archive.record({make_batch(
        "EURUSD",
        {make_tick(1.1000, 1.1002, 1000, 10),
         make_tick(1.1000, 1.1002, 1000, 20),
         make_tick(1.1001, 1.1003, 1000, 30)})});

    const auto result = archive.fetch(TickHistoryRequest("EURUSD", 1000, 1000));

    ASSERT_TRUE(result);
    ASSERT_EQ(result.sequence.ticks.size(), 2U);
    EXPECT_DOUBLE_EQ(result.sequence.ticks[0].bid, 1.1000);
    EXPECT_DOUBLE_EQ(result.sequence.ticks[1].bid, 1.1001);
}

TEST(IntradeObservedTickHistory, CompletenessFailsClosedForMissingOrUnalignedSamples) {
    IntradeObservedTickHistory archive;
    archive.record({make_batch(
        "EURUSD",
        {make_tick(1.1000, 1.1002, 1000),
         make_tick(1.1002, 1.1004, 3000)})});

    const auto missing = archive.fetch(TickHistoryRequest("EURUSD", 1000, 3000));
    const auto unaligned = archive.fetch(TickHistoryRequest("EURUSD", 1001, 3000));

    ASSERT_TRUE(missing);
    EXPECT_FALSE(missing.range_complete);
    ASSERT_TRUE(unaligned);
    EXPECT_FALSE(unaligned.range_complete);
}

TEST(IntradeObservedTickHistory, EnforcesItemAndLookbackBounds) {
    IntradeObservedTickHistoryOptions options;
    options.max_items_per_symbol = 2;
    options.max_lookback_ms = 3000;
    IntradeObservedTickHistory archive(options);
    archive.record({make_batch(
        "EURUSD",
        {make_tick(1.1000, 1.1002, 1000),
         make_tick(1.1001, 1.1003, 2000),
         make_tick(1.1002, 1.1004, 3000)})});

    EXPECT_EQ(archive.size("EURUSD"), 2U);
    const auto retained = archive.fetch(TickHistoryRequest("EURUSD", 2000, 3000));
    const auto too_wide = archive.fetch(TickHistoryRequest("EURUSD", 1000, 5001));

    ASSERT_TRUE(retained);
    ASSERT_EQ(retained.sequence.ticks.size(), 2U);
    EXPECT_TRUE(retained.range_complete);
    EXPECT_FALSE(too_wide);
}

TEST(IntradeObservedTickHistory, UnknownSymbolsRemainSuccessfulButIncomplete) {
    IntradeObservedTickHistory archive;

    const auto result = archive.fetch(TickHistoryRequest("EURUSD", 1000, 1000));

    ASSERT_TRUE(result);
    EXPECT_FALSE(result.range_complete);
    EXPECT_TRUE(result.sequence.ticks.empty());
    EXPECT_EQ(result.sequence.symbol, "EURUSD");
}

TEST(IntradeObservedTickHistory, ClearDropsSessionObservations) {
    IntradeObservedTickHistory archive;
    archive.record({make_batch("EURUSD", {make_tick(1.1000, 1.1002, 1000)})});
    ASSERT_EQ(archive.size("EURUSD"), 1U);

    archive.clear();

    EXPECT_EQ(archive.size("EURUSD"), 0U);
}

TEST(IntradeObservedTickHistory, EstimatesBrokerAlignedTimeFromMedianOffsets) {
    IntradeObservedTickHistoryOptions options;
    options.clock_offset_sample_count = 5;
    IntradeObservedTickHistory archive(options);
    archive.record({make_batch(
        "EURUSD",
        {make_tick(1.1000, 1.1002, 1000, 1123),
         make_tick(1.1001, 1.1003, 2000, 2123),
         make_tick(1.1002, 1.1004, 3000, 3123),
         make_tick(1.1003, 1.1005, 4000, 1000),
         make_tick(1.1004, 1.1006, 5000, 5123)})});

    EXPECT_EQ(archive.provider_time_ms(4123), 4000U);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
