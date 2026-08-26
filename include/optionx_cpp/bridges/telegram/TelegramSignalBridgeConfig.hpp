#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_CONFIG_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_CONFIG_HPP_INCLUDED

/// \file TelegramSignalBridgeConfig.hpp
/// \brief Configuration for the Telegram signal bridge.

#include "data/bridge.hpp"
#include "bridges/telegram/detail/TelegramSignalParser.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace optionx::bridges::telegram {

    /// \enum TelegramMartingalePolicy
    /// \brief Selects how explicitly marked martingale steps are dispatched.
    enum class TelegramMartingalePolicy {
        UNKNOWN,
        ALL_SIGNALS,
        FIRST_SIGNAL_ONLY,
        CONTIGUOUS_STEPS
    };

    inline const char* telegram_martingale_policy_name(
            const TelegramMartingalePolicy policy) {
        switch (policy) {
        case TelegramMartingalePolicy::FIRST_SIGNAL_ONLY:
            return "FIRST_SIGNAL_ONLY";
        case TelegramMartingalePolicy::CONTIGUOUS_STEPS:
            return "CONTIGUOUS_STEPS";
        case TelegramMartingalePolicy::ALL_SIGNALS:
            return "ALL_SIGNALS";
        case TelegramMartingalePolicy::UNKNOWN:
        default:
            return "UNKNOWN";
        }
    }

    inline TelegramMartingalePolicy telegram_martingale_policy_from_name(
            const std::string& value) {
        if (value == "FIRST_SIGNAL_ONLY") {
            return TelegramMartingalePolicy::FIRST_SIGNAL_ONLY;
        }
        if (value == "CONTIGUOUS_STEPS") {
            return TelegramMartingalePolicy::CONTIGUOUS_STEPS;
        }
        if (value == "ALL_SIGNALS") {
            return TelegramMartingalePolicy::ALL_SIGNALS;
        }
        return TelegramMartingalePolicy::UNKNOWN;
    }

    inline const char* telegram_outcome_result_name(
            const TelegramOutcomeResult result) {
        switch (result) {
        case TelegramOutcomeResult::WIN:
            return "WIN";
        case TelegramOutcomeResult::LOSS:
            return "LOSS";
        case TelegramOutcomeResult::REFUND:
            return "REFUND";
        case TelegramOutcomeResult::UNKNOWN:
        default:
            return "UNKNOWN";
        }
    }

    inline TelegramOutcomeResult telegram_outcome_result_from_name(
            const std::string& value) {
        if (value == "WIN") {
            return TelegramOutcomeResult::WIN;
        }
        if (value == "LOSS") {
            return TelegramOutcomeResult::LOSS;
        }
        if (value == "REFUND") {
            return TelegramOutcomeResult::REFUND;
        }
        return TelegramOutcomeResult::UNKNOWN;
    }

    inline const char* telegram_expiry_mode_name(
            const TelegramExpiryMode mode) {
        switch (mode) {
        case TelegramExpiryMode::TIMEFRAME_DURATION:
            return "TIMEFRAME_DURATION";
        case TelegramExpiryMode::BAR_END:
            return "BAR_END";
        case TelegramExpiryMode::CUSTOM_DURATION:
            return "CUSTOM_DURATION";
        case TelegramExpiryMode::UNKNOWN:
        default:
            return "UNKNOWN";
        }
    }

    inline TelegramExpiryMode telegram_expiry_mode_from_name(
            const std::string& value) {
        if (value == "TIMEFRAME_DURATION") {
            return TelegramExpiryMode::TIMEFRAME_DURATION;
        }
        if (value == "BAR_END") {
            return TelegramExpiryMode::BAR_END;
        }
        if (value == "CUSTOM_DURATION") {
            return TelegramExpiryMode::CUSTOM_DURATION;
        }
        return TelegramExpiryMode::UNKNOWN;
    }

    /// \class TelegramSignalBridgeConfig
    /// \brief Parser and dispatch settings for Telegram live signal intake.
    class TelegramSignalBridgeConfig final : public IBridgeConfig {
    public:
        BridgeId bridge_id = 0;
        double fixed_amount = 0.0;
        std::size_t dedupe_cache_size = 4096;
        std::uint32_t max_signal_age_seconds = 0;
        TelegramMartingalePolicy martingale_policy = TelegramMartingalePolicy::ALL_SIGNALS;
        /// \brief Enables broker-result-driven anti-martingale stake sizing.
        bool anti_martingale_enabled = false;
        /// \brief Stake multiplier applied after each confirmed broker WIN.
        double anti_martingale_multiplier = 2.0;
        /// \brief Maximum number of consecutive winning step increases.
        std::uint32_t anti_martingale_max_steps = 1;
        /// \brief Absolute amount cap required when anti-martingale is enabled.
        double anti_martingale_max_amount = 0.0;
        TelegramParserConfig parser = TelegramSignalParser::default_config();

        void to_json(nlohmann::json& j) const override {
            j = nlohmann::json{
                {"bridge_id", bridge_id},
                {"fixed_amount", fixed_amount},
                {"dedupe_cache_size", dedupe_cache_size},
                {"max_signal_age_seconds", max_signal_age_seconds},
                {"martingale_policy", telegram_martingale_policy_name(martingale_policy)},
                {"anti_martingale_enabled", anti_martingale_enabled},
                {"anti_martingale_multiplier", anti_martingale_multiplier},
                {"anti_martingale_max_steps", anti_martingale_max_steps},
                {"anti_martingale_max_amount", anti_martingale_max_amount},
                {"symbol_pattern", parser.symbol_pattern},
                {"otc_symbol_suffix", parser.otc_symbol_suffix},
                {"expiry_mode", telegram_expiry_mode_name(parser.expiry_policy.mode)},
                {"custom_expiry_seconds", parser.expiry_policy.custom_duration_seconds},
                {"use_chat_title_as_signal_name", parser.use_chat_title_as_signal_name},
                {"signal_rules", nlohmann::json::array()},
                {"direction_rules", nlohmann::json::array()},
                {"outcome_rules", nlohmann::json::array()},
                {"martingale_rules", nlohmann::json::array()},
            };
            for (const auto& rule : parser.signal_rules) {
                j["signal_rules"].push_back({
                    {"name", rule.name},
                    {"pattern", rule.pattern},
                    {"symbol_group", rule.symbol_group},
                    {"direction_group", rule.direction_group},
                    {"expiry_group", rule.expiry_group},
                    {"unit_group", rule.unit_group},
                    {"option_type", rule.option_type},
                });
            }
            for (const auto& rule : parser.direction_rules) {
                j["direction_rules"].push_back({
                    {"name", rule.name},
                    {"pattern", rule.pattern},
                    {"order_type", rule.order_type},
                });
            }
            for (const auto& rule : parser.outcome_rules) {
                j["outcome_rules"].push_back({
                    {"name", rule.name},
                    {"pattern", rule.pattern},
                    {"symbol_group", rule.symbol_group},
                    {"result_group", rule.result_group},
                    {"fixed_result", telegram_outcome_result_name(rule.fixed_result)},
                    {"direction_group", rule.direction_group},
                });
            }
            for (const auto& rule : parser.martingale_rules) {
                j["martingale_rules"].push_back({
                    {"name", rule.name},
                    {"pattern", rule.pattern},
                    {"step_group", rule.step_group},
                });
            }
        }

        void from_json(const nlohmann::json& j) override {
            bridge_id = j.value("bridge_id", bridge_id);
            fixed_amount = j.value("fixed_amount", fixed_amount);
            dedupe_cache_size = j.value("dedupe_cache_size", dedupe_cache_size);
            max_signal_age_seconds = j.value(
                "max_signal_age_seconds", max_signal_age_seconds);
            if (j.contains("martingale_policy")) {
                martingale_policy = telegram_martingale_policy_from_name(
                    j.at("martingale_policy").get<std::string>());
            }
            anti_martingale_enabled = j.value(
                "anti_martingale_enabled", anti_martingale_enabled);
            anti_martingale_multiplier = j.value(
                "anti_martingale_multiplier", anti_martingale_multiplier);
            anti_martingale_max_steps = j.value(
                "anti_martingale_max_steps", anti_martingale_max_steps);
            anti_martingale_max_amount = j.value(
                "anti_martingale_max_amount", anti_martingale_max_amount);
            parser.symbol_pattern = j.value("symbol_pattern", parser.symbol_pattern);
            parser.otc_symbol_suffix = j.value("otc_symbol_suffix", parser.otc_symbol_suffix);
            if (j.contains("expiry_mode")) {
                parser.expiry_policy.mode = telegram_expiry_mode_from_name(
                    j.at("expiry_mode").get<std::string>());
            }
            parser.expiry_policy.custom_duration_seconds = j.value(
                "custom_expiry_seconds",
                parser.expiry_policy.custom_duration_seconds);
            parser.use_chat_title_as_signal_name = j.value(
                "use_chat_title_as_signal_name",
                parser.use_chat_title_as_signal_name);

            if (j.contains("signal_rules")) {
                parser.signal_rules.clear();
                for (const auto& item : j.at("signal_rules")) {
                    TelegramSignalRule rule;
                    rule.name = item.value("name", "");
                    rule.pattern = item.at("pattern").get<std::string>();
                    rule.symbol_group = item.value("symbol_group", 1u);
                    rule.direction_group = item.value("direction_group", 2u);
                    rule.expiry_group = item.value("expiry_group", 3u);
                    rule.unit_group = item.value("unit_group", 4u);
                    rule.option_type = item.value("option_type", OptionType::SPRINT);
                    parser.signal_rules.push_back(std::move(rule));
                }
            }
            if (j.contains("direction_rules")) {
                parser.direction_rules.clear();
                for (const auto& item : j.at("direction_rules")) {
                    TelegramDirectionRule rule;
                    rule.name = item.value("name", "");
                    rule.pattern = item.at("pattern").get<std::string>();
                    rule.order_type = item.value("order_type", OrderType::UNKNOWN);
                    parser.direction_rules.push_back(std::move(rule));
                }
            }
            if (j.contains("outcome_rules")) {
                parser.outcome_rules.clear();
                for (const auto& item : j.at("outcome_rules")) {
                    TelegramOutcomeRule rule;
                    rule.name = item.value("name", "");
                    rule.pattern = item.at("pattern").get<std::string>();
                    rule.symbol_group = item.value("symbol_group", 1u);
                    rule.result_group = item.value("result_group", 2u);
                    rule.fixed_result = telegram_outcome_result_from_name(
                        item.value("fixed_result", std::string("UNKNOWN")));
                    rule.direction_group = item.value("direction_group", 0u);
                    parser.outcome_rules.push_back(std::move(rule));
                }
            }
            if (j.contains("martingale_rules")) {
                parser.martingale_rules.clear();
                for (const auto& item : j.at("martingale_rules")) {
                    TelegramMartingaleRule rule;
                    rule.name = item.value("name", "");
                    rule.pattern = item.at("pattern").get<std::string>();
                    rule.step_group = item.value("step_group", 1u);
                    parser.martingale_rules.push_back(std::move(rule));
                }
            }
        }

        std::pair<bool, std::string> validate() const override {
            if (!std::isfinite(fixed_amount) || fixed_amount <= 0.0) {
                return {false, "Telegram fixed_amount must be positive and finite."};
            }
            if (dedupe_cache_size == 0) {
                return {false, "Telegram dedupe_cache_size must be positive."};
            }
            if (martingale_policy == TelegramMartingalePolicy::UNKNOWN) {
                return {false, "Telegram martingale_policy is unsupported."};
            }
            if (anti_martingale_enabled &&
                martingale_policy == TelegramMartingalePolicy::CONTIGUOUS_STEPS) {
                return {false,
                        "Telegram anti-martingale cannot use CONTIGUOUS_STEPS martingale_policy."};
            }
            if (anti_martingale_enabled &&
                (!std::isfinite(anti_martingale_multiplier) ||
                 anti_martingale_multiplier <= 1.0)) {
                return {false,
                        "Telegram anti_martingale_multiplier must be finite and greater than one."};
            }
            if (anti_martingale_enabled && anti_martingale_max_steps == 0) {
                return {false, "Telegram anti_martingale_max_steps must be positive."};
            }
            if (anti_martingale_enabled &&
                (!std::isfinite(anti_martingale_max_amount) ||
                 anti_martingale_max_amount < fixed_amount)) {
                return {false,
                        "Telegram anti_martingale_max_amount must be finite and at least fixed_amount."};
            }
            if (parser.expiry_policy.mode == TelegramExpiryMode::UNKNOWN) {
                return {false, "Telegram expiry_mode is unsupported."};
            }
            if (parser.expiry_policy.mode == TelegramExpiryMode::CUSTOM_DURATION &&
                parser.expiry_policy.custom_duration_seconds == 0) {
                return {false, "Telegram custom_expiry_seconds must be positive."};
            }
            try {
                (void)TelegramSignalParser(parser);
            }
            catch (const std::exception& error) {
                return {false, std::string("Invalid Telegram parser rules: ") + error.what()};
            }
            return {true, {}};
        }

        std::unique_ptr<IBridgeConfig> clone_unique() const override {
            return std::make_unique<TelegramSignalBridgeConfig>(*this);
        }

        std::shared_ptr<IBridgeConfig> clone_shared() const override {
            return std::make_shared<TelegramSignalBridgeConfig>(*this);
        }

        BridgeType bridge_type() const override {
            return BridgeType::TELEGRAM_SIGNAL;
        }
    };

} // namespace optionx::bridges::telegram

#endif // OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_CONFIG_HPP_INCLUDED
