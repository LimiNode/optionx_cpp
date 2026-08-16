#include <gtest/gtest.h>

#include "optionx_cpp/bridges/telegram.hpp"

namespace {

optionx::bridges::telegram::TelegramRawMessage message_with_text(std::string text) {
    optionx::bridges::telegram::TelegramRawMessage message;
    message.chat_id = "-10042";
    message.chat_title = "Morning signals";
    message.message_id = 123;
    message.date_ms = 1800000000000;
    message.text = std::move(text);
    return message;
}

} // namespace

TEST(TelegramSignalParser, ParsesExecutableSignalAndNormalizesExpiry) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("EURUSD BUY 5m"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_EQ(parsed.signals[0].symbol, "EURUSD");
    EXPECT_EQ(parsed.signals[0].order_type, optionx::OrderType::BUY);
    EXPECT_EQ(parsed.signals[0].option_type, optionx::OptionType::SPRINT);
    EXPECT_EQ(parsed.signals[0].duration, 300u);
    EXPECT_EQ(parsed.signals[0].source_message_identity, "telegram:-10042:0:123");
    EXPECT_EQ(parsed.signals[0].source_date_ms, 1800000000000);
    EXPECT_TRUE(parsed.outcomes.empty());
}

TEST(TelegramSignalParser, PreservesSourceTimeForOutcomes) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("EURUSD WIN"));

    ASSERT_EQ(parsed.outcomes.size(), 1u);
    EXPECT_EQ(parsed.outcomes[0].source_date_ms, 1800000000000);
}

