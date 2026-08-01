#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_PARSER_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_PARSER_HPP_INCLUDED

/// \file TelegramSignalParser.hpp
/// \brief Deterministic regex parser for Telegram signal and outcome messages.

#include "bridges/telegram/TelegramParsedMessage.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace optionx::bridges::telegram {

    /// \struct TelegramSignalRule
    /// \brief Capture-group mapping and defaults for one signal regex.
    struct TelegramSignalRule {
        std::string name;
        std::string pattern;
        std::size_t symbol_group = 1;
        std::size_t direction_group = 2;
        std::size_t expiry_group = 3;
        std::size_t unit_group = 4;
        OptionType option_type = OptionType::SPRINT;
    };

    /// \struct TelegramOutcomeRule
    /// \brief Capture-group mapping for one outcome regex.
    struct TelegramOutcomeRule {
        std::string name;
        std::string pattern;
        std::size_t symbol_group = 1;
        std::size_t result_group = 2;
    };

    /// \struct TelegramParserConfig
    /// \brief Source-independent deterministic parser configuration.
    struct TelegramParserConfig {
        std::vector<TelegramSignalRule> signal_rules;
        std::vector<TelegramOutcomeRule> outcome_rules;
        bool use_chat_title_as_signal_name = true;
    };

    /// \class TelegramSignalParser
    /// \brief Parses raw text while preserving outcomes and diagnostics.
    class TelegramSignalParser final {
    public:
        /// \brief Returns conservative rules for common binary signal text.
        static TelegramParserConfig default_config() {
            TelegramParserConfig config;
            config.signal_rules.push_back({
                "pair-direction-expiry",
                R"(\b([A-Z]{3,6}(?:[/_-]?[A-Z]{3,6})?)\s+(BUY|SELL|CALL|PUT)\b(?:\s+(\d{1,5})\s*(s|sec|secs|m|min|mins|h|hr|hour|hours))?)",
                1,
                2,
                3,
                4,
                OptionType::SPRINT,
            });
            config.outcome_rules.push_back({
                "pair-outcome",
                R"(\b([A-Z]{3,6}(?:[/_-]?[A-Z]{3,6})?)\s+(WIN|LOSS|REFUND|DRAW)\b)",
                1,
                2,
            });
            return config;
        }

        explicit TelegramSignalParser(
                TelegramParserConfig config = default_config())
            : m_config(std::move(config)) {
            for (const auto& rule : m_config.signal_rules) {
                m_signal_rules.push_back({
                    rule,
                    std::regex(rule.pattern, std::regex::ECMAScript | std::regex::icase),
                });
            }
            for (const auto& rule : m_config.outcome_rules) {
                m_outcome_rules.push_back({
                    rule,
                    std::regex(rule.pattern, std::regex::ECMAScript | std::regex::icase),
                });
            }
        }

        /// \brief Parses one message into signals, outcomes and diagnostics.
        TelegramParsedMessage parse(const TelegramRawMessage& raw) const {
            raw.validate();
            TelegramParsedMessage result;
            result.raw = raw;
            parse_signals(raw, result);
            parse_outcomes(raw, result);
            return result;
        }

    private:
        struct CompiledSignalRule {
            TelegramSignalRule spec;
            std::regex expression;
        };

        struct CompiledOutcomeRule {
            TelegramOutcomeRule spec;
            std::regex expression;
        };

        struct SignalCandidate {
            std::size_t begin = 0;
            std::size_t end = 0;
            TelegramParsedSignal signal;
        };

        static std::string upper(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::toupper(ch));
            });
            return value;
        }

        static bool has_group(
                const std::smatch& match,
                const std::size_t group) {
            return group != 0 && group < match.size() && match[group].matched;
        }

        static std::uint32_t parse_duration(
                const std::string& value,
                const std::string& unit) {
            std::uint64_t amount = 0;
            try {
                std::size_t consumed = 0;
                amount = std::stoull(value, &consumed);
                if (consumed != value.size() || amount == 0) {
                    throw std::invalid_argument("expiry amount is invalid");
                }
            }
            catch (const std::exception&) {
                throw std::invalid_argument("expiry amount is invalid");
            }

            const auto normalized_unit = upper(unit);
            std::uint64_t multiplier = 0;
            if (normalized_unit == "S" || normalized_unit == "SEC" ||
                normalized_unit == "SECS") {
                multiplier = 1;
            }
            else if (normalized_unit == "M" || normalized_unit == "MIN" ||
                     normalized_unit == "MINS") {
                multiplier = 60;
            }
            else if (normalized_unit == "H" || normalized_unit == "HR" ||
                     normalized_unit == "HOUR" || normalized_unit == "HOURS") {
                multiplier = 60 * 60;
            }
            else {
                throw std::invalid_argument("expiry unit is unsupported");
            }
            if (amount > (std::numeric_limits<std::uint32_t>::max)() / multiplier) {
                throw std::invalid_argument("expiry duration is too large");
            }
            return static_cast<std::uint32_t>(amount * multiplier);
        }

        TelegramParsedSignal build_signal(
                const TelegramRawMessage& raw,
                const TelegramSignalRule& rule,
                const std::smatch& match,
                TelegramParsedMessage& result) const {
            if (!has_group(match, rule.symbol_group) ||
                !has_group(match, rule.direction_group)) {
                throw std::invalid_argument("signal rule has no symbol or direction");
            }

            TelegramParsedSignal signal;
            signal.source_message_identity = raw.message_identity();
            signal.symbol = upper(match[rule.symbol_group].str());
            if (!optionx::to_enum(upper(match[rule.direction_group].str()), signal.order_type)) {
                throw std::invalid_argument("signal direction is unsupported");
            }
            signal.option_type = rule.option_type;
            signal.signal_name = m_config.use_chat_title_as_signal_name
                ? raw.chat_title
                : rule.name;
            signal.raw_text = match.str();

            if (has_group(match, rule.expiry_group)) {
                if (!has_group(match, rule.unit_group)) {
                    throw std::invalid_argument("signal expiry has no unit");
                }
                signal.duration = parse_duration(
                    match[rule.expiry_group].str(),
                    match[rule.unit_group].str());
            }
            return signal;
        }

        static bool same_signal(
                const TelegramParsedSignal& left,
                const TelegramParsedSignal& right) {
            return left.symbol == right.symbol &&
                left.order_type == right.order_type &&
                left.option_type == right.option_type &&
                left.duration == right.duration &&
                left.expiry_time == right.expiry_time &&
                left.signal_name == right.signal_name;
        }

        void parse_signals(
                const TelegramRawMessage& raw,
                TelegramParsedMessage& result) const {
            std::vector<SignalCandidate> candidates;
            for (const auto& compiled : m_signal_rules) {
                for (std::sregex_iterator it(raw.text.begin(), raw.text.end(), compiled.expression);
                     it != std::sregex_iterator(); ++it) {
                    const auto& match = *it;
                    try {
                        candidates.push_back({
                            static_cast<std::size_t>(match.position()),
                            static_cast<std::size_t>(match.position() + match.length()),
                            build_signal(raw, compiled.spec, match, result),
                        });
                    }
                    catch (const std::exception& error) {
                        result.diagnostics.push_back({
                            "invalid_signal_match",
                            error.what(),
                            static_cast<std::size_t>(match.position()),
                            static_cast<std::size_t>(match.length()),
                        });
                    }
                }
            }

            std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
                if (left.begin != right.begin) {
                    return left.begin < right.begin;
                }
                return left.end < right.end;
            });

            std::vector<bool> rejected(candidates.size(), false);
            bool unmatched_expiry = false;
            static const std::regex trailing_expiry(
                R"(^\s+(?:\d+\s*)?(?:s|sec|secs|m|min|mins|h|hr|hour|hours|d|day|days|w|week|weeks|tick|ticks)\b)",
                std::regex::ECMAScript | std::regex::icase);
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (candidates[i].signal.duration != 0) {
                    continue;
                }
                const auto tail = raw.text.substr(
                    candidates[i].end,
                    std::min<std::size_t>(raw.text.size() - candidates[i].end, 48));
                if (std::regex_search(tail, trailing_expiry)) {
                    rejected[i] = true;
                    unmatched_expiry = true;
                }
            }
            bool ambiguous = false;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                for (std::size_t j = i + 1; j < candidates.size(); ++j) {
                    if (candidates[j].begin >= candidates[i].end) {
                        break;
                    }
                    if (same_signal(candidates[i].signal, candidates[j].signal)) {
                        rejected[j] = true;
                    }
                    else {
                        rejected[i] = true;
                        rejected[j] = true;
                        ambiguous = true;
                    }
                }
            }
            if (ambiguous) {
                result.diagnostics.push_back({
                    "ambiguous_overlapping_signal",
                    "overlapping signal rules produced different meanings",
                    0,
                    raw.text.size(),
                });
            }
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (!rejected[i]) {
                    result.signals.push_back(std::move(candidates[i].signal));
                }
            }

            if (unmatched_expiry) {
                result.diagnostics.push_back({
                    "unmatched_expiry",
                    "signal expiry was not recognized",
                    0,
                    raw.text.size(),
                });
            }
            else if (result.signals.empty() && !raw.text.empty()) {
                static const std::regex unsupported_expiry(
                    R"(\b(?:BUY|SELL|CALL|PUT)\b[^\r\n]{0,32}\b(?:\d+\s*)?(?:d|day|days|w|week|weeks|tick|ticks)\b)",
                    std::regex::ECMAScript | std::regex::icase);
                std::smatch match;
                if (std::regex_search(raw.text, match, unsupported_expiry)) {
                    result.diagnostics.push_back({
                        "unmatched_expiry",
                        "signal expiry was not recognized",
                        static_cast<std::size_t>(match.position()),
                        static_cast<std::size_t>(match.length()),
                    });
                }
            }
        }

        static TelegramOutcomeResult outcome_from_token(const std::string& value) {
            const auto token = upper(value);
            if (token == "WIN") {
                return TelegramOutcomeResult::WIN;
            }
            if (token == "LOSS") {
                return TelegramOutcomeResult::LOSS;
            }
            if (token == "REFUND") {
                return TelegramOutcomeResult::REFUND;
            }
            return TelegramOutcomeResult::UNKNOWN;
        }

        void parse_outcomes(
                const TelegramRawMessage& raw,
                TelegramParsedMessage& result) const {
            for (const auto& compiled : m_outcome_rules) {
                for (std::sregex_iterator it(raw.text.begin(), raw.text.end(), compiled.expression);
                     it != std::sregex_iterator(); ++it) {
                    const auto& match = *it;
                    try {
                        if (!has_group(match, compiled.spec.result_group)) {
                            throw std::invalid_argument("outcome rule has no result");
                        }
                        TelegramParsedOutcome outcome;
                        outcome.source_message_identity = raw.message_identity();
                        outcome.reply_to_message_identity = raw.reply_to_message_identity();
                        if (has_group(match, compiled.spec.symbol_group)) {
                            outcome.symbol = upper(match[compiled.spec.symbol_group].str());
                        }
                        outcome.result = outcome_from_token(
                            match[compiled.spec.result_group].str());
                        outcome.signal_name = raw.chat_title;
                        outcome.raw_text = match.str();
                        result.outcomes.push_back(std::move(outcome));
                    }
                    catch (const std::exception& error) {
                        result.diagnostics.push_back({
                            "invalid_outcome_match",
                            error.what(),
                            static_cast<std::size_t>(match.position()),
                            static_cast<std::size_t>(match.length()),
                        });
                    }
                }
            }
        }

        TelegramParserConfig m_config;
        std::vector<CompiledSignalRule> m_signal_rules;
        std::vector<CompiledOutcomeRule> m_outcome_rules;
    };

} // namespace optionx::bridges::telegram

#endif // OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_PARSER_HPP_INCLUDED
