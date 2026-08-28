#include <gtest/gtest.h>

#include <optionx_cpp/bridges/trading_view.hpp>
#include <optionx_cpp/utils/json_comments.hpp>

#include <client_http.hpp>

#include <chrono>
#include <limits>
#include <mutex>
#include <thread>

namespace {

using optionx::bridges::tradingview::TradingViewExtensionBridgeConfig;
using optionx::bridges::tradingview::TradingViewLevelAlertRule;
using HttpClient = SimpleWeb::Client<SimpleWeb::HTTP>;
namespace tv_protocol = optionx::bridges::tradingview::detail;

TradingViewExtensionBridgeConfig base_config() {
    TradingViewExtensionBridgeConfig config;
    config.bridge_id = 77;
    config.secret = "test-secret";
    config.fixed_amount = 12.5;
    config.duration = 120;
    config.min_payout = 0.65;
    config.symbol_map["FX:EURUSD"] = "EURUSD";
    return config;
}

nlohmann::json price_alert_payload(
        const std::string& method,
        nlohmann::json payload) {
    return nlohmann::json{
        {"text", {
            {"channel", "pricealerts"},
            {"content", {
                {"m", method},
                {"p", std::move(payload)}
            }}
        }}
    };
}

bool wait_for_port(const optionx::bridges::tradingview::TradingViewExtensionBridge& bridge) {
    for (int i = 0; i < 200; ++i) {
        if (bridge.bound_port() != 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

struct BridgeGuard {
    optionx::bridges::tradingview::TradingViewExtensionBridge& bridge;
    ~BridgeGuard() { bridge.shutdown(); }
};

} // namespace

TEST(JsonComments, StripsCommentsOutsideStrings) {
    const std::string text = R"json(
        {
          // endpoint comment
          "url": "http://127.0.0.1:6560/api//v1#fragment",
          "value": 7, /* block comment */
          # hash comment
          "hash": "value # not a comment"
        }
    )json";

    const auto parsed = optionx::utils::parse_json_with_comments(text);

    EXPECT_EQ(parsed.at("url"), "http://127.0.0.1:6560/api//v1#fragment");
    EXPECT_EQ(parsed.at("value"), 7);
    EXPECT_EQ(parsed.at("hash"), "value # not a comment");
}

TEST(TradingViewExtensionProtocol, ConfigRoundTripsJson) {
    TradingViewExtensionBridgeConfig config;
    config.address = "127.0.0.1";
    config.port = 0;
    config.signal_path = "/tv/signal";
    config.bridge_id = 9;
    config.secret = "shared";
    config.allowed_origin = "chrome-extension://abc123";
    config.allow_body_secret_fallback = false;
    config.sizing_mode = "balance_percent";
    config.balance_percent = 2.5;
    config.min_amount = 1.0;
    config.max_amount = 10.0;
    config.duration = 90;
    config.symbol_map["FX:EURUSD"] = "EURUSD";
    config.use_default_action_keywords = false;
    config.buy_action_keywords = {"entry-long"};
    config.sell_action_keywords = {"entry-short"};
    config.study_alert_mode = "confirmed_only";
    config.study_alert_close_window_seconds = 7;
    config.study_alert_default_timeframe_seconds = 300;
    config.study_alert_max_signal_age_seconds = 30;
    config.study_alert_reject_historical = true;
    config.level_alert_rules.push_back(
        TradingViewLevelAlertRule{
            "123",
            "FX:EURUSD",
            "crossing_down",
            {},
            {},
            "sell",
            "eurusd_level_sell"});

    nlohmann::json json;
    config.to_json(json);

    TradingViewExtensionBridgeConfig restored;
    restored.from_json(json);

    EXPECT_EQ(restored.bridge_type(), optionx::BridgeType::TRADING_VIEW_EXTENSION_HTTP);
    EXPECT_EQ(restored.port, 0);
    EXPECT_EQ(restored.signal_path, "/tv/signal");
    EXPECT_FALSE(json.contains("health_path"));
    EXPECT_EQ(restored.allowed_origin, "chrome-extension://abc123");
    EXPECT_FALSE(restored.allow_body_secret_fallback);
    EXPECT_EQ(restored.sizing_mode, "balance_percent");
    EXPECT_DOUBLE_EQ(restored.balance_percent, 2.5);
    EXPECT_FALSE(restored.use_default_action_keywords);
    ASSERT_EQ(restored.buy_action_keywords.size(), 1u);
    EXPECT_EQ(restored.buy_action_keywords.front(), "entry-long");
    ASSERT_EQ(restored.sell_action_keywords.size(), 1u);
    EXPECT_EQ(restored.sell_action_keywords.front(), "entry-short");
    EXPECT_EQ(restored.study_alert_mode, "confirmed_only");
    EXPECT_EQ(restored.study_alert_close_window_seconds, 7);
    EXPECT_EQ(restored.study_alert_default_timeframe_seconds, 300);
    EXPECT_EQ(restored.study_alert_max_signal_age_seconds, 30);
    EXPECT_TRUE(restored.study_alert_reject_historical);
    EXPECT_EQ(restored.level_alert_rules.size(), 1u);
    EXPECT_EQ(restored.level_alert_rules.front().action, "sell");
    EXPECT_TRUE(restored.validate().first);

    restored.bridge_id = 0;
    EXPECT_FALSE(restored.validate().first);
}

TEST(TradingViewExtensionProtocol, ConfigRejectsNonExecutableBusinessValues) {
    auto config = base_config();
    config.fixed_amount = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(config.validate().first);

    config = base_config();
    config.fixed_amount = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(config.validate().first);

    config = base_config();
    config.fixed_amount = 0.0;
    EXPECT_FALSE(config.validate().first);

    config = base_config();
    config.sizing_mode = "none";
    config.fixed_amount = 0.0;
    EXPECT_TRUE(config.validate().first);

    config = base_config();
    config.duration = 0;
    EXPECT_FALSE(config.validate().first);

    config = base_config();
    config.option_type = optionx::OptionType::UNKNOWN;
    EXPECT_FALSE(config.validate().first);

    config = base_config();
    config.min_payout = 1.01;
    EXPECT_FALSE(config.validate().first);

    config = base_config();
    config.min_payout = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(config.validate().first);
}

TEST(TradingViewExtensionProtocol, ConstantTimeSecretCompareMatchesEquality) {
    EXPECT_TRUE(tv_protocol::protocol::constant_time_equals("secret", "secret"));
    EXPECT_FALSE(tv_protocol::protocol::constant_time_equals("secret", "Secret"));
    EXPECT_FALSE(tv_protocol::protocol::constant_time_equals("secret", "secret1"));
    EXPECT_FALSE(tv_protocol::protocol::constant_time_equals("secret1", "secret"));
}

TEST(TradingViewExtensionProtocol, RejectsInvalidConfiguredTradeSignalBeforeAccepted) {
    auto config = base_config();
    config.fixed_amount = 0.0;

    const nlohmann::json payload = {
        {"source", "tradingview"},
        {"action", "buy"},
        {"symbol", "FX:EURUSD"},
        {"event_id", "indicator:eurusd:zero-amount:buy"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.signal);
    EXPECT_EQ(result.reason, "invalid_trade_signal");
    EXPECT_FALSE(result.response.at("accepted").get<bool>());
    EXPECT_NE(
        result.response.at("message").get<std::string>().find("amount must be positive"),
        std::string::npos);
}

TEST(TradingViewExtensionProtocol, AcceptsClassicConfiguredWithRelativeDuration) {
    auto config = base_config();
    config.option_type = optionx::OptionType::CLASSIC;
    config.duration = 900;

    const nlohmann::json payload = {
        {"source", "tradingview"},
        {"action", "sell"},
        {"symbol", "FX:EURUSD"},
        {"event_id", "indicator:eurusd:classic-duration:sell"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->option_type, optionx::OptionType::CLASSIC);
    EXPECT_EQ(result.signal->duration, 900u);
    EXPECT_EQ(result.signal->expiry_time, 0);
}

TEST(TradingViewExtensionProtocol, RejectsWhitespaceOnlyMappedSymbol) {
    auto config = base_config();
    config.symbol_map["FX:EURUSD"] = " \r\n ";

    const nlohmann::json payload = {
        {"source", "tradingview"},
        {"action", "buy"},
        {"symbol", "FX:EURUSD"},
        {"event_id", "indicator:eurusd:blank-map:buy"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.signal);
    EXPECT_EQ(result.reason, "missing_symbol");
}

TEST(BridgeTradeSignalValidation, EnforcesExecutableSignalBoundary) {
    optionx::TradeSignal signal;
    signal.symbol = "EURUSD";
    signal.order_type = optionx::OrderType::BUY;
    signal.option_type = optionx::OptionType::SPRINT;
    signal.amount = 1.0;
    signal.duration = 60;

    EXPECT_NO_THROW(optionx::bridges::detail::validate_executable_trade_signal(signal));

    signal.symbol = " \t ";
    EXPECT_THROW(
        optionx::bridges::detail::validate_executable_trade_signal(signal),
        std::invalid_argument);
    signal.symbol = "EURUSD";

    signal.amount = -1.0;
    EXPECT_THROW(
        optionx::bridges::detail::validate_executable_trade_signal(signal, "test", false),
        std::invalid_argument);
    signal.amount = 0.0;
    EXPECT_NO_THROW(optionx::bridges::detail::validate_executable_trade_signal(signal, "test", false));
    EXPECT_THROW(
        optionx::bridges::detail::validate_executable_trade_signal(signal, "test", true),
        std::invalid_argument);
    signal.amount = 1.0;

    signal.option_type = optionx::OptionType::CLASSIC;
    signal.duration = 900;
    signal.expiry_time = 0;
    EXPECT_NO_THROW(optionx::bridges::detail::validate_executable_trade_signal(signal));

    signal.duration = 0;
    signal.expiry_time = 1900000000;
    EXPECT_NO_THROW(optionx::bridges::detail::validate_executable_trade_signal(signal));

    signal.duration = 900;
    EXPECT_THROW(
        optionx::bridges::detail::validate_executable_trade_signal(signal),
        std::invalid_argument);

    signal.duration = 0;
    signal.expiry_time = 0;
    EXPECT_THROW(
        optionx::bridges::detail::validate_executable_trade_signal(signal),
        std::invalid_argument);
}

TEST(TradingViewExtensionProtocol, ParsesIndicatorBuySignal) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"source", "tradingview"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "buy"},
        {"symbol", "FX:EURUSD"},
        {"tickerid", "FX:EURUSD"},
        {"price", 1.14055},
        {"time", 1783476660000LL},
        {"event_id", "indicator:eurusd:1783476660000:buy"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->bridge_id, 77u);
    EXPECT_EQ(result.signal->symbol, "EURUSD");
    EXPECT_EQ(result.signal->signal_name, "noisy_rsi_test");
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
    EXPECT_EQ(result.signal->option_type, optionx::OptionType::SPRINT);
    EXPECT_DOUBLE_EQ(result.signal->amount, 12.5);
    EXPECT_EQ(result.signal->mm_type, optionx::MmSystemType::FIXED);
    EXPECT_EQ(result.signal->duration, 120u);
    EXPECT_EQ(result.signal->min_payout, 0.65);
    EXPECT_NE(result.signal->user_data.find("\"source_kind\":\"tradingview\""), std::string::npos);
}

TEST(TradingViewExtensionProtocol, ParsesChartStudyIndicatorSignal) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"version", 1},
        {"source", "tradingview_extension"},
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"event_id", "tv_study_alert:abc123"},
        {"dedupe_key", "tv_study_alert:abc123"},
        {"fingerprint", "abc123"},
        {"chart_session", "cs_test"},
        {"study_id", "8x94yO"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "sell"},
        {"symbol", "BTCUSD"},
        {"tickerid", "CRYPTO:BTCUSD"},
        {"price", 64131.92},
        {"time", 1783763820000LL},
        {"bar_time", 1783763820000LL},
        {"update_time", 1783763881395LL},
        {"bar_state", "RT_CONFIRMED"},
        {"bar_state_source", "debug_idx"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.event_id, "tv_study_alert:abc123");
    EXPECT_EQ(result.signal->unique_hash, "tv_study_alert:abc123");
    EXPECT_EQ(result.signal->symbol, "BTCUSD");
    EXPECT_EQ(result.signal->signal_name, "noisy_rsi_test");
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::SELL);
    EXPECT_EQ(result.parsed_payload.at("bar_state"), "RT_CONFIRMED");
    EXPECT_EQ(result.parsed_payload.at("bar_state_source"), "debug_idx");
    EXPECT_NE(
        result.signal->user_data.find("\"source_kind\":\"private_chart_study_alert_messages\""),
        std::string::npos);
    EXPECT_NE(
        result.signal->user_data.find("\"bar_state\":\"RT_CONFIRMED\""),
        std::string::npos);
}