TEST(TelegramSignalParser, AppliesCustomExpiryDurationPolicy) {
    auto config = optionx::bridges::telegram::TelegramSignalParser::default_config();
    config.expiry_policy.mode =
        optionx::bridges::telegram::TelegramExpiryMode::CUSTOM_DURATION;
    config.expiry_policy.custom_duration_seconds = 180;

    const auto parsed = optionx::bridges::telegram::TelegramSignalParser(
        std::move(config)).parse(message_with_text("EURUSD BUY 5m"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_EQ(parsed.signals[0].option_type, optionx::OptionType::SPRINT);
    EXPECT_EQ(parsed.signals[0].duration, 180u);
    EXPECT_EQ(parsed.signals[0].expiry_time, 0);
}

TEST(TelegramSignalParser, ExpiryPolicyOverridesConflictingRuleOptionType) {
    auto config = optionx::bridges::telegram::TelegramSignalParser::default_config();
    config.signal_rules = {
        {"classic-rule", R"((EURUSD)\s+(BUY)\s+(\d+)(m)\b)",
         1, 2, 3, 4, optionx::OptionType::CLASSIC},
    };

    const auto parsed = optionx::bridges::telegram::TelegramSignalParser(
        std::move(config)).parse(message_with_text("EURUSD BUY 5m"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_EQ(parsed.signals[0].option_type, optionx::OptionType::SPRINT);
    EXPECT_EQ(parsed.signals[0].duration, 300u);
    EXPECT_EQ(parsed.signals[0].expiry_time, 0);
}

TEST(TelegramSignalParser, CustomExpiryAppliesWithoutMessageTimeframe) {
    auto config = optionx::bridges::telegram::TelegramSignalParser::default_config();
    config.expiry_policy.mode =
        optionx::bridges::telegram::TelegramExpiryMode::CUSTOM_DURATION;
    config.expiry_policy.custom_duration_seconds = 180;
    config.signal_rules = {
        {"fixed-duration", R"((EURUSD)\s+(BUY)\b)",
         1, 2, 0, 0, optionx::OptionType::CLASSIC},
    };

    const auto parsed = optionx::bridges::telegram::TelegramSignalParser(
        std::move(config)).parse(message_with_text("EURUSD BUY"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_EQ(parsed.signals[0].option_type, optionx::OptionType::SPRINT);
    EXPECT_EQ(parsed.signals[0].duration, 180u);
    EXPECT_EQ(parsed.signals[0].expiry_time, 0);
}

TEST(TelegramSignalParser, CalculatesClassicExpiryAtNextBarEnd) {
    auto config = optionx::bridges::telegram::TelegramSignalParser::default_config();
    config.expiry_policy.mode = optionx::bridges::telegram::TelegramExpiryMode::BAR_END;

    const auto parsed = optionx::bridges::telegram::TelegramSignalParser(
        std::move(config)).parse(message_with_text("EURUSD BUY 5m"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_EQ(parsed.signals[0].option_type, optionx::OptionType::CLASSIC);
    EXPECT_EQ(parsed.signals[0].duration, 0u);
    EXPECT_EQ(parsed.signals[0].expiry_time, 1800000300);
}

TEST(TelegramSignalParser, ParsesMoneyBotSignalNamesAndSeparatesReportedStats) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(
            "\xF0\x9F\x93\xA3 BTC / USD m15 SELL \xE2\xAC\x87\xEF\xB8\x8F BRO-15-RF\n"
            "\xF0\x9F\x93\xA3 USD / CAD m15 SELL Pulse Zone new\n"
            "\xF0\x9F\x93\xA3 BTC / USD m5 BUY \xE2\xAC\x86\xEF\xB8\x8F BRO-5-RF\n"
            "\xE2\x9E\xA1 AUD / USD m5 BUY AP-5 \xE2\x9C\x85 9 | 1 =90.0\n"
            "\xE2\x9E\xA1 EUR / USD m5 BUY AP-5 \xE2\x9C\x85 3 | 1 =75.0"));

    ASSERT_EQ(parsed.signals.size(), 3u);
    EXPECT_EQ(parsed.signals[0].signal_name, "BRO-15-RF");
    EXPECT_EQ(parsed.signals[1].signal_name, "Pulse Zone new");
    EXPECT_EQ(parsed.signals[2].signal_name, "BRO-5-RF");

    ASSERT_EQ(parsed.outcomes.size(), 2u);
    EXPECT_EQ(parsed.outcomes[0].result,
              optionx::bridges::telegram::TelegramOutcomeResult::WIN);
    EXPECT_EQ(parsed.outcomes[0].signal_name, "AP-5");
    EXPECT_EQ(parsed.outcomes[1].result,
              optionx::bridges::telegram::TelegramOutcomeResult::WIN);
    EXPECT_EQ(parsed.outcomes[1].signal_name, "AP-5");
}

TEST(TelegramSignalParser, ParsesRealWorldPairFormatsAndStrategyName) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(
            "BTC / USD m5 BUY COBRA -5\n"
            "5M EURUSD 13:50 SELL\n"
            "30M EURCAD 19:00 SELL"));

    ASSERT_EQ(parsed.signals.size(), 3u);
    EXPECT_EQ(parsed.signals[0].symbol, "BTC/USD");
    EXPECT_EQ(parsed.signals[0].duration, 300u);
    EXPECT_EQ(parsed.signals[0].order_type, optionx::OrderType::BUY);
    EXPECT_EQ(parsed.signals[0].signal_name, "COBRA");
    EXPECT_EQ(parsed.signals[1].symbol, "EURUSD");
    EXPECT_EQ(parsed.signals[1].duration, 300u);
    EXPECT_EQ(parsed.signals[1].order_type, optionx::OrderType::SELL);
    EXPECT_EQ(parsed.signals[2].symbol, "EURCAD");
    EXPECT_EQ(parsed.signals[2].duration, 1800u);
}

TEST(TelegramSignalParser, DoesNotInferMartingaleStepFromStrategySuffix) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("BTC / USD m5 BUY COBRA -5"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_FALSE(parsed.signals[0].martingale_step.has_value());
}

TEST(TelegramSignalParser, ExtractsConfiguredExplicitMartingaleStep) {
    auto config = optionx::bridges::telegram::TelegramSignalParser::default_config();
    config.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };

    const auto parsed = optionx::bridges::telegram::TelegramSignalParser(
        std::move(config)).parse(message_with_text("EURUSD BUY 5m COBRA MG-2"));

    ASSERT_EQ(parsed.signals.size(), 1u)
        << (parsed.diagnostics.empty() ? "no diagnostic" : parsed.diagnostics.front().message);
    ASSERT_TRUE(parsed.signals[0].martingale_step.has_value());
    EXPECT_EQ(*parsed.signals[0].martingale_step, 2);
    EXPECT_EQ(parsed.signals[0].signal_name, "COBRA");
}

TEST(TelegramSignalParser, NormalizesConfiguredMartingaleMarkerForOutcomes) {
    auto config = optionx::bridges::telegram::TelegramSignalParser::default_config();
    config.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };

    const auto parsed = optionx::bridges::telegram::TelegramSignalParser(
        std::move(config)).parse(message_with_text(
            "EURUSD BUY 5m COBRA MG-2\n"
            "EURUSD BUY COBRA MG-2 \xE2\x9C\x85"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_EQ(parsed.signals[0].signal_name, "COBRA");
    ASSERT_EQ(parsed.outcomes.size(), 1u);
    EXPECT_EQ(parsed.outcomes[0].signal_name, "COBRA");
    ASSERT_TRUE(parsed.outcomes[0].martingale_step.has_value());
    EXPECT_EQ(*parsed.outcomes[0].martingale_step, 2);
}

TEST(TelegramSignalParser, RejectsAmbiguousConfiguredMartingaleStep) {
    auto config = optionx::bridges::telegram::TelegramSignalParser::default_config();
    config.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };

    const auto parsed = optionx::bridges::telegram::TelegramSignalParser(
        std::move(config)).parse(message_with_text("EURUSD BUY 5m MG-1 MG-2"));

    EXPECT_TRUE(parsed.signals.empty());
    ASSERT_EQ(parsed.diagnostics.size(), 1u);
    EXPECT_EQ(parsed.diagnostics.front().code, "invalid_signal_match");
}

TEST(TelegramSignalParser, ParsesProfitOutcomeFromReplyText) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("BTCUSD 13:50 SELL\nBTCUSD -> Profit 5586 | 55 =99.0"));

    ASSERT_EQ(parsed.outcomes.size(), 1u);
    EXPECT_EQ(parsed.outcomes[0].symbol, "BTCUSD");
    EXPECT_EQ(parsed.outcomes[0].result,
              optionx::bridges::telegram::TelegramOutcomeResult::WIN);
}

TEST(TelegramSignalParser, ParsesEmojiDecoratedScheduledSignal) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(u8"🔊 5M BTCUSD 15:50 BUY ⬆️"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_TRUE(parsed.outcomes.empty());
    EXPECT_EQ(parsed.signals[0].symbol, "BTCUSD");
    EXPECT_EQ(parsed.signals[0].order_type, optionx::OrderType::BUY);
    EXPECT_EQ(parsed.signals[0].duration, 300u);
}

TEST(TelegramSignalParser, OutcomeEmojiSuppressesOverlappingSignalCandidate) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(u8"➡️ BTC / USD m5 SELL COBRA -5 ✅ 5612 | 55 =99.0"));

    EXPECT_TRUE(parsed.signals.empty());
    ASSERT_EQ(parsed.outcomes.size(), 1u);
    EXPECT_EQ(parsed.outcomes[0].symbol, "BTC/USD");
    EXPECT_EQ(parsed.outcomes[0].order_type, optionx::OrderType::SELL);
    EXPECT_EQ(parsed.outcomes[0].result,
              optionx::bridges::telegram::TelegramOutcomeResult::WIN);
}

