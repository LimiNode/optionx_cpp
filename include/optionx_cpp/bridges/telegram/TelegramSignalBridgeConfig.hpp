#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_CONFIG_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_BRIDGE_CONFIG_HPP_INCLUDED

/// \file TelegramSignalBridgeConfig.hpp
/// \brief Configuration for the Telegram signal bridge.

#include "data/bridge.hpp"
#include "bridges/telegram/TelegramSignalParser.hpp"

#include <cmath>
#include <memory>
#include <string>

namespace optionx::bridges::telegram {

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

    /// \class TelegramSignalBridgeConfig
    /// \brief Parser and dispatch settings for Telegram live signal intake.
    class TelegramSignalBridgeConfig final : public IBridgeConfig {
    public:
        BridgeId bridge_id = 0;
        double fixed_amount = 0.0;
        std::size_t dedupe_cache_size = 4096;
        TelegramParserConfig parser = TelegramSignalParser::default_config();

        void to_json(nlohmann::json& j) const override {
            j = nlohmann::json{
                {"bridge_id", bridge_id},
                {"fixed_amount", fixed_amount},
                {"dedupe_cache_size", dedupe_cache_size},
                {"symbol_pattern", parser.symbol_pattern},
                {"otc_symbol_suffix", parser.otc_symbol_suffix},
                {"use_chat_title_as_signal_name", parser.use_chat_title_as_signal_name},
                {"signal_rules", nlohmann::json::array()},
                {"direction_rules", nlohmann::json::array()},
                {"outcome_rules", nlohmann::json::array()},
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
        }

        void from_json(const nlohmann::json& j) override {
            bridge_id = j.value("bridge_id", bridge_id);
            fixed_amount = j.value("fixed_amount", fixed_amount);
            dedupe_cache_size = j.value("dedupe_cache_size", dedupe_cache_size);
            parser.symbol_pattern = j.value("symbol_pattern", parser.symbol_pattern);
            parser.otc_symbol_suffix = j.value("otc_symbol_suffix", parser.otc_symbol_suffix);
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
        }

        std::pair<bool, std::string> validate() const override {
            if (!std::isfinite(fixed_amount) || fixed_amount <= 0.0) {
                return {false, "Telegram fixed_amount must be positive and finite."};
            }
            if (dedupe_cache_size == 0) {
                return {false, "Telegram dedupe_cache_size must be positive."};
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