TEST(TradingViewExtensionProtocol, RejectsUnconfirmedRealtimeStudyAlertWhenConfirmedOnly) {
    auto config = base_config();
    config.study_alert_mode = "confirmed_only";

    const nlohmann::json payload = {
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"event_id", "tv_study_alert:rt"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "buy"},
        {"symbol", "BTCUSD"},
        {"price", 64130.49},
        {"time", 1783764000000LL},
        {"bar_state", "RT"},
        {"bar_state_source", "debug_idx"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, "unconfirmed_study_alert");
    EXPECT_FALSE(result.signal);
    ASSERT_TRUE(result.parsed_payload.is_object());
    EXPECT_EQ(result.parsed_payload.at("bar_state"), "RT");
}

TEST(TradingViewExtensionProtocol, RejectsHistoricLastStudyAlertWhenConfirmedOnly) {
    auto config = base_config();
    config.study_alert_mode = "confirmed_only";

    const nlohmann::json payload = {
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"event_id", "tv_study_alert:hist_last"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "buy"},
        {"symbol", "BTCUSD"},
        {"price", 64130.49},
        {"time", 1783764000000LL},
        {"bar_state", "HIST_LAST"},
        {"bar_state_source", "debug_idx"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, "unconfirmed_study_alert");
    EXPECT_FALSE(result.signal);
    ASSERT_TRUE(result.parsed_payload.is_object());
    EXPECT_EQ(result.parsed_payload.at("bar_state"), "HIST_LAST");
}

