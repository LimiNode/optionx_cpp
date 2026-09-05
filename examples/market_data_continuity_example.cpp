#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace market_data_continuity_example_clock {
    inline std::uint64_t now_ms = 180000ULL;
}

#ifndef OPTIONX_TIMESTAMP_MS
#define OPTIONX_TIMESTAMP_MS market_data_continuity_example_clock::now_ms
#endif

#include <optionx_cpp/market_data.hpp>

namespace md = optionx::market_data;

namespace {

class DemoBarProvider final : public md::BaseMarketDataProvider {
public:
    bool subscribe_bars(
            md::BarSubscriptionRequest request,
            subscription_callback_t callback) override {
        m_subscription = md::MarketDataSubscriptionHandle::from_bar_request(
            provider_id(),
            m_next_subscription_id++,
            request);
        if (callback) {
            callback(md::MarketDataSubscriptionResult::subscribed(m_subscription));
        }
        return true;
    }

    bool unsubscribe(
            md::MarketDataSubscriptionHandle subscription,
            subscription_callback_t callback) override {
        if (callback) {
            callback(md::MarketDataSubscriptionResult::unsubscribed(
                std::move(subscription)));
        }
        m_subscription = {};
        return true;
    }

    bool fetch_bar_history(
            const optionx::BarHistoryRequest& request,
            bar_history_callback_t callback) override {
        std::cout << "history request: " << request.symbol
                  << " [" << request.from_ts << ", " << request.to_ts << "]\n";
        m_history_callbacks.push_back(std::move(callback));
        return true;
    }

    void emit_live_bar(std::uint64_t time_ms, double close) {
        auto batch = std::make_unique<md::BarDataBatch>();
        batch->subscription = m_subscription;
        batch->type = md::MarketDataType::BARS;
        batch->symbol = m_subscription.symbol;
        batch->timeframe = m_subscription.timeframe;
        batch->items.emplace_back(close - 0.1, close + 0.2, close - 0.3, close, 1.0, time_ms);
        optionx::mark_live_payload(batch->items.back().flags);
        if (on_bar_data()) on_bar_data()(std::move(batch));
    }

    void emit_status(md::MarketDataStreamStatus status) {
        if (!on_market_data_status()) return;
        md::MarketDataStatusUpdate update;
        update.subscription = m_subscription;
        update.type = md::MarketDataType::BARS;
        update.symbol = m_subscription.symbol;
        update.timeframe = m_subscription.timeframe;
        update.transport = m_subscription.transport;
        update.status = status;
        on_market_data_status()(std::move(update));
    }

    void complete_history(std::vector<optionx::Bar> bars) {
        if (m_history_callbacks.empty()) return;

        auto callback = std::move(m_history_callbacks.front());
        m_history_callbacks.erase(m_history_callbacks.begin());

        optionx::BarSequence sequence;
        sequence.symbol = "EURUSD";
        sequence.provider = "demo-provider";
        sequence.timeframe = 60;
        sequence.price_digits = 5;
        sequence.volume_digits = 0;
        sequence.price_source = optionx::BarPriceSource::MID;
        sequence.bars = std::move(bars);
        callback(optionx::BarHistoryResult::ok(std::move(sequence)));
    }

private:
    md::SubscriptionId m_next_subscription_id = 1;
    md::MarketDataSubscriptionHandle m_subscription;
    std::vector<bar_history_callback_t> m_history_callbacks;
};

class Chart final : public md::IMarketDataSubscriber {
public:
    void on_bar_data(const md::BarDataBatch& batch) override {
        for (const auto& bar : batch.items) {
            const char* source = "live";
            if (bar.has_flag(optionx::MarketDataFlags::HISTORICAL)) {
                source = bar.has_flag(optionx::MarketDataFlags::BACKFILL)
                    ? "backfill"
                    : "historical";
            }
            const char* delivery =
                bar.has_flag(optionx::MarketDataFlags::CATCHUP)
                ? "catchup"
                : bar.has_flag(optionx::MarketDataFlags::REALTIME)
                ? "realtime"
                : "history";
            std::cout << "chart bar: route provider subscription #"
                      << batch.subscription.id
                      << ", t=" << bar.time_ms
                      << ", source=" << source
                      << ", delivery=" << delivery << '\n';
        }
    }

    void on_market_data_continuity(
            const md::MarketDataContinuityUpdate& update) override {
        std::cout << "continuity: subscription #" << update.subscription.id
                  << ", status=" << md::to_str(update.status)
                  << ", history items=" << update.delivered_items << '\n';
    }

    void on_market_data_status(
            const md::MarketDataStatusUpdate& update) override {
        std::cout << "transport: subscription #" << update.subscription.id
                  << ", status=" << md::to_str(update.status) << '\n';
    }
};

std::vector<optionx::Bar> make_bars(
        std::initializer_list<std::uint64_t> timestamps) {
    std::vector<optionx::Bar> bars;
    for (const auto timestamp : timestamps) {
        bars.emplace_back(100.0, 101.0, 99.0, 100.5, 1.0, timestamp);
    }
    return bars;
}

} // namespace

int main() {
    DemoBarProvider provider;
    md::MarketDataRouter router;
    auto chart = std::make_shared<Chart>();

    md::BarSubscriptionRequest request(
        "EURUSD",
        60,
        optionx::BarPriceSource::MID,
        md::MarketDataTransport::WEBSOCKET);
    request.continuity.mode = md::MarketDataContinuityMode::PREFILL_AND_RECOVER;
    request.continuity.prefill_bars = 2;
    request.continuity.max_backfill_bars = 10;
    request.continuity.max_buffered_batches = 32;
    request.continuity.max_buffered_items = 256;
    request.continuity.retry.max_attempts = 3;
    request.continuity.retry.initial_backoff_ms = 100;
    request.continuity.retry.max_backoff_ms = 1000;

    auto route = router.subscribe_bars(provider, chart, request);
    if (!route.active()) {
        std::cerr << "could not create a bar route\n";
        return 1;
    }

    market_data_continuity_example_clock::now_ms = 240000ULL;
    provider.emit_live_bar(240000, 100.5);
    provider.complete_history(make_bars({120000, 180000}));

    // This live bar arrives after the initial history and is delivered directly.
    market_data_continuity_example_clock::now_ms = 300000ULL;
    provider.emit_live_bar(300000, 101.0);

    // 360000 is missing, so the Router requests it before releasing 420000.
    market_data_continuity_example_clock::now_ms = 420000ULL;
    provider.emit_live_bar(420000, 101.5);
    provider.complete_history(make_bars({360000}));

    // A reconnect invalidates the route. The 420000 live snapshot is held
    // while Router revalidates the closed 360000 and 420000 slots.
    market_data_continuity_example_clock::now_ms = 480000ULL;
    // process() cannot start that history request before READY has been observed.
    provider.emit_status(md::MarketDataStreamStatus::DISCONNECTED);
    provider.emit_live_bar(420000, 102.0);
    router.process();
    provider.emit_status(md::MarketDataStreamStatus::READY);
    provider.complete_history(make_bars({360000, 420000}));

    route.reset();
    router.shutdown();
    return 0;
}
