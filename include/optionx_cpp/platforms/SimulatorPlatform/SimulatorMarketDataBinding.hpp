#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_MARKET_DATA_BINDING_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_MARKET_DATA_BINDING_HPP_INCLUDED
/// \file SimulatorMarketDataBinding.hpp
/// \brief Scoped existing-Router subscription; host retains provider through Router cleanup.
#include <cmath>

namespace optionx::platforms {
struct SimulatorMarketDataConfig {
    market_data::MarketDataProviderId provider_key;
    std::string provider_symbol;
    std::string simulator_symbol;
    SimulatorPriceSide price_side = SimulatorPriceSide::REFERENCE;
    std::uint32_t price_decimals = 5;
    std::int64_t max_quote_age_ms = 0;
};

/// \class SimulatorMarketDataBinding
/// \brief Single-owner scoped binding, not a new provider or subscription framework.
/// Host owns Router and keeps providers alive through pending subscribe/unsubscribe
/// and Router shutdown. Callbacks must dispatch to this same owner thread.
class SimulatorMarketDataBinding {
public:
    using clock_t = std::function<std::int64_t()>;
    SimulatorMarketDataBinding(market_data::MarketDataRouter& router,
        std::shared_ptr<market_data::BaseMarketDataProvider> provider,
        std::shared_ptr<SimulatorQuoteBook> book, SimulatorMarketDataConfig config, clock_t clock)
        : m_provider(std::move(provider)), m_book(std::move(book)) {
        if (!m_provider || !m_book || !clock || !config.provider_key ||
            config.provider_symbol.empty() || config.provider_symbol.size() > 64 ||
            config.price_decimals > 8 || config.max_quote_age_ms < 0 ||
            config.price_side < SimulatorPriceSide::BID || config.price_side > SimulatorPriceSide::REFERENCE ||
            router.registered_provider_id(m_provider->provider_id()) != config.provider_key)
            throw std::invalid_argument("simulator market-data binding");
        m_generation = m_book->bind(config.simulator_symbol, config.max_quote_age_ms);
        if (!m_generation) throw std::invalid_argument("simulator source already bound or capacity exhausted");
        try {
            m_subscriber = std::make_shared<Subscriber>(m_book, config, std::move(clock),
                m_provider->provider_id(), m_generation);
            const std::weak_ptr<Subscriber> weak = m_subscriber;
            m_route = router.subscribe_ticks(config.provider_key, m_subscriber,
                market_data::TickSubscriptionRequest(config.provider_symbol), [weak](auto result) {
                    if (const auto subscriber = weak.lock()) subscriber->accepted(result);
                });
            if (!m_route.valid()) throw std::runtime_error("simulator route rejected");
        } catch (...) {
            if (m_subscriber) m_subscriber->enabled.store(false);
            m_book->unbind(m_generation);
            throw;
        }
    }
    SimulatorMarketDataBinding(const SimulatorMarketDataBinding&) = delete;
    SimulatorMarketDataBinding& operator=(const SimulatorMarketDataBinding&) = delete;
    ~SimulatorMarketDataBinding() { shutdown(); }
    void shutdown() noexcept {
        if (m_subscriber) m_subscriber->enabled.store(false);
        m_book->unbind(m_generation);
        m_route.reset();
        m_subscriber.reset();
    }
    std::uint64_t generation() const noexcept { return m_generation; }
    market_data::MarketDataSubscriptionHandle subscription() const { return m_route.provider_subscription(); }
private:
    struct Subscriber : market_data::IMarketDataSubscriber {
        std::weak_ptr<SimulatorQuoteBook> book;
        SimulatorMarketDataConfig config;
        clock_t clock;
        std::atomic_bool enabled{true};
        const std::thread::id owner = std::this_thread::get_id();
        std::uint64_t instance;
        std::uint64_t generation;
        market_data::MarketDataSubscriptionHandle handle;
        Subscriber(std::shared_ptr<SimulatorQuoteBook> value, SimulatorMarketDataConfig settings,
                   clock_t time, std::uint64_t provider, std::uint64_t epoch)
            : book(value), config(std::move(settings)), clock(std::move(time)), instance(provider), generation(epoch) {}
        bool on_owner(const std::shared_ptr<SimulatorQuoteBook>& value) const {
            if (!enabled.load()) return false;
            if (owner == std::this_thread::get_id()) return true;
            value->block(); return false;
        }
        bool matches(const market_data::MarketDataSubscriptionHandle& incoming) const {
            return handle.valid() && incoming.provider_id == instance && incoming.id == handle.id &&
                incoming.symbol == config.provider_symbol && incoming.stream_type == market_data::MarketDataType::TICKS;
        }
        void accepted(const market_data::MarketDataSubscriptionResult& result) {
            if (auto value = book.lock()) {
                if (!on_owner(value)) return;
                if (result.status != market_data::MarketDataSubscriptionStatus::SUBSCRIBED ||
                    result.subscription.provider_id != instance || result.subscription.symbol != config.provider_symbol) {
                    value->accept(generation, false); return;
                }
                handle = result.subscription;
                value->accept(generation, true);
            }
        }
        void on_market_data_status(const market_data::MarketDataStatusUpdate& update) override {
            if (auto value = book.lock()) {
                if (!on_owner(value) || !matches(update.subscription)) return;
                try { value->status(generation, update.status == market_data::MarketDataStreamStatus::READY, clock()); }
                catch (...) { value->block(); }
            }
        }
        static std::optional<std::int64_t> units(const Tick& tick, SimulatorPriceSide side, std::uint32_t decimals) {
            double price = 0;
            switch (side) {
            case SimulatorPriceSide::BID: price = tick.bid; break;
            case SimulatorPriceSide::ASK: price = tick.ask; break;
            case SimulatorPriceSide::REFERENCE: price = tick.reference; break;
            case SimulatorPriceSide::MID:
                if (!std::isfinite(tick.bid) || !std::isfinite(tick.ask) || tick.bid <= 0 || tick.ask < tick.bid) return {};
                price = tick.bid / 2 + tick.ask / 2; break;
            }
            if (!std::isfinite(price) || price <= 0) return {};
            std::int64_t scale = 1;
            for (std::uint32_t i = 0; i < decimals; ++i) scale *= 10;
            const auto rounded = std::round(price * static_cast<double>(scale));
            if (!std::isfinite(rounded) || rounded < 1 || rounded >= std::ldexp(1.0, 63)) return {};
            return utils::to_fixed_point(price, scale);
        }
        void on_tick_data(const market_data::TickDataBatch& batch) override {
            if (auto value = book.lock()) {
                if (!on_owner(value) || !matches(batch.subscription)) return;
                try {
                    if (batch.items.size() > 4096 || batch.price_digits != config.price_decimals) { value->block(); return; }
                    const auto received = clock();
                    for (const auto& tick : batch.items) {
                        if (tick.time_ms > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
                            tick.has_flag(MarketDataFlags::HISTORICAL) || tick.has_flag(MarketDataFlags::BACKFILL)) continue;
                        const auto price = units(tick, config.price_side, config.price_decimals);
                        if (!price) continue;
                        SimulatorTick quote{config.simulator_symbol, static_cast<std::int64_t>(tick.time_ms), *price};
                        quote.received_ms = received;
                        quote.provider_key = config.provider_key.value();
                        quote.provider_instance = instance;
                        quote.provider_received_ms = tick.received_ms;
                        quote.price_decimals = config.price_decimals;
                        quote.price_side = config.price_side;
                        value->observe(generation, std::move(quote));
                    }
                } catch (...) { value->block(); }
            }
        }
    };
    std::shared_ptr<market_data::BaseMarketDataProvider> m_provider;
    std::shared_ptr<SimulatorQuoteBook> m_book;
    std::uint64_t m_generation = 0;
    std::shared_ptr<Subscriber> m_subscriber;
    market_data::MarketDataRouterSubscription m_route;
};
} // namespace optionx::platforms
#endif // OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_MARKET_DATA_BINDING_HPP_INCLUDED
