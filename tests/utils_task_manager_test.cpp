#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <optionx_cpp/utils.hpp>

TEST(LogRedactionTest, RedactsNonEmptySecrets) {
    EXPECT_TRUE(optionx::utils::redact_secret_value("").empty());
    EXPECT_EQ(optionx::utils::redact_secret_value("session=abc"), "***");
    EXPECT_EQ(optionx::utils::redact_secret("legacy-token"), "***");
}

TEST(LogRedactionTest, RedactsSecretFieldsInsideText) {
    using optionx::utils::redact_secrets_in_text;

    EXPECT_EQ(
        redact_secrets_in_text("status=ok token=abc123 user_hash=deadbeef"),
        "status=ok token=*** user_hash=***");
    EXPECT_EQ(
        redact_secrets_in_text("password=secret&symbol=EURUSD"),
        "password=***&symbol=EURUSD");
    EXPECT_EQ(
        redact_secrets_in_text("Cookie: user_id=42; user_hash=deadbeef"),
        "Cookie: ***");
    EXPECT_EQ(
        redact_secrets_in_text("plain diagnostic text"),
        "plain diagnostic text");
}

TEST(TaskManagerTest, RejectsInvalidPeriodicPeriods) {
    optionx::utils::TaskManager manager;
    auto callback = [](std::shared_ptr<optionx::utils::Task>) {};

    EXPECT_FALSE(manager.add_periodic_task(0, callback));
    EXPECT_FALSE(manager.add_periodic_task(-1, callback));
    EXPECT_FALSE(manager.add_delayed_periodic_task(1, 0, callback));
    EXPECT_FALSE(manager.add_periodic_on_date_task(1, 0, callback));
    EXPECT_FALSE(manager.add_periodic_task("bad-periodic", 0, callback));
    EXPECT_FALSE(manager.add_delayed_periodic_task("bad-delayed-periodic", 1, 0, callback));
    EXPECT_FALSE(manager.add_periodic_on_date_task("bad-periodic-on-date", 1, 0, callback));
}

TEST(TaskManagerTest, RejectsQueueInsertionAfterShutdownBegins) {
    optionx::utils::TaskManager manager;
    std::atomic<bool> task_started{false};
    std::atomic<bool> release_task{false};

    ASSERT_TRUE(manager.add_single_task(
        [&task_started, &release_task](std::shared_ptr<optionx::utils::Task>) {
            task_started.store(true, std::memory_order_release);
            while (!release_task.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }));
    manager.run();

    for (int i = 0; i < 1000 && !task_started.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(task_started.load(std::memory_order_acquire));

    auto shutdown_future = std::async(std::launch::async, [&manager]() {
        manager.shutdown();
    });

    bool rejected = false;
    for (int i = 0; i < 10000 && !rejected; ++i) {
        rejected = !manager.add_single_task(
            [](std::shared_ptr<optionx::utils::Task>) {});
        if (!rejected) std::this_thread::yield();
    }

    release_task.store(true, std::memory_order_release);
    ASSERT_EQ(shutdown_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    shutdown_future.get();
    EXPECT_TRUE(rejected);
}

TEST(TaskTest, KeepsPreviousPeriodWhenSetPeriodIsInvalid) {
    int calls = 0;
    auto task = std::make_shared<optionx::utils::Task>(
        optionx::utils::TaskType::PERIODIC,
        [&calls](std::shared_ptr<optionx::utils::Task>) {
            ++calls;
        },
        0,
        10);

    EXPECT_FALSE(task->set_period(0));
    EXPECT_TRUE(task->set_period(1));
}

TEST(TaskTest, InitializesExecutionTimeBeforeProcessing) {
    auto callback = [](std::shared_ptr<optionx::utils::Task>) {};
    const auto before_ms = optionx::utils::Task::get_current_time();

    optionx::utils::Task single(
        optionx::utils::TaskType::SINGLE,
        callback);
    optionx::utils::Task delayed(
        optionx::utils::TaskType::DELAYED_SINGLE,
        callback,
        5000);
    optionx::utils::Task periodic(
        optionx::utils::TaskType::PERIODIC,
        callback,
        0,
        7000);
    optionx::utils::Task delayed_periodic(
        optionx::utils::TaskType::DELAYED_PERIODIC,
        callback,
        3000,
        7000);
    optionx::utils::Task on_date(
        optionx::utils::TaskType::ON_DATE,
        callback,
        0,
        0,
        123456789);
    optionx::utils::Task periodic_on_date(
        optionx::utils::TaskType::PERIODIC_ON_DATE,
        callback,
        0,
        7000,
        123456789);

    const auto after_ms = optionx::utils::Task::get_current_time();

    EXPECT_GE(single.get_next_execution_time(), before_ms);
    EXPECT_LE(single.get_next_execution_time(), after_ms);
    EXPECT_GE(delayed.get_next_execution_time(), before_ms + 5000);
    EXPECT_LE(delayed.get_next_execution_time(), after_ms + 5000);
    EXPECT_GE(periodic.get_next_execution_time(), before_ms + 7000);
    EXPECT_LE(periodic.get_next_execution_time(), after_ms + 7000);
    EXPECT_GE(delayed_periodic.get_next_execution_time(), before_ms + 3000);
    EXPECT_LE(delayed_periodic.get_next_execution_time(), after_ms + 3000);
    EXPECT_EQ(on_date.get_next_execution_time(), 123456789);
    EXPECT_EQ(periodic_on_date.get_next_execution_time(), 123456789);
}

TEST(TaskTest, RescheduleUpdatesExecutionTimeBeforeProcessing) {
    auto callback = [](std::shared_ptr<optionx::utils::Task>) {};
    optionx::utils::Task task(
        optionx::utils::TaskType::DELAYED_SINGLE,
        callback,
        5000);

    task.reschedule_at(123456789);
    EXPECT_EQ(task.get_next_execution_time(), 123456789);

    const auto before_ms = optionx::utils::Task::get_current_time();
    task.reschedule_in(5000);
    const auto after_ms = optionx::utils::Task::get_current_time();

    EXPECT_GE(task.get_next_execution_time(), before_ms + 5000);
    EXPECT_LE(task.get_next_execution_time(), after_ms + 5000);
}

TEST(TaskTest, SetPeriodUpdatesExecutionTimeBeforeProcessing) {
    auto callback = [](std::shared_ptr<optionx::utils::Task>) {};
    const auto before_ms = optionx::utils::Task::get_current_time();

    optionx::utils::Task task(
        optionx::utils::TaskType::PERIODIC,
        callback,
        0,
        5000);

    ASSERT_TRUE(task.set_period(8000));

    const auto after_ms = optionx::utils::Task::get_current_time();

    EXPECT_GE(task.get_next_execution_time(), before_ms + 8000);
    EXPECT_LE(task.get_next_execution_time(), after_ms + 8000);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