TEST(TelegramSignalParser, UsesDirectionEmojiForStatisticsSignal) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(u8"🔽 BTCUSD gap-01, win 60%, pay 70%, ping 562 ms, delay 281 ms"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_TRUE(parsed.outcomes.empty());
    EXPECT_EQ(parsed.signals[0].symbol, "BTCUSD");
    EXPECT_EQ(parsed.signals[0].order_type, optionx::OrderType::SELL);
}

TEST(TelegramSignalParser, ClassifiesEmojiResultWithoutPayoutDetails) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(u8"✅ BTCUSD gap-01, stats 55%"));

    EXPECT_TRUE(parsed.signals.empty());
    ASSERT_EQ(parsed.outcomes.size(), 1u);
    EXPECT_EQ(parsed.outcomes[0].symbol, "BTCUSD");
    EXPECT_EQ(parsed.outcomes[0].result,
              optionx::bridges::telegram::TelegramOutcomeResult::WIN);
}

TEST(TelegramSignalParser, SupportsCustomSymbolAndDirectionRules) {
    auto config = optionx::bridges::telegram::TelegramSignalParser::default_config();
    config.direction_rules = {
        {"up", R"(\bUP\b)", optionx::OrderType::BUY},
        {"down", R"(\bDOWN\b)", optionx::OrderType::SELL},
    };
    config.signal_rules = {
        {"custom", R"(\b{{SYMBOL}}\s+(UP|DOWN)\s+(\d+)(m)\b)",
         1, 2, 3, 4, optionx::OptionType::SPRINT},
    };

    const auto parsed = optionx::bridges::telegram::TelegramSignalParser(std::move(config)).parse(
        message_with_text("xEURUSD-OTC UP 5m"));

    ASSERT_EQ(parsed.signals.size(), 1u);
    EXPECT_EQ(parsed.signals[0].symbol, "XEURUSD_OTC");
    EXPECT_EQ(parsed.signals[0].order_type, optionx::OrderType::BUY);
    EXPECT_EQ(parsed.signals[0].duration, 300u);
}

