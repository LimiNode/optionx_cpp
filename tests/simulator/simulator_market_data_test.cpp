#include <optionx_cpp/simulator_market_data.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <thread>

using namespace optionx;
using namespace optionx::platforms;
namespace md = optionx::market_data;
namespace {
class Provider : public md::BaseMarketDataProvider {
public:
    md::MarketDataSubscriptionHandle handle;
    subscription_callback_t pending;
    bool defer = false;
    bool reject = false;
    int unsubscribes = 0;
    std::uint64_t sequence = 0;
    bool subscribe_ticks(md::TickSubscriptionRequest request, subscription_callback_t callback) override {
        if (reject) return false;
        handle = md::MarketDataSubscriptionHandle::from_tick_request(provider_id(), ++sequence, request);
        if (defer) pending = std::move(callback);
        else callback(md::MarketDataSubscriptionResult::subscribed(handle));
        return true;
    }
    bool unsubscribe(md::MarketDataSubscriptionHandle value, subscription_callback_t callback) override {
        ++unsubscribes;
        if (callback) callback(md::MarketDataSubscriptionResult::unsubscribed(value));
        return true;
    }
    void complete() {
        auto callback = std::move(pending);
        callback(md::MarketDataSubscriptionResult::subscribed(handle));
    }
    void status(md::MarketDataStreamStatus status) {
        md::MarketDataStatusUpdate update;
        update.provider_id = provider_id(); update.subscription = handle;
        update.type = md::MarketDataType::TICKS; update.symbol = handle.symbol; update.status = status;
        if (on_market_data_status()) on_market_data_status()(std::move(update));
    }
    void emit(std::uint64_t source_ms, double price, std::uint32_t digits = 2, std::uint32_t flags = 0) {
        auto batch = std::make_unique<md::TickDataBatch>();
        batch->subscription = handle; batch->symbol = handle.symbol; batch->type = md::MarketDataType::TICKS;
        batch->price_digits = digits;
        Tick tick; tick.time_ms = source_ms; tick.reference = price;
        tick.bid = price - 1; tick.ask = price + 1; tick.received_ms = 777; tick.flags = flags;
        batch->items.push_back(tick);
        if (on_tick_data()) on_tick_data()(std::move(batch));
    }
};
SimulatorPolicySnapshot policy() {
    SimulatorPolicy value;
    value.symbols["EURUSD"] = {}; value.symbols["GBPUSD"] = {};
    value.defaults.available = true;
    value.defaults.payout_ppm = 750000; value.defaults.min_stake_minor = 1;
    value.defaults.max_stake_minor = 10000; value.allowed_expiry = {60, 60};
    const auto snapshot = SimulatorPolicySnapshot::create(value);
    if (!snapshot) throw std::invalid_argument("test simulator policy");
    return *snapshot;
}
class MarketFeed : public testing::Test {
protected:
    std::int64_t now = 0;
    std::shared_ptr<Provider> provider = std::make_shared<Provider>();
    md::MarketDataRouter router;
    std::shared_ptr<SimulatorQuoteBook> book = std::make_shared<SimulatorQuoteBook>();
    std::unique_ptr<SimulatorMarketDataBinding> binding;
    void SetUp() override { ASSERT_TRUE(router.register_provider(md::MarketDataProviderId{1}, *provider)); }
    void bind(SimulatorPriceSide side = SimulatorPriceSide::REFERENCE, std::int64_t age = 0) {
        binding = std::make_unique<SimulatorMarketDataBinding>(router, provider, book,
            SimulatorMarketDataConfig{md::MarketDataProviderId{1}, "EURUSD.raw", "EURUSD", side, 2, age},
            [this] { return now; });
    }
    void ready() { provider->status(md::MarketDataStreamStatus::READY); }
    SimulatorCore core(std::int64_t age = 0, std::int64_t delay = 0) {
        return SimulatorCore({"paper", CurrencyType::USD, 2, 100000}, {delay, 0, age, 16}, policy(), book->selector());
    }
    void TearDown() override { binding.reset(); router.shutdown(); }
};
TEST_F(MarketFeed, SubscribedIsNotReadyAndReadinessAloneIsNotAPrice) {
    bind(); EXPECT_TRUE(binding->subscription().valid());
    provider->emit(0, 100);
    EXPECT_EQ(book->select("EURUSD", 0).availability, SimulatorQuoteAvailability::NOT_READY);
    ready(); EXPECT_EQ(book->select("EURUSD", 0).availability, SimulatorQuoteAvailability::MISSING);
    provider->emit(0, 100);
    const auto quote = book->select("EURUSD", 0);
    ASSERT_TRUE(quote.quote); EXPECT_EQ(quote.quote->price_units, 10000);
    EXPECT_EQ(quote.quote->provider_key, 1u); EXPECT_EQ(quote.quote->provider_instance, provider->provider_id());
    EXPECT_EQ(quote.quote->source_generation, binding->generation());
    EXPECT_EQ(quote.quote->provider_received_ms, 777u); EXPECT_EQ(quote.quote->received_ms, 0);
}
TEST_F(MarketFeed, RouterTicksDriveIntegerSettlementWithoutFixtureLookAhead) {
    bind(); ready(); provider->emit(0, 100);
    auto simulator = core(); const auto id = *simulator.enqueue({"EURUSD", OrderType::BUY, 1000, 60});
    ASSERT_TRUE(simulator.process()); EXPECT_EQ(simulator.account().exposure_minor, 1000);
    now = 60000; provider->emit(60000, 101); ASSERT_TRUE(simulator.advance_to(now));
    const auto result = simulator.query(id); ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, TradeState::WIN); EXPECT_EQ(result->net_profit_minor, 750);
    EXPECT_EQ(simulator.account().balance_minor, 100750); EXPECT_EQ(simulator.account().exposure_minor, 0);
    EXPECT_EQ(result->open_quote->source_generation, binding->generation());
    EXPECT_EQ(result->close_quote->received_ms, 60000);
}
TEST_F(MarketFeed, LateConflictingTicksCannotSettleOrNotifyTwice) {
    bind(); ready(); provider->emit(0, 100);
    auto simulator = core(); int callbacks = 0;
    const auto id = *simulator.enqueue({"EURUSD", OrderType::BUY, 1000, 60},
        [&](const SimulatorTrade&) { ++callbacks; });
    ASSERT_TRUE(simulator.process()); now = 60000; provider->emit(60000, 101);
    ASSERT_TRUE(simulator.advance_to(now));
    now = 60001; provider->emit(60000, 99); provider->emit(60000, 99);
    ASSERT_TRUE(simulator.advance_to(now)); ASSERT_TRUE(simulator.process());
    const auto result = simulator.query(id); ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, TradeState::WIN); EXPECT_EQ(result->net_profit_minor, 750);
    EXPECT_EQ(result->close_quote->price_units, 10100); EXPECT_EQ(result->settlement_ms, 60000);
    EXPECT_EQ(simulator.account().balance_minor, 100750); EXPECT_EQ(callbacks, 1);
}
TEST_F(MarketFeed, LatePreExpiryTickCannotRewriteUnresolvedResult) {
    bind(); ready(); provider->emit(0, 100);
    auto simulator = core(); const auto id = *simulator.enqueue({"EURUSD", OrderType::BUY, 1000, 60});
    simulator.process(); now = 60000; simulator.advance_to(now);
    EXPECT_EQ(simulator.query(id)->phase, SimulatorTradePhase::UNRESOLVED);
    now = 60001; provider->emit(59999, 101); simulator.advance_to(now);
    EXPECT_EQ(book->select("EURUSD", 60000).availability, SimulatorQuoteAvailability::STALE);
    EXPECT_EQ(simulator.query(id)->phase, SimulatorTradePhase::UNRESOLVED);
    EXPECT_FALSE(simulator.query(id)->net_profit_minor); EXPECT_EQ(simulator.account().exposure_minor, 1000);
}
TEST_F(MarketFeed, DisconnectRevokesCachedQuotesAndReconnectNeedsANewObservation) {
    bind(SimulatorPriceSide::REFERENCE, 1000); ready(); provider->emit(0, 100);
    now = 1; provider->status(md::MarketDataStreamStatus::DISCONNECTED);
    EXPECT_EQ(book->select("EURUSD", 1).availability, SimulatorQuoteAvailability::NOT_READY);
    now = 2; ready(); EXPECT_EQ(book->select("EURUSD", 2).availability, SimulatorQuoteAvailability::MISSING);
    provider->emit(2, 101); EXPECT_TRUE(book->select("EURUSD", 2).quote);
    EXPECT_EQ(book->select("EURUSD", 0).quote->price_units, 10000);
}
TEST_F(MarketFeed, MissingOrFutureOpeningQuoteFailsWithoutReservingForever) {
    bind(); ready(); provider->emit(10, 100);
    auto simulator = core(); const auto id = *simulator.enqueue({"EURUSD", OrderType::BUY, 1000, 60});
    simulator.process(); EXPECT_EQ(simulator.query(id)->phase, SimulatorTradePhase::FAILED_OPEN);
    EXPECT_EQ(simulator.account().reserved_minor, 0); EXPECT_EQ(simulator.account().exposure_minor, 0);
}
TEST_F(MarketFeed, DuplicateAndOutOfOrderPricesNeverRegressLatestSourceTime) {
    bind(SimulatorPriceSide::REFERENCE, 100); ready(); now = 20;
    provider->emit(20, 102); provider->emit(10, 101); provider->emit(20, 102);
    EXPECT_EQ(book->select("EURUSD", 20).quote->price_units, 10200);
    EXPECT_FALSE(book->select("EURUSD", 19).quote); // All were actually received at 20.
    provider->emit(20, 103); EXPECT_EQ(book->select("EURUSD", 20).quote->price_units, 10300);
}
TEST_F(MarketFeed, PricesHaveAnExplicitSideAndFixedScale) {
    for (const auto side : {SimulatorPriceSide::BID, SimulatorPriceSide::ASK,
                           SimulatorPriceSide::MID, SimulatorPriceSide::REFERENCE}) {
        bind(side); ready(); provider->emit(0, 100);
        EXPECT_EQ(book->select("EURUSD", 0).quote->price_units,
            side == SimulatorPriceSide::BID ? 9900 : side == SimulatorPriceSide::ASK ? 10100 : 10000);
        binding.reset();
    }
}
TEST_F(MarketFeed, InvalidPricesAreIgnoredAndScaleMismatchFailsClosed) {
    bind(); ready();
    provider->emit(0, (std::numeric_limits<double>::quiet_NaN)());
    provider->emit(0, (std::numeric_limits<double>::max)());
    provider->emit(0, -1);
    EXPECT_FALSE(book->select("EURUSD", 0).quote);
    provider->emit(0, 100, 3);
    EXPECT_EQ(book->select("EURUSD", 0).availability, SimulatorQuoteAvailability::BLOCKED);
}
TEST_F(MarketFeed, RouterNormalizesProviderLiveFlagsBeforeDelivery) {
    bind(); ready();
    provider->emit(0, 100, 2, static_cast<std::uint32_t>(MarketDataFlags::HISTORICAL));
    // A provider live callback is live evidence under the existing Router contract.
    const auto quote = book->select("EURUSD", 0).quote;
    ASSERT_TRUE(quote); EXPECT_EQ(quote->price_units, 10000);
}
TEST_F(MarketFeed, RebindingCannotSettleAnOpenedTradeAgainstAnotherGeneration) {
    bind(); ready(); provider->emit(0, 100);
    auto simulator = core(); const auto id = *simulator.enqueue({"EURUSD", OrderType::BUY, 1000, 60});
    ASSERT_TRUE(simulator.process()); binding.reset(); bind(); ready();
    now = 60000; provider->emit(60000, 101); ASSERT_TRUE(simulator.advance_to(now));
    const auto result = simulator.query(id); ASSERT_TRUE(result);
    EXPECT_EQ(result->phase, SimulatorTradePhase::UNRESOLVED);
    EXPECT_EQ(result->unresolved_reason, SimulatorUnresolvedReason::INCOMPATIBLE_SOURCE);
    EXPECT_FALSE(result->net_profit_minor); EXPECT_EQ(simulator.account().exposure_minor, 1000);
}
TEST_F(MarketFeed, MultipleProvidersAreIsolatedAndDoNotImplicitlyFallback) {
    bind(); ready(); provider->emit(0, 100);
    auto other = std::make_shared<Provider>(); ASSERT_TRUE(router.register_provider(md::MarketDataProviderId{2}, *other));
    {
        SimulatorMarketDataBinding second(router, other, book,
            {md::MarketDataProviderId{2}, "GBP.raw", "GBPUSD", SimulatorPriceSide::REFERENCE, 2, 0}, [this] { return now; });
        other->status(md::MarketDataStreamStatus::READY); other->emit(0, 200);
        EXPECT_EQ(book->select("GBPUSD", 0).quote->provider_key, 2u);
        provider->status(md::MarketDataStreamStatus::DISCONNECTED);
        EXPECT_FALSE(book->select("EURUSD", 0).quote);
        EXPECT_TRUE(book->select("GBPUSD", 0).quote);
    }
    router.unregister_provider(md::MarketDataProviderId{2});
}
TEST_F(MarketFeed, DelayedAcceptanceAndShutdownUseExistingRouterOwnership) {
    provider->defer = true; bind(); EXPECT_FALSE(binding->subscription().valid());
    provider->complete(); EXPECT_TRUE(binding->subscription().valid());
    ready(); provider->emit(0, 100); EXPECT_TRUE(book->select("EURUSD", 0).quote);
    const auto callback = provider->on_tick_data();
    binding.reset(); EXPECT_EQ(provider->unsubscribes, 1);
    EXPECT_FALSE(book->select("EURUSD", 0).quote);
    auto late = std::make_unique<md::TickDataBatch>(); late->subscription = provider->handle;
    callback(std::move(late)); EXPECT_FALSE(book->select("EURUSD", 0).quote);
}
TEST_F(MarketFeed, PendingAcceptanceAfterBindingDestructionOnlyCleansUpRouterRoute) {
    provider->defer = true; bind(); const auto old = binding->generation();
    binding.reset(); provider->complete();
    EXPECT_EQ(provider->unsubscribes, 1);
    EXPECT_FALSE(book->status(old, true, 0));
    EXPECT_FALSE(book->select("EURUSD", 0).quote);
    EXPECT_EQ(router.subscription_count(), 0u);
}
TEST_F(MarketFeed, RebindingAllocatesNewGenerationAndOldGenerationCannotFeedIt) {
    bind(); const auto old = binding->generation(); ready(); provider->emit(0, 100);
    binding.reset(); bind(); EXPECT_GT(binding->generation(), old);
    EXPECT_FALSE(book->status(old, true, 0));
    SimulatorTick stale{"EURUSD", 0, 123}; stale.received_ms = 0;
    EXPECT_FALSE(book->observe(old, stale));
    EXPECT_FALSE(book->select("EURUSD", 0).quote);
    ready(); provider->emit(0, 200); EXPECT_EQ(book->select("EURUSD", 0).quote->price_units, 20000);
}
TEST_F(MarketFeed, BindingDestructionPreservesOpenedUnresolvedRisk) {
    bind(); ready(); provider->emit(0, 100);
    auto simulator = core(); const auto id = *simulator.enqueue({"EURUSD", OrderType::BUY, 1000, 60});
    simulator.process(); binding.reset(); simulator.advance_to(60000);
    EXPECT_EQ(simulator.query(id)->phase, SimulatorTradePhase::UNRESOLVED);
    EXPECT_EQ(simulator.account().exposure_minor, 1000);
}
TEST_F(MarketFeed, InvalidProviderKeyAndDuplicateSymbolAreRejectedBeforeNewRoute) {
    EXPECT_THROW(SimulatorMarketDataBinding(router, provider, book,
        {md::MarketDataProviderId{2}, "EURUSD.raw", "EURUSD", SimulatorPriceSide::REFERENCE, 2, 0}, [this] { return now; }), std::invalid_argument);
    bind(); EXPECT_THROW(bind(), std::invalid_argument);
    EXPECT_EQ(router.subscription_count(), 1u);
}
TEST(SimulatorQuoteBookTest, BoundsClockRegressionAndWrongThreadFailClosed) {
    for (const int scenario : {0, 1, 2}) {
        auto book = std::make_shared<SimulatorQuoteBook>(2, 1);
        const auto generation = book->bind("EURUSD", 10); book->accept(generation, true);
        EXPECT_EQ(book->bind("GBPUSD", 10), 0u);
        ASSERT_TRUE(book->status(generation, true, 10));
        if (scenario == 0) { book->status(generation, false, 11); book->status(generation, true, 12); }
        if (scenario == 1) book->status(generation, false, 9);
        if (scenario == 2) { std::thread worker([&] { book->select("EURUSD", 10); }); worker.join(); }
        EXPECT_EQ(book->select("EURUSD", 12).availability, SimulatorQuoteAvailability::BLOCKED);
    }
}
TEST(SimulatorQuoteBookTest, SelectorDoesNotKeepDestroyedOwnerAlive) {
    auto book = std::make_shared<SimulatorQuoteBook>(); auto select = book->selector();
    const std::weak_ptr<SimulatorQuoteBook> weak = book; book.reset();
    EXPECT_TRUE(weak.expired()); EXPECT_FALSE(select("EURUSD", 0));
}
TEST(SimulatorQuoteBookTest, ClosingRequiresCompatiblePriceSemanticsEvenForCustomSelectors) {
    for (int scenario = 0; scenario < 5; ++scenario) {
        auto selector = [scenario](const std::string& symbol, std::int64_t when) {
            SimulatorTick quote{symbol, when, when == 0 ? 100 : 101};
            if (when != 0) {
                if (scenario == 0) quote.provider_key = 1;
                if (scenario == 1) quote.provider_instance = 1;
                if (scenario == 2) quote.source_generation = 1;
                if (scenario == 3) quote.price_decimals = 1;
                if (scenario == 4) quote.price_side = SimulatorPriceSide::BID;
            }
            return std::optional<SimulatorTick>{quote};
        };
        SimulatorCore simulator({"paper", CurrencyType::USD, 2, 100000}, {}, policy(), selector);
        const auto id = *simulator.enqueue({"EURUSD", OrderType::BUY, 1000, 60});
        ASSERT_TRUE(simulator.advance_to(60000));
        EXPECT_EQ(simulator.query(id)->phase, SimulatorTradePhase::UNRESOLVED);
        EXPECT_EQ(simulator.query(id)->unresolved_reason, SimulatorUnresolvedReason::INCOMPATIBLE_SOURCE);
        EXPECT_EQ(simulator.account().exposure_minor, 1000);
    }
}
} // namespace
int main(int argc, char** argv) { testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