TEST(TradingViewExtensionProtocol, AcceptsRealtimeConfirmedStudyAlertWhenConfirmedOnly) {
    auto config = base_config();
    config.study_alert_mode = "confirmed_only";

    const nlohmann::json payload = {
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"event_id", "tv_study_alert:rt_confirmed"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "sell"},
        {"symbol", "BTCUSD"},
        {"price", 64131.92},
        {"time", 1783763820000LL},
        {"bar_state", "RT_CONFIRMED"},
        {"bar_state_source", "debug_idx"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::SELL);
}

TEST(TradingViewExtensionProtocol, AcceptsHistoricStudyAlertWhenConfirmedOnly) {
    auto config = base_config();
    config.study_alert_mode = "confirmed_only";

    const nlohmann::json payload = {
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"event_id", "tv_study_alert:hist"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "buy"},
        {"symbol", "BTCUSD"},
        {"price", 64131.92},
        {"time", 1783763820000LL},
        {"bar_state", "HIST_CONFIRMED"},
        {"bar_state_source", "debug_last"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
}

TEST(TradingViewExtensionProtocol, AcceptsLateIntrabarStudyAlertInCloseWindow) {
    auto config = base_config();
    config.study_alert_mode = "close_window";
    config.study_alert_close_window_seconds = 10;
    config.study_alert_default_timeframe_seconds = 60;

    const auto bar_time = 1783763820000LL;
    const nlohmann::json payload = {
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"event_id", "tv_study_alert:late_window"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "buy"},
        {"symbol", "BTCUSD"},
        {"price", 64131.92},
        {"time", bar_time},
        {"bar_time", bar_time},
        {"update_time", bar_time + 55000LL}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
}

TEST(TradingViewExtensionProtocol, RejectsEarlyIntrabarStudyAlertOutsideCloseWindow) {
    auto config = base_config();
    config.study_alert_mode = "close_window";
    config.study_alert_close_window_seconds = 10;
    config.study_alert_default_timeframe_seconds = 60;

    const auto bar_time = 1783763820000LL;
    const nlohmann::json payload = {
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"event_id", "tv_study_alert:early_window"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "sell"},
        {"symbol", "BTCUSD"},
        {"price", 64131.92},
        {"time", bar_time},
        {"bar_time", bar_time},
        {"update_time", bar_time + 49000LL}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, "outside_close_window");
    EXPECT_FALSE(result.signal);
}