TEST(TelegramSignalParser, KeepsOtcMarketSeparateFromUnqualifiedSymbol) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("EURUSD-OTC BUY 5m\nEURUSD SELL 5m\nEURUSD OTC BUY 5m"));

    ASSERT_EQ(parsed.signals.size(), 3u);
    EXPECT_EQ(parsed.signals[0].symbol, "EURUSD_OTC");
    EXPECT_EQ(parsed.signals[0].market,
              optionx::bridges::telegram::TelegramAssetMarket::OTC);
    EXPECT_EQ(parsed.signals[1].symbol, "EURUSD");
    EXPECT_EQ(parsed.signals[1].market,
              optionx::bridges::telegram::TelegramAssetMarket::UNKNOWN);
    EXPECT_EQ(parsed.signals[2].symbol, "EURUSD_OTC");
    EXPECT_EQ(parsed.signals[2].market,
              optionx::bridges::telegram::TelegramAssetMarket::OTC);
}

TEST(TelegramSignalParser, CanonicalizesCommonOtcSymbolSpellings) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(
            "EURUSD_OTC BUY 5m\n"
            "EURUSD-OTC SELL 5m\n"
            "EURUSDOTC BUY 5m\n"
            "EURUSD OTC SELL 5m\n"
            "EUR/USD OTC BUY 5m"));

    ASSERT_EQ(parsed.signals.size(), 5u);
    for (const auto& signal : parsed.signals) {
        EXPECT_EQ(signal.symbol, "EURUSD_OTC");
        EXPECT_EQ(signal.market,
                  optionx::bridges::telegram::TelegramAssetMarket::OTC);
    }
}

TEST(TelegramSignalParser, CanonicalizesOtcOutcomeSymbol) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("EURUSDOTC WIN"));

    ASSERT_EQ(parsed.outcomes.size(), 1u);
    EXPECT_EQ(parsed.outcomes[0].symbol, "EURUSD_OTC");
    EXPECT_EQ(parsed.outcomes[0].market,
              optionx::bridges::telegram::TelegramAssetMarket::OTC);
}

TEST(TelegramSignalParser, ParsesSeparatedOtcOutcomeSpellings) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(
            "EURUSD OTC WIN\n"
            "EUR/USD OTC LOSS\n"
            "OTC EURUSD REFUND"));

    ASSERT_EQ(parsed.outcomes.size(), 3u);
    EXPECT_EQ(parsed.outcomes[0].symbol, "EURUSD_OTC");
    EXPECT_EQ(parsed.outcomes[0].result,
              optionx::bridges::telegram::TelegramOutcomeResult::WIN);
    EXPECT_EQ(parsed.outcomes[1].symbol, "EURUSD_OTC");
    EXPECT_EQ(parsed.outcomes[1].result,
              optionx::bridges::telegram::TelegramOutcomeResult::LOSS);
    EXPECT_EQ(parsed.outcomes[2].symbol, "EURUSD_OTC");
    EXPECT_EQ(parsed.outcomes[2].result,
              optionx::bridges::telegram::TelegramOutcomeResult::REFUND);
    for (const auto& outcome : parsed.outcomes) {
        EXPECT_EQ(outcome.market,
                  optionx::bridges::telegram::TelegramAssetMarket::OTC);
    }
}

