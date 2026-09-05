#include <optionx_cpp/market_data.hpp>

#include <algorithm>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace market_data = optionx::market_data;

class ManualOwnerLoop {
public:
    bool post(market_data::MarketDataRouter::owner_task_t task) {
        if (!task) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push_back(std::move(task));
        return true;
    }

    void process_all() {
        while (true) {
            market_data::MarketDataRouter::owner_task_t task;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_tasks.empty()) return;
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            task();
        }
    }

private:
    std::mutex m_mutex;
    std::deque<market_data::MarketDataRouter::owner_task_t> m_tasks;
};

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
        optionx::mark_live_payload(batch->items.back().flags);
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

    bool request_eur_subscription() {
        return post_subscribe_ticks(
            m_provider_id,
            market_data::TickSubscriptionRequest(
                "EURUSD",
                market_data::MarketDataTransport::WEBSOCKET),
            [this](market_data::RoutedSubscriptionId route) {
                m_eur_route = route;
            });
    }

    bool request_btc_subscription() {
        return post_subscribe_ticks(
            m_provider_alias,
            market_data::TickSubscriptionRequest(
                "BTCUSDT",
                market_data::MarketDataTransport::WEBSOCKET),
            [this](market_data::RoutedSubscriptionId route) {
                m_btc_route = route;
            });
    }

    market_data::MarketDataSubscriptionHandle eur_subscription() const {
        return provider_subscription(m_eur_route);
    }

    market_data::MarketDataSubscriptionHandle btc_subscription() const {
        return provider_subscription(m_btc_route);
    }

    bool request_stop_eur() {
        return post_unsubscribe(m_eur_route);
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
    ManualOwnerLoop owner_loop;
    market_data::MarketDataRouter router(
        [&owner_loop](market_data::MarketDataRouter::owner_task_t task) {
            return owner_loop.post(std::move(task));
        });
    const market_data::MarketDataProviderId provider_id{1001};
    if (!router.register_provider(provider_id, provider, {"demo", "primary"})) {
        std::cerr << "Provider registration failed\n";
        return 1;
    }

    // The bot knows stable configuration keys, but not the concrete provider
    // object selected for those keys by application composition.
    auto bot = std::make_shared<TradingBot>(router, provider_id, "primary");

    // A bot worker only posts commands. Provider methods and bot callbacks run
    // later when the application pumps its designated owner loop.
    bool eur_command_accepted = false;
    std::thread bot_thread([&]() {
        eur_command_accepted = bot->request_eur_subscription();
    });
    bot_thread.join();
    if (!eur_command_accepted) {
        std::cerr << "EURUSD subscription command was rejected\n";
        return 1;
    }
    owner_loop.process_all();
    if (!bot->eur_subscription().valid()) {
        std::cerr << "EURUSD subscription failed\n";
        return 1;
    }

    // BTC readiness is observed before the bot asks for BTC. The later route
    // receives this status with its own concrete provider subscription ID.
    std::thread source_thread([&]() {
        provider.emit_ready("BTCUSDT");
    });
    source_thread.join();
    owner_loop.process_all();

    bool btc_command_accepted = false;
    bot_thread = std::thread([&]() {
        btc_command_accepted = bot->request_btc_subscription();
    });
    bot_thread.join();
    if (!btc_command_accepted) {
        std::cerr << "BTCUSDT subscription command was rejected\n";
        return 1;
    }
    owner_loop.process_all();

    const auto btc = bot->btc_subscription();
    if (!btc.valid()) {
        std::cerr << "BTCUSDT subscription failed\n";
        return 1;
    }
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

    source_thread = std::thread([&]() {
        provider.emit_ready("EURUSD");
        provider.emit_tick(bot->eur_subscription(), 1.08420, 1.08422);
        provider.emit_tick(btc, 61521.30, 61521.38);
    });
    source_thread.join();
    owner_loop.process_all();

    bool stop_command_accepted = false;
    bot_thread = std::thread([&]() {
        stop_command_accepted = bot->request_stop_eur();
    });
    bot_thread.join();
    if (!stop_command_accepted) {
        std::cerr << "EURUSD unsubscribe command was rejected\n";
        return 1;
    }
    owner_loop.process_all();

    // Subscriber destruction posts its remaining BTC handle to the same loop.
    bot.reset();
    owner_loop.process_all();
    return 0;
}
