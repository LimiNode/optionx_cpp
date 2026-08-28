#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <typeindex>

#include <optionx_cpp/platforms.hpp>

namespace {

class TestEvent final : public optionx::utils::Event {
public:
    std::type_index type() const override {
        return typeid(TestEvent);
    }

    const char* name() const override {
        return "TestEvent";
    }
};

class TestMediator final : public optionx::utils::EventMediator {
public:
    using optionx::utils::EventMediator::EventMediator;

    void on_event(const optionx::utils::Event* const) override {}
};

class TestPlatform final : public optionx::platforms::BaseTradingPlatform {
public:
    TestPlatform()
        : optionx::platforms::BaseTradingPlatform(
              std::make_shared<
                  optionx::platforms::intrade_bar::AccountInfoData>()) {}

    optionx::PlatformType platform_type() const override {
        return optionx::PlatformType::INTRADE_BAR;
    }

    int initialize_count() const noexcept {
        return m_initialize_count;
    }

    int loop_count() const noexcept {
        return m_loop_count;
    }

private:
    void on_once() override {
        ++m_initialize_count;
    }

    void on_loop() override {
        ++m_loop_count;
    }

    int m_initialize_count = 0;
    int m_loop_count = 0;
};

} // namespace

TEST(BaseTradingPlatformLifecycle, RepeatedRunDoesNotDuplicateLifecycleTasks) {
    TestPlatform platform;

    platform.run(false);
    platform.run(false);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    platform.process();

    EXPECT_EQ(platform.initialize_count(), 1);
    EXPECT_EQ(platform.loop_count(), 1);

    platform.shutdown();
}

TEST(BaseTradingPlatformLifecycle, ConcurrentRunDoesNotDuplicateLifecycleTasks) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        TestPlatform platform;
        std::atomic<bool> start{false};

        auto run_platform = [&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            platform.run(false);
        };

        std::thread first(run_platform);
        std::thread second(run_platform);

        start.store(true, std::memory_order_release);
        first.join();
        second.join();

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        platform.process();

        EXPECT_EQ(platform.initialize_count(), 1);
        EXPECT_EQ(platform.loop_count(), 1);

        platform.shutdown();
    }
}

TEST(BaseTradingPlatformLifecycle, ShutdownBeforeProcessSkipsLifecycleCallbacks) {
    TestPlatform platform;

    platform.run(false);
    platform.shutdown();
    platform.process();

    EXPECT_EQ(platform.initialize_count(), 0);
    EXPECT_EQ(platform.loop_count(), 0);
}

TEST(BaseTradingPlatformLifecycle, PostsForeignThreadWorkToTheOwnerLoop) {
    TestPlatform platform;
    platform.run(false);
    std::atomic<bool> accepted{false};
    std::atomic<bool> executed{false};
    std::thread::id callback_thread;

    std::thread bot_thread([&]() {
        accepted.store(
            platform.post_task([&]() {
                callback_thread = std::this_thread::get_id();
                executed.store(true, std::memory_order_release);
            }),
            std::memory_order_release);
    });
    bot_thread.join();

    EXPECT_TRUE(accepted.load(std::memory_order_acquire));
    EXPECT_FALSE(executed.load(std::memory_order_acquire));

    const auto owner_thread = std::this_thread::get_id();
    platform.process();

    EXPECT_TRUE(executed.load(std::memory_order_acquire));
    EXPECT_EQ(callback_thread, owner_thread);

    platform.shutdown();
    EXPECT_FALSE(platform.post_task([]() {}));
}

TEST(BaseTradingPlatformLifecycle, ConcurrentRunAndShutdownDoNotRunAfterStop) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        TestPlatform platform;
        std::atomic<bool> start{false};

        std::thread run_thread([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            platform.run(false);
        });

        std::thread shutdown_thread([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            platform.shutdown();
        });

        start.store(true, std::memory_order_release);
        run_thread.join();
        shutdown_thread.join();

        platform.process();

        EXPECT_EQ(platform.initialize_count(), 0);
        EXPECT_EQ(platform.loop_count(), 0);
    }
}

TEST(EventBusSafety, NullEventsAreIgnored) {
    optionx::utils::EventBus bus;
    TestMediator mediator(bus);
    int callback_count = 0;

    mediator.subscribe<TestEvent>(
        [&callback_count](const TestEvent&) {
            ++callback_count;
        });

    EXPECT_NO_THROW(bus.notify(static_cast<const optionx::utils::Event*>(nullptr)));
    EXPECT_NO_THROW(bus.notify_async(nullptr));
    EXPECT_NO_THROW(bus.process());

    TestEvent event;
    bus.notify(event);

    EXPECT_EQ(callback_count, 1);
}

TEST(EventMediatorSafety, NullBusIsANoop) {
    TestEvent event;

    EXPECT_NO_THROW({
        TestMediator mediator(static_cast<optionx::utils::EventBus*>(nullptr));
        mediator.subscribe<TestEvent>(
            [](const TestEvent&) {});
        mediator.subscribe<TestEvent>();
        mediator.unsubscribe<TestEvent>();
        mediator.unsubscribe_all();
        mediator.notify(static_cast<const optionx::utils::Event*>(nullptr));
        mediator.notify(event);
        mediator.notify_async(std::make_unique<TestEvent>());
        mediator.await_once<TestEvent>(
            [](const TestEvent&) {});
    });
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