TEST(TelegramSignalParser, DeduplicatesOverlappingOutcomeMatches) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(std::string("EURUSD WIN ") + "\xE2\x9C\x85"));

    EXPECT_TRUE(parsed.signals.empty());
    ASSERT_EQ(parsed.outcomes.size(), 1u);
    EXPECT_EQ(parsed.outcomes[0].symbol, "EURUSD");
    EXPECT_EQ(parsed.outcomes[0].result,
              optionx::bridges::telegram::TelegramOutcomeResult::WIN);
}

TEST(TelegramSignalParser, BlocksSignalsWhenOutcomeMeaningIsAmbiguous) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(std::string("EURUSD BUY 5m ") + "\xE2\x9C\x85 \xE2\x9D\x8C"));

    EXPECT_TRUE(parsed.signals.empty());
    EXPECT_TRUE(parsed.outcomes.empty());
    ASSERT_EQ(parsed.diagnostics.size(), 1u);
    EXPECT_EQ(parsed.diagnostics.front().code,
              "ambiguous_overlapping_outcome");
}

TEST(TelegramSignalParser, KeepsSignalsAndOutcomesSeparateInLiveSmokeBatch) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text(
            "SMOKE-OTC-BATCH\n"
            "EURUSD_OTC BUY 5m\n"
            "EURUSD-OTC SELL 5m\n"
            "EURUSDOTC BUY 5m\n"
            "EURUSD OTC SELL 5m\n"
            "EUR/USD OTC BUY 5m\n"
            "BTC / USD m5 BUY COBRA -5\n"
            "5M EURUSD 15:50 SELL\n"
            "30M EURCAD 19:00 SELL\n"
            "✅ EURUSDOTC gap-01, stats 55%\n"
            "➡️ EUR/USD m5 SELL COBRA -5 ✅ 5612 | 55 =99.0\n"
            "EURUSD BUY 5 weeks\n"
            "EURUSD UP 5m\n"
            "This is EURUSD but not a trade"));

    ASSERT_EQ(parsed.signals.size(), 8u);
    EXPECT_EQ(parsed.outcomes.size(), 2u);
    for (const auto& signal : parsed.signals) {
        if (signal.market == optionx::bridges::telegram::TelegramAssetMarket::OTC) {
            EXPECT_EQ(signal.symbol, "EURUSD_OTC");
        }
    }
}

TEST(TelegramSignalParser, ParsesMultipleSignalsAndOutcomeSeparately) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("EURUSD BUY 5m\nGBPUSD SELL 10m\nEURUSD WIN"));

    ASSERT_EQ(parsed.signals.size(), 2u);
    EXPECT_EQ(parsed.signals[0].symbol, "EURUSD");
    EXPECT_EQ(parsed.signals[1].symbol, "GBPUSD");
    ASSERT_EQ(parsed.outcomes.size(), 1u);
    EXPECT_EQ(parsed.outcomes[0].symbol, "EURUSD");
    EXPECT_EQ(parsed.outcomes[0].result,
              optionx::bridges::telegram::TelegramOutcomeResult::WIN);
}

TEST(TelegramSignalParser, RejectsUnsupportedExpiryUnitFailClosed) {
    const auto parsed = optionx::bridges::telegram::TelegramSignalParser().parse(
        message_with_text("USDJPY BUY 2 weeks"));

    EXPECT_TRUE(parsed.signals.empty());
    ASSERT_EQ(parsed.diagnostics.size(), 1u);
    EXPECT_EQ(parsed.diagnostics.front().code, "unmatched_expiry");
}

TEST(TelegramSignalParser, RejectsConflictingOverlappingRules) {
    auto config = optionx::bridges::telegram::TelegramParserConfig{};
    config.signal_rules = {
        {"sprint", R"((EURUSD)\s+(BUY))", 1, 2, 0, 0, optionx::OptionType::SPRINT},
        {"classic", R"((EURUSD)\s+(BUY))", 1, 2, 0, 0, optionx::OptionType::CLASSIC},
    };
    optionx::bridges::telegram::TelegramSignalParser parser(std::move(config));
    const auto parsed = parser.parse(message_with_text("EURUSD BUY"));

    EXPECT_TRUE(parsed.signals.empty());
    ASSERT_EQ(parsed.diagnostics.size(), 1u);
    EXPECT_EQ(parsed.diagnostics.front().code, "ambiguous_overlapping_signal");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
