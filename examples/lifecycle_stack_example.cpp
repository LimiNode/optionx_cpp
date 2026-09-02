#include <optionx_cpp/lifecycle.hpp>
#include <optionx_cpp/market_data.hpp>

#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace {

namespace lifecycle = optionx::lifecycle;
namespace market_data = optionx::market_data;

class OwnerLoop final : public lifecycle::ILifecycleModule {
public:
    bool post(std::function<void()> task) {
        if (!task) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown_requested) return false;
        m_tasks.push_back(std::move(task));
        return true;
    }

    void process() override {
        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            tasks.swap(m_tasks);
        }
        for (auto& task : tasks) task();

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown_requested && m_tasks.empty()) m_stopped = true;
    }

    void shutdown() noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown_requested = true;
        if (m_tasks.empty()) m_stopped = true;
    }

    [[nodiscard]] bool is_stopped() const noexcept override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopped;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::function<void()>> m_tasks;
    bool m_shutdown_requested = false;
    bool m_stopped = false;
};

class DeferredProvider final : public market_data::BaseMarketDataProvider {
public:
    explicit DeferredProvider(OwnerLoop& owner_loop)
            : m_owner_loop(owner_loop) {}

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
        return m_owner_loop.post(
            [subscription = std::move(subscription),
             callback = std::move(callback)]() mutable {
                if (callback) {
                    callback(market_data::MarketDataSubscriptionResult::unsubscribed(
                        std::move(subscription)));
                }
            });
    }

private:
    OwnerLoop& m_owner_loop;
    market_data::SubscriptionId m_next_subscription_id = 1;
};

class QuoteSink final : public market_data::IMarketDataSubscriber {};

} // namespace

int main() {
    OwnerLoop owner_loop;
    DeferredProvider provider(owner_loop);
    market_data::MarketDataRouter router(
        [&owner_loop](market_data::MarketDataRouter::owner_task_t task) {
            return owner_loop.post(std::move(task));
        });
    auto subscriber = std::make_shared<QuoteSink>();

    lifecycle::LifecycleStack application;
    // Registration order declares dependencies: executor first, Router next.
    if (!application.add_module(owner_loop) ||
        !application.add_module(router)) {
        std::cerr << "Lifecycle registration failed\n";
        return 1;
    }

    auto route = router.subscribe_ticks(
        provider,
        subscriber,
        market_data::TickSubscriptionRequest("EURUSD"));
    application.process();
    if (!route.active()) {
        std::cerr << "Subscription did not become active\n";
        return 1;
    }

    application.shutdown();
    while (!application.is_stopped()) {
        application.process();
    }
    if (route.valid()) {
        std::cerr << "Routed subscription cleanup did not finish\n";
        return 1;
    }

    std::cout << "Lifecycle stopped after routed subscription cleanup\n";
    return 0;
}
