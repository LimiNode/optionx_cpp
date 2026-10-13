#include <gtest/gtest.h>

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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