TEST(TradingViewExtensionProtocol, UsesExtensionIntervalForCloseWindow) {
    auto config = base_config();
    config.study_alert_mode = "close_window";
    config.study_alert_close_window_seconds = 10;
    config.study_alert_default_timeframe_seconds = 300;

    const auto bar_time = 1783763820000LL;
    const nlohmann::json payload = {
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"event_id", "tv_study_alert:extension_interval"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "buy"},
        {"symbol", "BTCUSD"},
        {"price", 64131.92},
        {"time", bar_time},
        {"bar_time", bar_time},
        {"update_time", bar_time + 55000LL},
        {"extension", {
            {"interval", "1"}
        }}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.parsed_payload.at("timeframe"), "1");
}

TEST(TradingViewExtensionProtocol, RejectsStaleStudyAlertWhenEnabled) {
    auto config = base_config();
    config.study_alert_reject_historical = true;
    config.study_alert_max_signal_age_seconds = 10;

    const auto now = tv_protocol::protocol::current_time_ms();
    const nlohmann::json payload = {
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"event_id", "tv_study_alert:stale"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "buy"},
        {"symbol", "BTCUSD"},
        {"price", 64131.92},
        {"time", now - 60000LL},
        {"bar_time", now - 60000LL},
        {"update_time", now - 60000LL}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, "stale_study_alert");
    EXPECT_FALSE(result.signal);
}

