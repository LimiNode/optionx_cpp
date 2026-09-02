#include <optionx_cpp/market_data.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace market_data = optionx::market_data;

class DemoMarketDataProvider final : public market_data::BaseMarketDataProvider {
public:
    bars_callback_t& on_bar_data() override {
        return m_bars_callback;
    }

    ticks_callback_t& on_tick_data() override {
        return m_ticks_callback;
    }

    status_callback_t& on_market_data_status() override {
        return m_status_callback;
    }

    bool subscribe_ticks(
            market_data::TickSubscriptionRequest request,
            subscription_callback_t callback) override {
        auto subscription = market_data::MarketDataSubscriptionHandle::from_tick_request(
            provider_id(),
            m_next_subscription_id++,
            request);
        if (callback) {
            callback(market_data::MarketDataSubscriptionResult::subscribed(
                std::move(subscription)));
        }
        return true;
    }

    bool unsubscribe(
            market_data::MarketDataSubscriptionHandle subscription,
            subscription_callback_t callback) override {
        if (callback) {
            callback(market_data::MarketDataSubscriptionResult::unsubscribed(
                std::move(subscription)));
        }
        return true;
    }

    void emit_ready(std::string symbol) {
        market_data::MarketDataStatusUpdate update;
        update.provider_id = provider_id();
        update.type = market_data::MarketDataType::TICKS;
        update.symbol = std::move(symbol);
        update.transport = market_data::MarketDataTransport::WEBSOCKET;
        update.status = market_data::MarketDataStreamStatus::READY;
        if (m_status_callback) m_status_callback(std::move(update));
    }

    void emit_tick(
            const market_data::MarketDataSubscriptionHandle& subscription,
            double bid,
            double ask) {
        auto batch = std::make_unique<market_data::TickDataBatch>();
        batch->subscription = subscription;
        batch->type = market_data::MarketDataType::TICKS;
        batch->symbol = subscription.symbol;
        batch->price_digits = subscription.symbol == "BTCUSDT" ? 2 : 5;
        batch->items.emplace_back(bid, ask, 0.0, 1.0, 1783028778697ULL, 0, 0);
        batch->items.back().set_flag(optionx::MarketDataFlags::REALTIME);
        if (m_ticks_callback) m_ticks_callback(std::move(batch));
    }

private:
    market_data::SubscriptionId m_next_subscription_id = 1;
    bars_callback_t m_bars_callback;
    ticks_callback_t m_ticks_callback;
    status_callback_t m_status_callback;
};

class TradingBot final : public market_data::IMarketDataSubscriber {
public:
    std::vector<market_data::MarketDataStatusUpdate> statuses;

    void on_tick_data(const market_data::TickDataBatch& batch) override {
        std::cout << "tick subscription=" << batch.subscription.id
                  << " symbol=" << batch.symbol
                  << " mid=" << batch.items.back().mid_price(batch.price_digits)
                  << '\n';
    }

    void on_market_data_status(
            const market_data::MarketDataStatusUpdate& update) override {
        statuses.push_back(update);
        std::cout << "status subscription=" << update.subscription.id
                  << " symbol=" << update.symbol
                  << " state=" << market_data::to_str(update.status)
                  << '\n';
    }
};

} // namespace

int main() {
    DemoMarketDataProvider provider;
    market_data::MarketDataRouter router;
    auto bot = std::make_shared<TradingBot>();

    auto eur = router.subscribe_ticks(
        provider,
        bot,
        market_data::TickSubscriptionRequest(
            "EURUSD",
            market_data::MarketDataTransport::WEBSOCKET));
    if (!eur.active()) {
        std::cerr << "EURUSD subscription failed\n";
        return 1;
    }

    // The source can become ready before this bot creates its BTC route. The
    // router caches stream state while another route keeps the provider bound.
    provider.emit_ready("BTCUSDT");

    auto btc = router.subscribe_ticks(
        provider,
        bot,
        market_data::TickSubscriptionRequest(
            "BTCUSDT",
            market_data::MarketDataTransport::WEBSOCKET));
    if (!btc.active()) {
        std::cerr << "BTCUSDT subscription failed\n";
        return 1;
    }

    const auto btc_subscription = btc.provider_subscription();
    const auto replayed_btc = std::find_if(
        bot->statuses.begin(),
        bot->statuses.end(),
        [&btc_subscription](const market_data::MarketDataStatusUpdate& update) {
            return update.subscription.id == btc_subscription.id &&
                   update.status == market_data::MarketDataStreamStatus::READY;
        });
    if (replayed_btc == bot->statuses.end()) {
        std::cerr << "BTCUSDT READY was not replayed for its concrete route\n";
        return 1;
    }

    provider.emit_ready("EURUSD");
    provider.emit_tick(eur.provider_subscription(), 1.08420, 1.08422);
    provider.emit_tick(btc_subscription, 61521.30, 61521.38);

    eur.unsubscribe([](market_data::MarketDataSubscriptionResult result) {
        std::cout << "unsubscribe subscription=" << result.subscription.id
                  << " status=" << market_data::to_str(result.status)
                  << '\n';
    });
    return 0;
}
