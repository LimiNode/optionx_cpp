#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <optionx_cpp/platforms.hpp>

namespace {

struct ConnectionCallbackState {
    int count = 0;
    bool success = true;
    std::string reason;
};

void pump_platform(
        optionx::platforms::IntradeBarPlatform& platform,
        int iterations = 100) {
    for (int i = 0; i < iterations; ++i) {
        platform.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

TEST(IntradeBarAuthLifecycle, SupersededConnectCallbackIsCancelledOnce) {
    optionx::platforms::IntradeBarPlatform platform;
    ConnectionCallbackState first;
    ConnectionCallbackState second;

    platform.run(false);
    platform.connect([&first](optionx::ConnectionResult result) {
        ++first.count;
        first.success = result.success;
        first.reason = std::move(result.reason);
    });
    platform.connect([&second](optionx::ConnectionResult result) {
        ++second.count;
        second.success = result.success;
        second.reason = std::move(result.reason);
    });

    pump_platform(platform);
    platform.shutdown();

    EXPECT_EQ(first.count, 1);
    EXPECT_FALSE(first.success);
    EXPECT_NE(
        first.reason.find("Authentication cancelled: connect-request"),
        std::string::npos);

    EXPECT_EQ(second.count, 1);
    EXPECT_FALSE(second.success);
    EXPECT_EQ(second.reason, "Authentication data is missing.");
}

TEST(IntradeBarAuthLifecycle, DisconnectCancelsPendingConnectCallbackOnce) {
    optionx::platforms::IntradeBarPlatform platform;
    ConnectionCallbackState connect;
    ConnectionCallbackState disconnect;

    platform.run(false);
    platform.connect([&connect](optionx::ConnectionResult result) {
        ++connect.count;
        connect.success = result.success;
        connect.reason = std::move(result.reason);
    });
    platform.disconnect([&disconnect](optionx::ConnectionResult result) {
        ++disconnect.count;
        disconnect.success = result.success;
        disconnect.reason = std::move(result.reason);
    });

    pump_platform(platform);
    platform.shutdown();

    EXPECT_EQ(connect.count, 1);
    EXPECT_FALSE(connect.success);
    EXPECT_NE(
        connect.reason.find("Authentication cancelled: disconnect-request"),
        std::string::npos);

    EXPECT_EQ(disconnect.count, 1);
    EXPECT_FALSE(disconnect.success);
    EXPECT_EQ(disconnect.reason, "Already disconnected.");
}

TEST(IntradeBarAuthLifecycle, TerminalFailureIsNotCancelledAgainOnShutdown) {
    optionx::platforms::IntradeBarPlatform platform;
    ConnectionCallbackState connect;

    platform.run(false);
    platform.connect([&connect](optionx::ConnectionResult result) {
        ++connect.count;
        connect.success = result.success;
        connect.reason = std::move(result.reason);
    });

    pump_platform(platform);
    ASSERT_EQ(connect.count, 1);
    EXPECT_FALSE(connect.success);
    EXPECT_EQ(connect.reason, "Authentication data is missing.");

    platform.shutdown();

    EXPECT_EQ(connect.count, 1);
    EXPECT_EQ(connect.reason, "Authentication data is missing.");
}

TEST(IntradeBarAuthLifecycle, TerminalFailureIsNotCancelledAgainOnDisconnect) {
    optionx::platforms::IntradeBarPlatform platform;
    ConnectionCallbackState connect;
    ConnectionCallbackState disconnect;

    platform.run(false);
    platform.connect([&connect](optionx::ConnectionResult result) {
        ++connect.count;
        connect.success = result.success;
        connect.reason = std::move(result.reason);
    });

    pump_platform(platform);
    ASSERT_EQ(connect.count, 1);
    EXPECT_FALSE(connect.success);
    EXPECT_EQ(connect.reason, "Authentication data is missing.");

    platform.disconnect([&disconnect](optionx::ConnectionResult result) {
        ++disconnect.count;
        disconnect.success = result.success;
        disconnect.reason = std::move(result.reason);
    });

    pump_platform(platform);
    platform.shutdown();

    EXPECT_EQ(connect.count, 1);
    EXPECT_EQ(connect.reason, "Authentication data is missing.");

    EXPECT_EQ(disconnect.count, 1);
    EXPECT_FALSE(disconnect.success);
    EXPECT_EQ(disconnect.reason, "Already disconnected.");
}

TEST(IntradeBarAuthLifecycle, InvalidAuthDataReportsValidationFailure) {
    optionx::platforms::IntradeBarPlatform platform;
    auto auth_data = std::make_unique<optionx::platforms::intrade_bar::AuthData>();
    auth_data->set_email_password("user@example.test", "password");
    auth_data->host.clear();
    auth_data->auto_find_domain = false;

    bool callback_called = false;
    bool success = true;
    std::string message;
    auth_data->add_callback([&](bool result, const std::string& reason) {
        callback_called = true;
        success = result;
        message = reason;
    });

    ASSERT_TRUE(platform.configure_auth(std::move(auth_data)));
    platform.event_bus().drain();

    EXPECT_TRUE(callback_called);
    EXPECT_FALSE(success);
    EXPECT_EQ(message, "Host is empty and auto_find_domain is disabled");
    platform.shutdown();
}

TEST(IntradeBarPublicRequests, OwnerLoopAndForeignThreadCallsHaveOneTerminalCallback) {
    optionx::platforms::IntradeBarPlatform platform;
    platform.run(false);

    std::atomic<int> callback_count{0};
    std::atomic<bool> foreign_accepted{false};
    optionx::TradeHistoryResult owner_result;
    optionx::TradeHistoryResult foreign_result;

    ASSERT_TRUE(platform.fetch_trade_history(
        optionx::TradeHistoryRequest::all(),
        [&callback_count, &owner_result](optionx::TradeHistoryResult result) {
            owner_result = std::move(result);
            callback_count.fetch_add(1, std::memory_order_release);
        }));
    platform.process();

    std::thread caller([&]() {
        foreign_accepted.store(platform.fetch_trade_history(
            optionx::TradeHistoryRequest::all(),
            [&callback_count, &foreign_result](optionx::TradeHistoryResult result) {
                foreign_result = std::move(result);
                callback_count.fetch_add(1, std::memory_order_release);
            }),
            std::memory_order_release);
    });
    caller.join();

    EXPECT_TRUE(foreign_accepted.load(std::memory_order_acquire));
    EXPECT_EQ(callback_count.load(std::memory_order_acquire), 1);
    platform.process();

    EXPECT_EQ(callback_count.load(std::memory_order_acquire), 2);
    EXPECT_FALSE(owner_result.success);
    EXPECT_FALSE(foreign_result.success);
    platform.shutdown();
}

TEST(IntradeBarPublicRequests, AcceptedRequestsCompleteWhenShutdownCancelsOwnerTask) {
    optionx::platforms::IntradeBarPlatform platform;
    platform.run(false);

    int history_callbacks = 0;
    int result_callbacks = 0;
    int bar_callbacks = 0;
    int tick_callbacks = 0;
    optionx::TradeHistoryResult history_result;
    std::unique_ptr<optionx::TradeResult> trade_result;
    optionx::BarHistoryResult bar_result;
    optionx::TickHistoryResult tick_result;

    optionx::TradeHistoryRequest history_request =
        optionx::TradeHistoryRequest::all();
    ASSERT_TRUE(platform.fetch_trade_history(
        history_request,
        [&history_callbacks, &history_result](optionx::TradeHistoryResult result) {
            ++history_callbacks;
            history_result = std::move(result);
        }));

    auto result = std::make_unique<optionx::TradeResult>();
    result->trade_id = 77;
    optionx::TradeResultQuery query;
    query.trade_id = result->trade_id;
    ASSERT_TRUE(platform.fetch_trade_result(
        query,
        std::move(result),
        [&result_callbacks, &trade_result](std::unique_ptr<optionx::TradeResult> completed) {
            ++result_callbacks;
            trade_result = std::move(completed);
        }));

    optionx::BarHistoryRequest bar_request;
    bar_request.symbol = "EURUSD";
    bar_request.timeframe = 60;
    bar_request.from_ts = 1;
    bar_request.to_ts = 60;
    ASSERT_TRUE(platform.fetch_bar_history(
        bar_request,
        [&bar_callbacks, &bar_result](optionx::BarHistoryResult result) {
            ++bar_callbacks;
            bar_result = std::move(result);
        }));

    const optionx::TickHistoryRequest tick_request(
        "EURUSD",
        1000,
        2000,
        optionx::TickHistorySource::BROKER_HISTORICAL);
    ASSERT_TRUE(platform.fetch_tick_history(
        tick_request,
        [&tick_callbacks, &tick_result](optionx::TickHistoryResult result) {
            ++tick_callbacks;
            tick_result = std::move(result);
        }));

    platform.shutdown();

    EXPECT_EQ(history_callbacks, 1);
    EXPECT_FALSE(history_result.success);
    EXPECT_NE(
        history_result.error_desc.find("cancelled during platform shutdown"),
        std::string::npos);

    EXPECT_EQ(result_callbacks, 1);
    ASSERT_TRUE(trade_result);
    EXPECT_EQ(trade_result->trade_id, 77u);
    EXPECT_EQ(trade_result->trade_state, optionx::TradeState::CHECK_ERROR);
    EXPECT_EQ(
        trade_result->error_code,
        optionx::TradeErrorCode::CLIENT_FORCED_CLOSE);

    EXPECT_EQ(bar_callbacks, 1);
    EXPECT_FALSE(bar_result.success);
    EXPECT_NE(
        bar_result.error_desc.find("cancelled during platform shutdown"),
        std::string::npos);

    EXPECT_EQ(tick_callbacks, 1);
    EXPECT_FALSE(tick_result.success);
    EXPECT_NE(
        tick_result.error_desc.find("cancelled during platform shutdown"),
        std::string::npos);
}

TEST(IntradeBarPublicRequests, DefaultTickHistorySourceRemainsLocalObserved) {
    optionx::platforms::IntradeBarPlatform platform;
    optionx::TickHistoryResult result;
    int callback_count = 0;

    ASSERT_TRUE(platform.fetch_tick_history(
        optionx::TickHistoryRequest("EURUSD", 1000, 2000),
        [&result, &callback_count](optionx::TickHistoryResult update) {
            result = std::move(update);
            ++callback_count;
        }));

    EXPECT_EQ(callback_count, 1);
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.range_complete);
    EXPECT_TRUE(result.sequence.ticks.empty());
    EXPECT_EQ(result.sequence.source, "LOCAL_OBSERVED");
    platform.shutdown();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