TEST(TradingViewExtensionProtocol, RejectsLifecycleCancelStudyAlert) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"event_id", "tv_study_alert:lifecycle_cancel"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "buy"},
        {"state", "cancel"},
        {"signal_id", "BTCUSD|1|1783763820000|buy"},
        {"revision", 2},
        {"symbol", "BTCUSD"},
        {"price", 64131.92},
        {"time", 1783763820000LL}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, "signal_lifecycle_cancel");
    EXPECT_FALSE(result.signal);
    EXPECT_EQ(result.parsed_payload.at("signal_id"), "BTCUSD|1|1783763820000|buy");
    EXPECT_EQ(result.parsed_payload.at("revision"), 2);
}

TEST(TradingViewExtensionProtocol, AcceptsLifecycleConfirmedStudyAlertWhenConfirmedOnly) {
    auto config = base_config();
    config.study_alert_mode = "confirmed_only";

    const nlohmann::json payload = {
        {"source_kind", "private_chart_study_alert_messages"},
        {"method", "du.alertMessages"},
        {"signal_name", "noisy_rsi_test"},
        {"action", "sell"},
        {"state", "confirmed"},
        {"signal_id", "BTCUSD|1|1783763820000|sell"},
        {"revision", 3},
        {"symbol", "BTCUSD"},
        {"price", 64131.92},
        {"time", 1783763820000LL}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.event_id, "BTCUSD|1|1783763820000|sell:confirmed:3");
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::SELL);
    EXPECT_EQ(result.parsed_payload.at("signal_state"), "confirmed");
}

TEST(TradingViewExtensionProtocol, RejectsBodySecretByDefault) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"secret", "test-secret"},
        {"action", "buy"},
        {"symbol", "FX:EURUSD"}
    };

    auto result = tv_protocol::parse_extension_payload(payload, config);

    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.authorized);
    EXPECT_EQ(result.reason, "invalid_secret");
    EXPECT_FALSE(result.signal);
}

TEST(TradingViewExtensionProtocol, AcceptsBodySecretOnlyWhenFallbackIsEnabled) {
    auto config = base_config();
    config.allow_body_secret_fallback = true;

    const nlohmann::json payload = {
        {"secret", "test-secret"},
        {"action", "buy"},
        {"symbol", "FX:EURUSD"}
    };

    auto result = tv_protocol::parse_extension_payload(payload, config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
    ASSERT_TRUE(result.raw_payload.is_object());
    EXPECT_EQ(result.raw_payload.at("secret"), "[redacted]");
}

TEST(TradingViewExtensionProtocol, AppliesAlertIdRuleForLevelAlert) {
    auto config = base_config();
    config.level_alert_rules.push_back(
        TradingViewLevelAlertRule{
            "53256556946",
            {},
            {},
            {},
            {},
            "sell",
            "eurusd_level_sell"});

    auto result = tv_protocol::parse_extension_payload(
        price_alert_payload(
            "alert_fired",
            nlohmann::json{
                {"fire_id", 53256556946LL},
                {"symbol", "FX:EURUSD"},
                {"message", "EURUSD Crossing 1.14072"},
                {"price", 1.14072},
                {"time", 1783476705177LL}
            }),
        "test-secret",
        config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->symbol, "EURUSD");
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::SELL);
    EXPECT_EQ(result.signal->signal_name, "eurusd_level_sell");
    EXPECT_EQ(result.signal->unique_id, 53256556946LL);
    EXPECT_EQ(result.reason, "accepted");
    EXPECT_EQ(result.parsed_payload.at("source_kind"), "private_pricealerts_ws");
}

