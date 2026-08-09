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

    /// \struct TelegramDirectionRule
    /// \brief Maps a textual or UTF-8 emoji direction token to an order type.
    struct TelegramDirectionRule {
        std::string name;
        std::string pattern;
        OrderType order_type = OrderType::UNKNOWN;
    };

    /// \struct TelegramOutcomeRule
    /// \brief Capture-group mapping for one outcome regex.
    ///
    /// A rule may provide a fixed result and use result_group == 0 for
    /// outcome text that has no stable result token, such as "Profit".
    struct TelegramOutcomeRule {
        std::string name;
        std::string pattern;
        std::size_t symbol_group = 1;
        std::size_t result_group = 2;
        TelegramOutcomeResult fixed_result = TelegramOutcomeResult::UNKNOWN;
        std::size_t direction_group = 0;
    };

    /// \struct TelegramParserConfig
    /// \brief Source-independent deterministic parser configuration.
    struct TelegramParserConfig {
        // Inserted as one capture group wherever a rule contains {{SYMBOL}}.
        std::string symbol_pattern =
            R"((?!(?:BUY|SELL|CALL|PUT|WIN|LOSS|REFUND|DRAW|PROFIT|OTC)\b)[A-Z][A-Z0-9]{2,11}(?:\s*[/_-]\s*[A-Z][A-Z0-9]{1,11})?)";
        // Canonical OTC symbols use the same suffix as UTE and the rest of
        // the trading stack. A source-specific alias may override it.
        std::string otc_symbol_suffix = "_OTC";
        std::vector<TelegramSignalRule> signal_rules;
        std::vector<TelegramDirectionRule> direction_rules;
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
            const std::string symbol = "{{SYMBOL}}";
            config.direction_rules = {
                {"buy-word", R"(\b(?:BUY|CALL)\b)", OrderType::BUY},
                {"sell-word", R"(\b(?:SELL|PUT)\b)", OrderType::SELL},
                // Telegram messages may contain either the emoji with or
                // without its optional variation selector.
                {"buy-up-emoji", "\xE2\xAC\x86(?:\xEF\xB8\x8F)?", OrderType::BUY},
                {"sell-down-emoji", "\xE2\xAC\x87(?:\xEF\xB8\x8F)?", OrderType::SELL},
                {"buy-up-arrow-emoji", "\xF0\x9F\x94\xBC", OrderType::BUY},
                {"sell-down-arrow-emoji", "\xF0\x9F\x94\xBD", OrderType::SELL},
            };
            config.signal_rules.push_back({
                "pair-prefix-expiry-direction",
                "\\b" + symbol +
                    R"([ \t]+(s|sec|secs|m|min|mins|h|hr|hour|hours)[ \t]*(\d{1,5})[ \t]+(BUY|SELL|CALL|PUT)\b)",
                1,
                4,
                3,
                2,
                OptionType::SPRINT,
            });
            config.signal_rules.push_back({
                "pair-expiry-direction",
                "\\b" + symbol +
                    R"([ \t]+(\d{1,5})[ \t]*(s|sec|secs|m|min|mins|h|hr|hour|hours)[ \t]+(BUY|SELL|CALL|PUT)\b)",
                1,
                4,
                2,
                3,
                OptionType::SPRINT,
            });
            config.signal_rules.push_back({
                "pair-direction-expiry",
                "\\b" + symbol +
                    R"([ \t]+(BUY|SELL|CALL|PUT)\b(?:[ \t]+(\d{1,5})[ \t]*(s|sec|secs|m|min|mins|h|hr|hour|hours))?)",
                1,
                2,
                3,
                4,
                OptionType::SPRINT,
            });
            config.signal_rules.push_back({
                "pair-otc-direction-expiry",
                "\\b" + symbol +
                    R"([ \t]+OTC\b[ \t]+(BUY|SELL|CALL|PUT)\b(?:[ \t]+(\d{1,5})[ \t]*(s|sec|secs|m|min|mins|h|hr|hour|hours))?)",
                1,
                2,
                3,
                4,
                OptionType::SPRINT,
            });
            config.signal_rules.push_back({
                "otc-pair-direction-expiry",
                R"(\bOTC[ \t]+)" + symbol +
                    R"([ \t]+(BUY|SELL|CALL|PUT)\b(?:[ \t]+(\d{1,5})[ \t]*(s|sec|secs|m|min|mins|h|hr|hour|hours))?)",
                1,
                2,
                3,
                4,
                OptionType::SPRINT,
            });
            config.signal_rules.push_back({
                "expiry-pair-scheduled-direction",
                R"(\b(\d{1,5})[ \t]*(s|sec|secs|m|min|mins|h|hr|hour|hours)[ \t]+)" + symbol +
                    R"((?:[ \t]+\d{1,2}:\d{2})?[ \t]+(BUY|SELL|CALL|PUT)\b)",
                3,
                4,
                1,
                2,
                OptionType::SPRINT,
            });
            config.signal_rules.push_back({
                "expiry-pair-scheduled-direction-fallback",
                R"(\b(\d{1,5})[ \t]*(s|sec|secs|m|min|mins|h|hr|hour|hours)[ \t]+)" + symbol +
                    R"((?:[ \t]+\d{1,2}:\d{2})?(?![ \t]+(?:BUY|SELL|CALL|PUT)\b))",
                3,
                0,
                1,
                2,
                OptionType::SPRINT,
            });
            config.signal_rules.push_back({
                "pair-expiry-direction-fallback",
                "\\b" + symbol +
                    R"([ \t]+(\d{1,5})[ \t]*(s|sec|secs|m|min|mins|h|hr|hour|hours)\b(?![ \t]+(?:BUY|SELL|CALL|PUT)\b))",
                1,
                0,
                2,
                3,
                OptionType::SPRINT,
            });
            config.signal_rules.push_back({
                "pair-statistics-direction-fallback",
                "\\b" + symbol +
                    R"([^\r\n]{0,160}\b(?:WIN|STATS|PAY|PING|DELAY)\b[^\r\n]{0,160})",
                1,
                0,
                0,
                0,
                OptionType::SPRINT,
            });
            config.outcome_rules.push_back({
                "pair-outcome",
                "\\b" + symbol + R"([ \t]+(WIN|LOSS|REFUND|DRAW)\b)",
                1,
                2,
            });
            config.outcome_rules.push_back({
                "pair-otc-outcome",
                "\\b" + symbol + R"([ \t]+OTC[ \t]+(WIN|LOSS|REFUND|DRAW)\b)",
                1,
                2,
            });
            config.outcome_rules.push_back({
                "otc-pair-outcome",
                R"(\bOTC[ \t]+)" + symbol +
                    R"([ \t]+(WIN|LOSS|REFUND|DRAW)\b)",
                1,
                2,
            });
            config.outcome_rules.push_back({
                "pair-profit",
                "\\b" + symbol + R"([^\r\n]{0,40}\bPROFIT\b)",
                1,
                0,
                TelegramOutcomeResult::WIN,
            });
            config.outcome_rules.push_back({
                "pair-win-emoji-after",
                "\\b" + symbol +
                    R"([^\r\n]{0,160}\xE2\x9C\x85)",
                1,
                0,
                TelegramOutcomeResult::WIN,
            });
            config.outcome_rules.push_back({
                "pair-win-emoji-before",
                "\xE2\x9C\x85[ \t]*" + symbol + R"([^\r\n]{0,160})",
                1,
                0,
                TelegramOutcomeResult::WIN,
            });
            config.outcome_rules.push_back({
                "pair-loss-emoji-after",
                "\\b" + symbol +
                    R"([^\r\n]{0,160}\xE2\x9D\x8C)",
                1,
                0,
                TelegramOutcomeResult::LOSS,
            });
            config.outcome_rules.push_back({
                "pair-loss-emoji-before",
                "\xE2\x9D\x8C[ \t]*" + symbol + R"([^\r\n]{0,160})",
                1,
                0,
                TelegramOutcomeResult::LOSS,
            });
            return config;
        }

        explicit TelegramSignalParser(
                TelegramParserConfig config = default_config())
            : m_config(std::move(config)) {
            for (const auto& rule : m_config.signal_rules) {
                m_signal_rules.push_back({
                    rule,
                    std::regex(expand_symbol_pattern(rule.pattern),
                               std::regex::ECMAScript | std::regex::icase),
                });
            }
            for (const auto& rule : m_config.direction_rules) {
                m_direction_rules.push_back({
                    rule,
                    std::regex(rule.pattern, std::regex::ECMAScript | std::regex::icase),
                });
            }
            for (const auto& rule : m_config.outcome_rules) {
                m_outcome_rules.push_back({
                    rule,
                    std::regex(expand_symbol_pattern(rule.pattern),
                               std::regex::ECMAScript | std::regex::icase),
                });
            }
        }

        /// \brief Parses one message into signals, outcomes and diagnostics.
        TelegramParsedMessage parse(const TelegramRawMessage& raw) const {
            raw.validate();
            TelegramParsedMessage result;
            result.raw = raw;
            const auto outcome_spans = parse_outcomes(raw, result);
            parse_signals(raw, result, outcome_spans);
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

        struct CompiledDirectionRule {
            TelegramDirectionRule spec;
            std::regex expression;
        };

        struct SignalCandidate {
            std::size_t begin = 0;
            std::size_t end = 0;
            TelegramParsedSignal signal;
        };

        struct OutcomeCandidate {
            std::size_t begin = 0;
            std::size_t end = 0;
            TelegramParsedOutcome outcome;
        };

        using TextSpan = std::pair<std::size_t, std::size_t>;

        std::string expand_symbol_pattern(std::string pattern) const {
            static constexpr const char* placeholder = "{{SYMBOL}}";
            const auto token_length = std::char_traits<char>::length(placeholder);
            std::size_t position = 0;
            while ((position = pattern.find(placeholder, position)) != std::string::npos) {
                pattern.replace(position, token_length, "(" +
                    m_config.symbol_pattern + ")");
                position += m_config.symbol_pattern.size() + 2;
            }
            return pattern;
        }

        static std::string upper(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::toupper(ch));
            });
            return value;
        }

        static std::string normalize_symbol(std::string value) {
            value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
                return std::isspace(ch);
            }), value.end());
            return upper(std::move(value));
        }

        static TelegramAssetMarket classify_market(
                const std::string& symbol,
                const std::string& matched_text) {
            static const std::regex otc_suffix(
                R"((?:^|[-_/])OTC$)",
                std::regex::ECMAScript | std::regex::icase);
            static const std::regex otc_token(
                R"(\bOTC\b)",
                std::regex::ECMAScript | std::regex::icase);
            const auto normalized_symbol = normalize_symbol(symbol);
            const bool otc_attached_suffix =
                normalized_symbol.size() > 3 &&
                normalized_symbol.compare(
                    normalized_symbol.size() - 3, 3, "OTC") == 0;
            if (otc_attached_suffix ||
                std::regex_search(symbol, otc_suffix) ||
                std::regex_search(matched_text, otc_token)) {
                return TelegramAssetMarket::OTC;
            }
            return TelegramAssetMarket::UNKNOWN;
        }

        static std::string strip_otc_suffix(std::string symbol) {
            symbol = normalize_symbol(std::move(symbol));
            if (symbol.size() > 3 &&
                symbol.compare(symbol.size() - 3, 3, "OTC") == 0) {
                symbol.erase(symbol.size() - 3);
                if (!symbol.empty() &&
                    (symbol.back() == '_' || symbol.back() == '-' ||
                     symbol.back() == '/')) {
                    symbol.pop_back();
                }
            }
            symbol.erase(std::remove(symbol.begin(), symbol.end(), '/'), symbol.end());
            return symbol;
        }

        std::string canonical_symbol(
                std::string symbol,
                const TelegramAssetMarket market) const {
            if (market != TelegramAssetMarket::OTC) {
                return symbol;
            }
            if (m_config.otc_symbol_suffix.empty()) {
                return strip_otc_suffix(std::move(symbol));
            }
            return strip_otc_suffix(std::move(symbol)) + m_config.otc_symbol_suffix;
        }

        static std::string trim_ascii(std::string value) {
            const auto first = value.find_first_not_of(" \t");
            if (first == std::string::npos) {
                return {};
            }
            const auto last = value.find_last_not_of(" \t");
            return value.substr(first, last - first + 1);
        }

        static std::string signal_name_from_tail(
                const TelegramRawMessage& raw,
                const std::smatch& match) {
            const auto begin = static_cast<std::size_t>(match.position() + match.length());
            if (begin >= raw.text.size()) {
                return {};
            }
            const auto line_end = raw.text.find_first_of("\r\n", begin);
            auto tail = trim_ascii(raw.text.substr(begin, line_end - begin));
            static const std::regex step_suffix(
                R"(\s+-\s*\d+\s*$)",
                std::regex::ECMAScript);
            tail = std::regex_replace(tail, step_suffix, "");
            tail = trim_ascii(std::move(tail));
            if (tail.empty() || !std::isalpha(static_cast<unsigned char>(tail.front()))) {
                return {};
            }
            return tail;
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

        static std::string line_text(
                const TelegramRawMessage& raw,
                const std::size_t begin,
                const std::size_t end) {
            const auto line_begin = raw.text.rfind('\n', begin == 0 ? 0 : begin - 1);
            const auto line_end = raw.text.find('\n', end);
            const auto first = line_begin == std::string::npos ? 0 : line_begin + 1;
            return raw.text.substr(first, line_end == std::string::npos
                ? std::string::npos
                : line_end - first);
        }

        std::optional<OrderType> resolve_direction(
                const std::string& text) const {
            std::optional<OrderType> resolved;
            for (const auto& compiled : m_direction_rules) {
                if (!std::regex_search(text, compiled.expression)) {
                    continue;
                }
                if (resolved && *resolved != compiled.spec.order_type) {
                    throw std::invalid_argument("signal direction is ambiguous");
                }
                resolved = compiled.spec.order_type;
            }
            if (!resolved) {
                OrderType standard = OrderType::UNKNOWN;
                if (optionx::to_enum(upper(text), standard)) {
                    resolved = standard;
                }
            }
            return resolved;
        }

        std::optional<OrderType> resolve_direction(
                const TelegramRawMessage& raw,
                const std::size_t begin,
                const std::size_t end) const {
            return resolve_direction(line_text(raw, begin, end));
        }

        TelegramParsedSignal build_signal(
                const TelegramRawMessage& raw,
                const TelegramSignalRule& rule,
                const std::smatch& match,
                TelegramParsedMessage& result) const {
            if (!has_group(match, rule.symbol_group)) {
                throw std::invalid_argument("signal rule has no symbol");
            }

            TelegramParsedSignal signal;
            signal.source_message_identity = raw.message_identity();
            signal.market = classify_market(
                match[rule.symbol_group].str(), match.str());
            signal.symbol = canonical_symbol(
                normalize_symbol(match[rule.symbol_group].str()), signal.market);
            if (has_group(match, rule.direction_group)) {
                const auto direction = resolve_direction(match[rule.direction_group].str());
                if (!direction) {
                    throw std::invalid_argument("signal direction is unsupported");
                }
                signal.order_type = *direction;
            }
            else {
                const auto direction = resolve_direction(
                    raw,
                    static_cast<std::size_t>(match.position()),
                    static_cast<std::size_t>(match.position() + match.length()));
                if (!direction) {
                    throw std::invalid_argument("signal direction is missing");
                }
                signal.order_type = *direction;
            }
            signal.option_type = rule.option_type;
            signal.signal_name = signal_name_from_tail(raw, match);
            if (signal.signal_name.empty()) {
                signal.signal_name = m_config.use_chat_title_as_signal_name
                    ? raw.chat_title
                    : rule.name;
            }
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

        static bool same_outcome(
                const TelegramParsedOutcome& left,
                const TelegramParsedOutcome& right) {
            return left.symbol == right.symbol &&
                left.order_type == right.order_type &&
                left.result == right.result &&
                left.market == right.market;
        }

        void parse_signals(
                const TelegramRawMessage& raw,
                TelegramParsedMessage& result,
                const std::vector<TextSpan>& outcome_spans) const {
            std::vector<SignalCandidate> candidates;
            for (const auto& compiled : m_signal_rules) {
                for (std::sregex_iterator it(raw.text.begin(), raw.text.end(), compiled.expression);
                     it != std::sregex_iterator(); ++it) {
                    const auto& match = *it;
                    const auto begin = static_cast<std::size_t>(match.position());
                    const auto end = begin + static_cast<std::size_t>(match.length());
                    const auto overlaps_outcome = std::any_of(
                        outcome_spans.begin(), outcome_spans.end(), [&](const auto& span) {
                            return begin < span.second && span.first < end;
                        });
                    if (overlaps_outcome) {
                        continue;
                    }
                    try {
                        candidates.push_back({
                            begin,
                            end,
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
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                for (const auto& span : outcome_spans) {
                    if (candidates[i].begin < span.second && span.first < candidates[i].end) {
                        rejected[i] = true;
                        break;
                    }
                }
            }
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

        static bool has_otc_prefix(
                const std::string& text,
                const std::size_t begin) {
            const auto line_begin = text.rfind('\n', begin == 0 ? 0 : begin - 1);
            const auto first = line_begin == std::string::npos ? 0 : line_begin + 1;
            auto prefix = trim_ascii(text.substr(first, begin - first));
            const auto separator = prefix.find_last_of(" \t");
            prefix = prefix.substr(separator == std::string::npos ? 0 : separator + 1);
            return upper(std::move(prefix)) == "OTC";
        }

        std::vector<TextSpan> parse_outcomes(
                const TelegramRawMessage& raw,
                TelegramParsedMessage& result) const {
            std::vector<OutcomeCandidate> candidates;
            for (const auto& compiled : m_outcome_rules) {
                for (std::sregex_iterator it(raw.text.begin(), raw.text.end(), compiled.expression);
                     it != std::sregex_iterator(); ++it) {
                    const auto& match = *it;
                    try {
                        if (compiled.spec.fixed_result == TelegramOutcomeResult::UNKNOWN &&
                            !has_group(match, compiled.spec.result_group)) {
                            throw std::invalid_argument("outcome rule has no result");
                        }
                        TelegramParsedOutcome outcome;
                        outcome.source_message_identity = raw.message_identity();
                        outcome.reply_to_message_identity = raw.reply_to_message_identity();
                        if (has_group(match, compiled.spec.symbol_group)) {
                            const auto normalized_symbol = normalize_symbol(
                                match[compiled.spec.symbol_group].str());
                            outcome.market = classify_market(
                                match[compiled.spec.symbol_group].str(), match.str());
                            outcome.symbol = canonical_symbol(
                                normalized_symbol,
                                outcome.market);
                            if (outcome.market == TelegramAssetMarket::UNKNOWN &&
                                has_otc_prefix(
                                    raw.text,
                                    static_cast<std::size_t>(match.position()))) {
                                outcome.market = TelegramAssetMarket::OTC;
                                outcome.symbol = canonical_symbol(
                                    normalized_symbol,
                                    outcome.market);
                            }
                        }
                        outcome.result = compiled.spec.fixed_result;
                        if (has_group(match, compiled.spec.result_group)) {
                            outcome.result = outcome_from_token(
                                match[compiled.spec.result_group].str());
                        }
                        if (has_group(match, compiled.spec.direction_group)) {
                            const auto direction = resolve_direction(
                                match[compiled.spec.direction_group].str());
                            if (direction) {
                                outcome.order_type = *direction;
                            }
                        }
                        else if (const auto direction = resolve_direction(
                                raw,
                                static_cast<std::size_t>(match.position()),
                                static_cast<std::size_t>(match.position() + match.length()))) {
                            outcome.order_type = *direction;
                        }
                        outcome.signal_name = raw.chat_title;
                        outcome.raw_text = match.str();
                        candidates.push_back({
                            static_cast<std::size_t>(match.position()),
                            static_cast<std::size_t>(match.position() + match.length()),
                            std::move(outcome),
                        });
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

            std::sort(candidates.begin(), candidates.end(), [](
                    const auto& left,
                    const auto& right) {
                if (left.begin != right.begin) {
                    return left.begin < right.begin;
                }
                return left.end < right.end;
            });

            std::vector<TextSpan> blocking_spans;
            blocking_spans.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                blocking_spans.emplace_back(candidate.begin, candidate.end);
            }

            std::vector<bool> rejected(candidates.size(), false);
            bool ambiguous = false;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                for (std::size_t j = i + 1; j < candidates.size(); ++j) {
                    if (candidates[j].begin >= candidates[i].end) {
                        break;
                    }
                    if (same_outcome(
                            candidates[i].outcome,
                            candidates[j].outcome)) {
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
                    "ambiguous_overlapping_outcome",
                    "overlapping outcome rules produced different meanings",
                    0,
                    raw.text.size(),
                });
            }

            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (rejected[i]) {
                    continue;
                }
                result.outcomes.push_back(std::move(candidates[i].outcome));
            }
            return blocking_spans;
        }

        TelegramParserConfig m_config;
        std::vector<CompiledSignalRule> m_signal_rules;
        std::vector<CompiledDirectionRule> m_direction_rules;
        std::vector<CompiledOutcomeRule> m_outcome_rules;
    };

} // namespace optionx::bridges::telegram

#endif // OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_SIGNAL_PARSER_HPP_INCLUDED
