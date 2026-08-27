#include <gtest/gtest.h>

#include <optionx_cpp/bridges/trading_view.hpp>

#include <asio.hpp>
#include <client_http.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using optionx::bridges::tradingview::TradingViewExtensionBridge;
using optionx::bridges::tradingview::TradingViewExtensionBridgeConfig;
using HttpClient = SimpleWeb::Client<SimpleWeb::HTTP>;

TradingViewExtensionBridgeConfig test_config() {
    TradingViewExtensionBridgeConfig config;
    config.address = "127.0.0.1";
    config.port = 0;
    config.bridge_id = 77;
    config.secret = "test-secret";
    config.fixed_amount = 1.0;
    config.duration = 60;
    config.symbol_map["FX:EURUSD"] = "EURUSD";
    return config;
}

nlohmann::json test_payload(const std::string& event_id) {
    return nlohmann::json{
        {"source", "tradingview"},
        {"signal_name", "bridge-audit"},
        {"action", "buy"},
        {"symbol", "FX:EURUSD"},
        {"tickerid", "FX:EURUSD"},
        {"price", 1.14055},
        {"time", 1783476660000LL},
        {"event_id", event_id}
    };
}

bool wait_until(
        const std::function<bool()>& predicate,
        const std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

unsigned short reserve_port() {
    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor(io_context);
    acceptor.open(asio::ip::tcp::v4());
    acceptor.set_option(asio::socket_base::reuse_address(false));
    acceptor.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = acceptor.local_endpoint().port();
    acceptor.close();
    return port;
}

nlohmann::json post_signal(
        const TradingViewExtensionBridgeConfig& config,
        const unsigned short port,
        const nlohmann::json& payload) {
    HttpClient client(config.address + ":" + std::to_string(port));
    client.config.timeout = 3;
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-OptionX-Secret", config.secret);
    const auto response = client.request(
        "POST",
        config.signal_path,
        payload.dump(),
        headers);
    return nlohmann::json::parse(response->content.string());
}

} // namespace

TEST(TradingViewExtensionBridge, BindFailureFinalizesLifecycleAndAllowsRestart) {
    asio::io_context io_context;
    asio::ip::tcp::acceptor occupied_port(io_context);
    occupied_port.open(asio::ip::tcp::v4());
    occupied_port.set_option(asio::socket_base::reuse_address(false));
    occupied_port.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    occupied_port.listen();

    auto config = test_config();
    config.port = occupied_port.local_endpoint().port();

    TradingViewExtensionBridge bridge;
    ASSERT_TRUE(bridge.configure(
        std::make_unique<TradingViewExtensionBridgeConfig>(config)));

    std::atomic<optionx::SignalId> next_signal_id{1};
    std::atomic<bool> start_failed{false};
    bridge.on_signal_id() = [&]() { return next_signal_id.fetch_add(1); };
    bridge.on_trade_signal() = [](std::unique_ptr<optionx::TradeSignal>) {};
    bridge.on_status_update() = [&](const optionx::BridgeStatusUpdate& update) {
        if (update.status == optionx::BridgeStatus::SERVER_START_FAILED) {
            start_failed.store(true);
        }
    };

    bridge.run();
    ASSERT_TRUE(wait_until([&]() { return start_failed.load(); }));

    occupied_port.close();
    config.port = 0;
    ASSERT_TRUE(bridge.configure(
        std::make_unique<TradingViewExtensionBridgeConfig>(config)));
    bridge.run();
    ASSERT_TRUE(wait_until([&]() { return bridge.bound_port() != 0; }));
    bridge.shutdown();
}

TEST(TradingViewExtensionBridge, ShutdownFromServerStartedAllowsSamePortRestart) {
    auto config = test_config();
    config.port = reserve_port();

    TradingViewExtensionBridge bridge;
    ASSERT_TRUE(bridge.configure(
        std::make_unique<TradingViewExtensionBridgeConfig>(config)));

    std::atomic<optionx::SignalId> next_signal_id{1};
    std::atomic<int> started_count{0};
    std::atomic<int> stopped_count{0};
    bridge.on_signal_id() = [&]() { return next_signal_id.fetch_add(1); };
    bridge.on_trade_signal() = [](std::unique_ptr<optionx::TradeSignal>) {};
    bridge.on_status_update() = [&](const optionx::BridgeStatusUpdate& update) {
        if (update.status == optionx::BridgeStatus::SERVER_STARTED &&
            started_count.fetch_add(1) == 0) {
            bridge.shutdown();
        }
        if (update.status == optionx::BridgeStatus::SERVER_STOPPED) {
            stopped_count.fetch_add(1);
        }
    };

    bridge.run();
    ASSERT_TRUE(wait_until([&]() { return stopped_count.load() == 1; }));

    bridge.run();
    ASSERT_TRUE(wait_until([&]() {
        return started_count.load() >= 2 && bridge.bound_port() == config.port;
    }));
    bridge.shutdown();
    EXPECT_TRUE(wait_until([&]() { return stopped_count.load() == 2; }));
}

TEST(TradingViewExtensionBridge, CallbackShutdownPreservesResponseAndAllowsRestart) {
    auto config = test_config();
    TradingViewExtensionBridge bridge;
    ASSERT_TRUE(bridge.configure(
        std::make_unique<TradingViewExtensionBridgeConfig>(config)));

    std::atomic<optionx::SignalId> next_signal_id{1};
    std::atomic<int> signal_count{0};
    std::atomic<int> stopped_count{0};
    bridge.on_signal_id() = [&]() { return next_signal_id.fetch_add(1); };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal>) {
        signal_count.fetch_add(1);
        bridge.shutdown();
    };
    bridge.on_status_update() = [&](const optionx::BridgeStatusUpdate& update) {
        if (update.status == optionx::BridgeStatus::SERVER_STOPPED) {
            stopped_count.fetch_add(1);
        }
    };

    bridge.run();
    ASSERT_TRUE(wait_until([&]() { return bridge.bound_port() != 0; }));
    const auto first_port = bridge.bound_port();
    const auto response = post_signal(config, first_port, test_payload("callback-stop"));

    EXPECT_TRUE(response.at("accepted").get<bool>());
    EXPECT_EQ(signal_count.load(), 1);
    ASSERT_TRUE(wait_until([&]() { return stopped_count.load() == 1; }));

    bridge.on_trade_signal() = [](std::unique_ptr<optionx::TradeSignal>) {};
    bridge.run();
    ASSERT_TRUE(wait_until([&]() { return bridge.bound_port() != 0; }));
    bridge.shutdown();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