TEST(TradingViewExtensionProtocol, AppliesConditionRuleForLevelAlert) {
    auto config = base_config();
    config.level_alert_rules.push_back(
        TradingViewLevelAlertRule{
            {},
            "FX:EURUSD",
            "crossing_up",
            {},
            {},
            "buy",
            "eurusd_crossing_up"});

    auto result = tv_protocol::parse_extension_payload(
        price_alert_payload(
            "alert_fired",
            nlohmann::json{
                {"fire_id", 53256556947LL},
                {"symbol", "FX:EURUSD"},
                {"message", "EURUSD Crossing Up 1.14072"}
            }),
        "test-secret",
        config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
    EXPECT_EQ(result.signal->signal_name, "eurusd_crossing_up");
    EXPECT_DOUBLE_EQ(result.parsed_payload.at("price").get<double>(), 1.14072);
}

TEST(TradingViewExtensionProtocol, ParsesForwardedPrivateFeedPayload) {
    auto config = base_config();
    config.level_alert_rules.push_back(
        TradingViewLevelAlertRule{
            {},
            "FX:EURUSD",
            "crossing",
            {},
            {},
            "buy",
            "private_pricealert"});

    auto payload = price_alert_payload(
        "alert_fired",
        nlohmann::json{
            {"fire_id", 53256556946LL},
            {"alert_id", 5099741779LL},
            {"symbol", "={\"symbol\":\"FX:EURUSD\",\"adjustment\":\"splits\"}"},
            {"message", "EURUSD Crossing 1.14072"},
            {"fire_time", "2026-07-08T02:36:22Z"}
        });
    payload["source_kind"] = "private_pricealerts_ws";
    payload["event_id"] = "tv_price_alert:53256556946";
    payload["dedupe_key"] = "tv_price_alert:53256556946";
    payload["fingerprint"] = "b9d62011";

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.event_id, "tv_price_alert:53256556946");
    EXPECT_EQ(result.dedupe_key, "tv_price_alert:53256556946");
    EXPECT_EQ(result.signal->unique_hash, "tv_price_alert:53256556946");
    EXPECT_EQ(result.signal->symbol, "EURUSD");
    EXPECT_EQ(result.signal->signal_name, "private_pricealert");
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
    EXPECT_EQ(result.parsed_payload.at("source_kind"), "private_pricealerts_ws");
    EXPECT_DOUBLE_EQ(result.parsed_payload.at("price").get<double>(), 1.14072);
}

TEST(TradingViewExtensionProtocol, RejectsUnmappedLevelAlert) {
    auto config = base_config();

    auto result = tv_protocol::parse_extension_payload(
        price_alert_payload(
            "alert_fired",
            nlohmann::json{
                {"fire_id", 53256556948LL},
                {"symbol", "FX:EURUSD"},
                {"message", "EURUSD Crossing 1.14072"}
            }),
        "test-secret",
        config);

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, "unmapped_level_alert");
    EXPECT_FALSE(result.signal);
    ASSERT_TRUE(result.parsed_payload.is_object());
    EXPECT_EQ(result.parsed_payload.at("symbol"), "EURUSD");
    EXPECT_EQ(result.parsed_payload.at("condition_type"), "crossing");
}

TEST(TradingViewExtensionProtocol, MapsLevelAlertActionFromDefaultKeyword) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "toast:btcusd:buy"},
        {"action", "alert"},
        {"symbol", "BTCUSD"},
        {"message", "BTCUSD Crossing BUY 64,114.82"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->symbol, "BTCUSD");
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
    EXPECT_EQ(result.signal->signal_name, "tradingview_level_alert");
}

TEST(TradingViewExtensionProtocol, DoesNotMapDirectionalLevelWordsByDefault) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "toast:eurusd:crossing-up"},
        {"action", "alert"},
        {"symbol", "EURUSD"},
        {"message", "EURUSD Crossing Up 1.1400"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, "unmapped_level_alert");
    EXPECT_FALSE(result.signal);
}

TEST(TradingViewExtensionProtocol, MapsToastDirectionToConditionRule) {
    auto config = base_config();
    config.level_alert_rules.push_back(
        TradingViewLevelAlertRule{
            {},
            "EURUSD",
            "crossing_up",
            {},
            {},
            "buy",
            "eurusd_crossing_up"});

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "toast:eurusd:direction-up"},
        {"action", "alert"},
        {"symbol", "EURUSD"},
        {"direction", "up"},
        {"message", "EURUSD Crossing Up 1.1400"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.parsed_payload.at("condition_type"), "crossing_up");
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
}

