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

class TradingBot final : public market_data::MarketDataSubscriberBase {
public:
    TradingBot(
            market_data::MarketDataRouter& router,
            market_data::MarketDataProviderId provider_id,
            std::string provider_alias)
            : MarketDataSubscriberBase(router),
              m_provider_id(provider_id),
              m_provider_alias(std::move(provider_alias)) {}

    bool subscribe_eur() {
        m_eur_route = subscribe_ticks(
            m_provider_id,
            market_data::TickSubscriptionRequest(
                "EURUSD",
                market_data::MarketDataTransport::WEBSOCKET));
        return m_eur_route.valid();
    }

    bool subscribe_btc() {
        m_btc_route = subscribe_ticks(
            m_provider_alias,
            market_data::TickSubscriptionRequest(
                "BTCUSDT",
                market_data::MarketDataTransport::WEBSOCKET));
        return m_btc_route.valid();
    }

    market_data::MarketDataSubscriptionHandle eur_subscription() const {
        return provider_subscription(m_eur_route);
    }

    market_data::MarketDataSubscriptionHandle btc_subscription() const {
        return provider_subscription(m_btc_route);
    }

    bool stop_eur() {
        return unsubscribe(m_eur_route);
    }

    void on_tick_data(const market_data::TickDataBatch& batch) override {
        const auto provider_id = market_data_router().registered_provider_id(
            batch.subscription.provider_id);
        std::cout << "tick provider=" << provider_id.value()
                  << " subscription=" << batch.subscription.id
                  << " symbol=" << batch.symbol
                  << " mid=" << batch.items.back().mid_price(batch.price_digits)
                  << '\n';
    }

    void on_market_data_status(
            const market_data::MarketDataStatusUpdate& update) override {
        statuses.push_back(update);
        const auto provider_id = market_data_router().registered_provider_id(
            update.provider_id);
        std::cout << "status provider=" << provider_id.value()
                  << " subscription=" << update.subscription.id
                  << " symbol=" << update.symbol
                  << " state=" << market_data::to_str(update.status)
                  << '\n';
    }

    std::vector<market_data::MarketDataStatusUpdate> statuses;

private:
    market_data::MarketDataProviderId m_provider_id;
    std::string m_provider_alias;
    market_data::RoutedSubscriptionId m_eur_route;
    market_data::RoutedSubscriptionId m_btc_route;
};

} // namespace

int main() {
    DemoMarketDataProvider provider;
    market_data::MarketDataRouter router;
    const market_data::MarketDataProviderId provider_id{1001};
    if (!router.register_provider(provider_id, provider, {"demo", "primary"})) {
        std::cerr << "Provider registration failed\n";
        return 1;
    }

    // The bot knows stable configuration keys, but not the concrete provider
    // object selected for those keys by application composition.
    auto bot = std::make_shared<TradingBot>(router, provider_id, "primary");

    if (!bot->subscribe_eur()) {
        std::cerr << "EURUSD subscription failed\n";
        return 1;
    }

    // BTC readiness is observed before the bot asks for BTC. The later route
    // receives this status with its own concrete provider subscription ID.
    provider.emit_ready("BTCUSDT");
    if (!bot->subscribe_btc()) {
        std::cerr << "BTCUSDT subscription failed\n";
        return 1;
    }

    const auto btc = bot->btc_subscription();
    const auto replayed_btc = std::find_if(
        bot->statuses.begin(),
        bot->statuses.end(),
        [&btc](const market_data::MarketDataStatusUpdate& update) {
            return update.subscription.id == btc.id &&
                   update.status == market_data::MarketDataStreamStatus::READY;
        });
    if (replayed_btc == bot->statuses.end()) {
        std::cerr << "BTCUSDT READY replay is missing\n";
        return 1;
    }

    provider.emit_ready("EURUSD");
    provider.emit_tick(bot->eur_subscription(), 1.08420, 1.08422);
    provider.emit_tick(btc, 61521.30, 61521.38);
    bot->stop_eur();
    return 0;
}
