#include <optionx_cpp/market_data.hpp>

#include <iostream>
#include <memory>
#include <utility>

namespace {

class DemoMarketDataProvider final : public optionx::market_data::BaseMarketDataProvider {
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

    void emit_status(optionx::market_data::MarketDataStatusUpdate update) {
        if (m_status_callback) {
            m_status_callback(std::move(update));
        }
    }

    void emit_ticks(std::unique_ptr<optionx::market_data::TickDataBatch> batch) {
        if (m_ticks_callback) {
            m_ticks_callback(std::move(batch));
        }
    }

    void emit_bars(std::unique_ptr<optionx::market_data::BarDataBatch> batch) {
        if (m_bars_callback) {
            m_bars_callback(std::move(batch));
        }
    }

private:
    bars_callback_t m_bars_callback;
    ticks_callback_t m_ticks_callback;
    status_callback_t m_status_callback;
};

class ConsoleMarketDataSubscriber final : public optionx::market_data::IMarketDataSubscriber {
public:
    void on_tick_data(const optionx::market_data::TickDataBatch& batch) override {
        std::cout << "[ticks] "
                  << batch.symbol
                  << " items=" << batch.items.size();
        if (!batch.items.empty()) {
            std::cout << " last=" << batch.items.back().mid_price(batch.price_digits);
        }
        std::cout << '\n';
    }

    void on_bar_data(const optionx::market_data::BarDataBatch& batch) override {
        std::cout << "[bars] "
                  << batch.symbol
                  << " timeframe=" << batch.timeframe
                  << " items=" << batch.items.size()
                  << '\n';
    }

    void on_market_data_status(
            const optionx::market_data::MarketDataStatusUpdate& update) override {
        std::cout << "[status] "
                  << update.symbol
                  << " "
                  << optionx::market_data::to_str(update.type)
                  << " -> "
                  << optionx::market_data::to_str(update.status);
        if (update.subscription.valid()) {
            std::cout << " subscription_id=" << update.subscription.id;
        }
        std::cout << '\n';
    }
};

optionx::market_data::MarketDataStatusUpdate make_ready_status() {
    const optionx::market_data::TickSubscriptionRequest request(
        "BTCUSDT",
        optionx::market_data::MarketDataTransport::WEBSOCKET);

    optionx::market_data::MarketDataStatusUpdate update;
    update.provider_id = 1;
    update.subscription = optionx::market_data::MarketDataSubscriptionHandle::from_tick_request(
        update.provider_id,
        42,
        request);
    update.type = optionx::market_data::MarketDataType::TICKS;
    update.symbol = request.symbol;
    update.transport = request.transport;
    update.status = optionx::market_data::MarketDataStreamStatus::READY;
    return update;
}

std::unique_ptr<optionx::market_data::TickDataBatch> make_tick_batch() {
    auto batch = std::make_unique<optionx::market_data::TickDataBatch>();
    batch->type = optionx::market_data::MarketDataType::TICKS;
    batch->symbol = "BTCUSDT";
    batch->price_digits = 2;
    batch->items.push_back(optionx::Tick(
        61521.30,
        61521.38,
        61521.34,
        0.00017,
        1783028778697ULL,
        0,
        0));
    optionx::mark_live_payload(batch->items.back().flags);
    return batch;
}

std::unique_ptr<optionx::market_data::BarDataBatch> make_bar_batch() {
    auto batch = std::make_unique<optionx::market_data::BarDataBatch>();
    batch->type = optionx::market_data::MarketDataType::BARS;
    batch->symbol = "BTCUSDT";
    batch->timeframe = 60;
    batch->price_digits = 2;
    batch->items.push_back(optionx::Bar(
        61520.00,
        61522.00,
        61519.75,
        61521.34,
        10.0,
        1783028760000ULL));
    optionx::mark_live_payload(batch->items.back().flags);
    batch->items.back().set_flag(optionx::MarketDataFlags::INCOMPLETE);
    return batch;
}

} // namespace

int main() {
    DemoMarketDataProvider provider;
    optionx::market_data::MarketDataHub hub;

    // The hub binds provider callbacks and routes them to all live subscribers.
    // Call unbind_from() before destroying the hub if the provider can outlive it.
    hub.bind_to(provider);

    // MarketDataHub stores weak references. Keep this shared_ptr alive while
    // the chart/bot should receive market-data events.
    auto chart = std::make_shared<ConsoleMarketDataSubscriber>();
    const auto subscriber_id = hub.add_subscriber(chart);
    if (subscriber_id == optionx::market_data::MarketDataHub::INVALID_SUBSCRIBER_ID) {
        std::cerr << "failed to add market-data subscriber\n";
        return 1;
    }

    provider.emit_status(make_ready_status());
    provider.emit_ticks(make_tick_batch());
    provider.emit_bars(make_bar_batch());

    // Late subscribers receive cached stream status, but not old tick/bar data.
    auto late_chart = std::make_shared<ConsoleMarketDataSubscriber>();
    hub.add_subscriber(late_chart);

    hub.remove_subscriber(subscriber_id);
    hub.unbind_from(provider);
    return 0;
}