TEST(TradingViewExtensionProtocol, MatchesDefaultRussianKeywordsCaseInsensitively) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "toast:btcusd:russian-buy"},
        {"action", "alert"},
        {"symbol", "BTCUSD"},
        {"message", u8"BTCUSD Crossing \u0411\u0410\u0419 64,143.35"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
}

TEST(TradingViewExtensionProtocol, MatchesCustomKeywordsWithUnicodeCaseFolding) {
    auto config = base_config();
    config.use_default_action_keywords = false;
    config.buy_action_keywords = {"strasse"};

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "toast:btcusd:german-fold"},
        {"action", "alert"},
        {"symbol", "BTCUSD"},
        {"message", u8"BTCUSD Crossing Stra\u00DFe 64,143.35"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
}

TEST(TradingViewExtensionProtocol, ParsesCommaPriceAndIsoTimeFromToastPayload) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "tv_toast:d8b7589f"},
        {"fingerprint", "d8b7589f"},
        {"action", "alert"},
        {"symbol", "BTCUSD"},
        {"message", "BTCUSD Crossing BUY 64,143.35"},
        {"price", "64,143.35"},
        {"time", "2026-07-11T08:09:45.650Z"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
    EXPECT_DOUBLE_EQ(result.parsed_payload.at("price").get<double>(), 64143.35);
    EXPECT_EQ(result.parsed_payload.at("time").get<std::int64_t>(), 1783757385650LL);

    const auto user_data = nlohmann::json::parse(result.signal->user_data);
    EXPECT_DOUBLE_EQ(user_data.at("price").get<double>(), 64143.35);
    EXPECT_EQ(user_data.at("time").get<std::int64_t>(), 1783757385650LL);
}

TEST(TradingViewExtensionProtocol, KeepsPercentTriggerOutOfPriceFallback) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "toast:eurusd:moving-up-pct"},
        {"action", "alert"},
        {"symbol", "EURUSD"},
        {"message", "EURUSD Moving Up 1.0%"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, "unmapped_level_alert");
    ASSERT_TRUE(result.parsed_payload.is_object());
    EXPECT_EQ(result.parsed_payload.at("condition_type"), "moving_up_pct");
    EXPECT_DOUBLE_EQ(result.parsed_payload.at("price").get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(result.parsed_payload.at("trigger_value").get<double>(), 1.0);
    EXPECT_EQ(result.parsed_payload.at("trigger_unit"), "percent");
}

TEST(TradingViewExtensionProtocol, PreservesAlertNameAndUsesItForKeywords) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "toast:btcusd:title-buy"},
        {"action", "alert"},
        {"symbol", "BTCUSD"},
        {"alert_name", "BUY Test99"},
        {"message", "BTCUSD Crossing 64,119.59"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
    EXPECT_EQ(result.signal->signal_name, "BUY Test99");
    EXPECT_EQ(result.signal->comment, "BTCUSD Crossing 64,119.59");
    EXPECT_NE(result.signal->user_data.find("\"alert_name\":\"BUY Test99\""), std::string::npos);
}

TEST(TradingViewExtensionProtocol, ExtendsActionKeywordsWithCustomRussianTerm) {
    auto config = base_config();
    config.use_default_action_keywords = false;
    config.buy_action_keywords = {u8"\u043F\u043E\u043A\u0443\u043F\u0430"};

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "toast:btcusd:custom-buy"},
        {"action", "alert"},
        {"symbol", "BTCUSD"},
        {"message", u8"BTCUSD Crossing \u041F\u043E\u043A\u0443\u043F\u0430 64,114.82"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.signal->order_type, optionx::OrderType::BUY);
}

TEST(TradingViewExtensionProtocol, CanDisableDefaultActionKeywords) {
    auto config = base_config();
    config.use_default_action_keywords = false;

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "toast:btcusd:disabled-keyword"},
        {"action", "alert"},
        {"symbol", "BTCUSD"},
        {"message", "BTCUSD Crossing BUY 64,114.82"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, "unmapped_level_alert");
    EXPECT_FALSE(result.signal);
}

TEST(TradingViewExtensionProtocol, IgnoresPriceAlertLifecycleMessages) {
    auto config = base_config();

    auto result = tv_protocol::parse_extension_payload(
        price_alert_payload(
            "alerts_updated",
            nlohmann::json::array({
                nlohmann::json{
                    {"alert_id", 123},
                    {"active", false},
                    {"message", "EURUSD Crossing 1.14072"}
                }
            })),
        "test-secret",
        config);

    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, "ignored_state_message");
    EXPECT_FALSE(result.signal);
}

TEST(TradingViewExtensionProtocol, RejectsInvalidSecret) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"action", "buy"},
        {"symbol", "FX:EURUSD"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "wrong-secret", config);

    EXPECT_FALSE(result.accepted);
    EXPECT_FALSE(result.authorized);
    EXPECT_EQ(result.reason, "invalid_secret");
    EXPECT_FALSE(result.signal);
}

TEST(TradingViewExtensionProtocol, PreservesFingerprintAsDedupeFallback) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"fingerprint", "fnv1a:abc123"},
        {"action", "buy"},
        {"symbol", "FX:EURUSD"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    ASSERT_TRUE(result.signal);
    EXPECT_EQ(result.dedupe_key, "fnv1a:abc123");
    EXPECT_NE(result.signal->user_data.find("\"fingerprint\":\"fnv1a:abc123\""), std::string::npos);
}

TEST(TradingViewExtensionProtocol, PrefersEventIdOverFingerprint) {
    auto config = base_config();

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "tv_toast:event-123"},
        {"fingerprint", "fnv1a:abc123"},
        {"action", "buy"},
        {"symbol", "FX:EURUSD"}
    };

    auto result =
        tv_protocol::parse_extension_payload(payload, "test-secret", config);

    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.event_id, "tv_toast:event-123");
    EXPECT_EQ(result.dedupe_key, "tv_toast:event-123");
}

TEST(TradingViewExtensionBridge, ReportsRejectedLevelAlertOverHttp) {
    auto config = base_config();
    config.port = 0;

    optionx::bridges::tradingview::TradingViewExtensionBridge bridge;
    BridgeGuard guard{bridge};
    ASSERT_TRUE(bridge.configure(std::make_unique<TradingViewExtensionBridgeConfig>(config)));

    bridge.on_signal_id() = [] {
        return optionx::SignalId{100};
    };
    bridge.on_trade_signal() = [](std::unique_ptr<optionx::TradeSignal>) {};

    std::mutex reports_mutex;
    std::vector<optionx::BridgeSignalReport> reports;
    bridge.on_signal_report() = [&](const optionx::BridgeSignalReport& report) {
        std::lock_guard<std::mutex> lock(reports_mutex);
        reports.push_back(report);
    };

    bridge.run();
    ASSERT_TRUE(wait_for_port(bridge));

    HttpClient client(config.address + ":" + std::to_string(bridge.bound_port()));
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-OptionX-Secret", config.secret);

    const nlohmann::json payload = {
        {"source_kind", "alert_toast_dom"},
        {"event_id", "toast:btcusd:unmapped"},
        {"action", "alert"},
        {"symbol", "BTCUSD"},
        {"message", "BTCUSD Crossing 64114.82"}
    };

    const auto response =
        client.request("POST", config.signal_path, payload.dump(), headers);
    const auto response_json = nlohmann::json::parse(response->content.string());
    EXPECT_FALSE(response_json.at("accepted").get<bool>());
    EXPECT_EQ(response_json.at("reason"), "unmapped_level_alert");

    std::vector<optionx::BridgeSignalReport> reports_snapshot;
    {
        std::lock_guard<std::mutex> lock(reports_mutex);
        reports_snapshot = reports;
    }

    ASSERT_EQ(reports_snapshot.size(), 1u);
    const auto& report = reports_snapshot.front();
    EXPECT_EQ(report.status, optionx::BridgeSignalReportStatus::REJECTED);
    EXPECT_EQ(report.reason_code, "unmapped_level_alert");
    EXPECT_EQ(report.bridge_id, config.bridge_id);
    EXPECT_EQ(report.bridge_type, optionx::BridgeType::TRADING_VIEW_EXTENSION_HTTP);
    EXPECT_EQ(report.event_id, "toast:btcusd:unmapped");
    EXPECT_EQ(report.dedupe_key, "toast:btcusd:unmapped");
    EXPECT_EQ(report.symbol, "BTCUSD");
    EXPECT_EQ(report.parsed_payload.at("condition_type"), "crossing");
    EXPECT_FALSE(report.candidate_signal);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
