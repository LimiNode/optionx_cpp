#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_HTTP_PARSERS_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_HTTP_PARSERS_HPP_INCLUDED

/// \file http_parsers.hpp
/// \brief Helper functions for parsing HTTP and WebSocket responses for the
///        IntradeBar trading platform.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <cmath>
#include <limits>
#include <optional>
#include <regex>
#include <stdexcept>
#include <utility>

#include <optionx_cpp/utils/response_parse_utils.hpp>
#include "ApiResponses.hpp"
#include "http_utils.hpp"

namespace optionx::platforms::intrade_bar {

    inline std::uint32_t duration_sec_from_ms_delta(std::int64_t delta_ms) noexcept {
        if (delta_ms <= 0) return 0;
        const auto seconds = time_shield::ms_to_sec(delta_ms);
        if (seconds <= 0 ||
            seconds > static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())) {
            return 0;
        }
        return static_cast<std::uint32_t>(seconds);
    }

    /// \brief Parses the login response and extracts user ID and hash.
    /// \param content The HTTP response content to parse.
    /// \return A pair of optional values for user_id and user_hash. If parsing fails, both values will be std::nullopt.
    inline std::optional<std::pair<std::string, std::string>> parse_login(const std::string& content) {
        try {
            std::string user_id, user_hash, fragment;
            // Extract "/auth/" fragment
            if (utils::extract_between(content, "/auth/", "'", fragment) == std::string::npos || fragment.empty()) {
                LOGIT_ERROR("Failed to extract auth fragment.");
                return std::nullopt;
            }

            // Extract user ID
            if (utils::extract_between(fragment, "id=", "&", user_id) == std::string::npos || user_id.empty()) {
                LOGIT_ERROR("Failed to extract user ID.");
                return std::nullopt;
            }

            // Extract user hash
            if (utils::extract_after(fragment, "hash=", user_hash) == std::string::npos || user_hash.empty()) {
                LOGIT_ERROR("Failed to extract user hash.");
                return std::nullopt;
            }

            return {{user_id, user_hash}};
        } catch (...) {
            return std::nullopt;
        }
    }

    /// \brief Parses balance information and detects the currency.
    /// \param content Raw HTML fragment containing the balance value and currency symbol.
    /// \return Optional pair of balance amount and detected currency type. Returns
    ///         std::nullopt if the content cannot be parsed.
    inline std::optional<std::pair<double, CurrencyType>> parse_balance(const std::string& content) {
        try {
            const std::string STR_RUB = u8"₽";
            const std::string STR_RUB_UTF8 = "\xE2\x82\xBD";
            const std::string STR_USD = u8"$";

            CurrencyType currency = CurrencyType::UNKNOWN;
            if (content.find(STR_RUB_UTF8) != std::string::npos ||
                content.find(STR_RUB) != std::string::npos ||
                content.find("RUB") != std::string::npos) {
                currency = CurrencyType::RUB;
            } else if (content.find(STR_USD) != std::string::npos || content.find("USD") != std::string::npos) {
                currency = CurrencyType::USD;
            } else {
                LOGIT_ERROR("Unsupported currency type detected.");
                return std::nullopt;
            }

            std::string cleaned_content = content;
            std::replace(cleaned_content.begin(), cleaned_content.end(), ',', '.');
            const std::string marker = (currency == CurrencyType::RUB) ? STR_RUB_UTF8 : STR_USD;
            size_t pos = cleaned_content.find(marker);
            if (pos == std::string::npos && currency == CurrencyType::RUB) {
                pos = cleaned_content.find(STR_RUB);
            }
            if (pos == std::string::npos && currency == CurrencyType::RUB) {
                pos = cleaned_content.find("RUB");
            } else if (pos == std::string::npos && currency == CurrencyType::USD) {
                pos = cleaned_content.find("USD");
            }
            if (pos != std::string::npos) {
                cleaned_content = cleaned_content.substr(0, pos);
            }

            cleaned_content.erase(std::remove(cleaned_content.begin(), cleaned_content.end(), ' '), cleaned_content.end());
            return {{std::stod(cleaned_content), currency}};
        } catch (...) {
            return std::nullopt;
        }
    }

    /// \brief Parses the response content to extract account and currency information.
    /// \param content The HTTP response content to parse.
    /// \return A pair of optional values for CurrencyType and AccountType. If parsing fails, both values will be std::nullopt.
    inline std::pair<CurrencyType, AccountType> parse_profile_response(const std::string& content) {
        // Strings for matching
        const char str_demo_ru[] = u8"Демо";
        const char str_real_ru[] = u8"Реал";
        const char str_demo_en[] = u8"Demo";
        const char str_real_en[] = u8"Real";
        const char str_rub[] = u8"RUB";
        const char str_usd[] = u8"USD";

        CurrencyType currency = CurrencyType::UNKNOWN;
        AccountType account = AccountType::UNKNOWN;

        // Offset for parsing
        size_t offset = 0;
        while (true) {
            std::string temp;
            size_t new_offset = utils::extract_between(content, "<div class=\"radio\">", "</div>", temp, offset);
            if (new_offset == std::string::npos) break;
            offset = new_offset;

            // Determine account type
            if ((temp.find(str_demo_ru) != std::string::npos ||
                 temp.find(str_demo_en) != std::string::npos) &&
                temp.find("checked=\"checked\"") != std::string::npos) {
                account = AccountType::DEMO;
            } else if ((temp.find(str_real_ru) != std::string::npos ||
                        temp.find(str_real_en) != std::string::npos) &&
                       temp.find("checked=\"checked\"") != std::string::npos) {
                account = AccountType::REAL;
            }

            // Determine currency
            if (temp.find(str_rub) != std::string::npos &&
                temp.find("checked=\"checked\"") != std::string::npos) {
                currency = CurrencyType::RUB;
            } else if (temp.find(str_usd) != std::string::npos &&
                       temp.find("checked=\"checked\"") != std::string::npos) {
                currency = CurrencyType::USD;
            }
        }

        return {currency, account};
    }

    /// \brief Parses the platform's main page to extract request identifiers and cookies.
    /// \param content The raw HTML of the response body.
    /// \param headers The HTTP headers returned with the response.
    /// \return Optional tuple containing the request identifier, request value
    ///         and a cookie string to be used in subsequent requests. Returns
    ///         std::nullopt if any of the required fields cannot be found.
    inline std::optional<std::tuple<std::string, std::string, std::string>> parse_main_page_response(
            const std::string& content,
            const kurlyk::Headers& headers) {
        std::string req_id, req_value, cookies;

        // Extract hidden input fields
        std::string fragment;
        if (utils::extract_between(content, "<input type=\"hidden\" name=\"g-rec-res-l\"", "\">", fragment) == std::string::npos) {
            LOGIT_ERROR("Failed to extract hidden input fragment.");
            return std::nullopt;
        }
        if (utils::extract_between(fragment, "id=\"", "\"", req_id) == std::string::npos) {
            LOGIT_ERROR("Failed to extract request ID.");
            return std::nullopt;
        }
        if (utils::extract_after(fragment, "value=\"", req_value) == std::string::npos) {
            LOGIT_ERROR("Failed to extract request value.");
            return std::nullopt;
        }

        // Parse cookies
        kurlyk::Cookies set_cookie;
        for (const auto& item : headers) {
            if (item.first == "set-cookie") {
                kurlyk::Cookies cookie(kurlyk::utils::parse_cookie(item.second));
                set_cookie.insert(cookie.begin(), cookie.end());
            }
        }
        cookies = kurlyk::utils::to_cookie_string(set_cookie);

        return std::make_tuple(req_id, req_value, cookies);
    }

    namespace detail {

        inline std::optional<int64_t> parse_active_trade_close_time_ms(
                const std::string& row,
                int64_t trade_id) {
            std::smatch match;
            static const std::regex close_time_regex(
                R"(setInterval\s*\(\s*showRemaining[^;]*'([0-9]+)'\s*\))");
            if (std::regex_search(row, match, close_time_regex) && match.size() >= 2) {
                if (auto close_time = utils::parse_i64_strict(match[1].str())) {
                    return time_shield::sec_to_ms(*close_time);
                }
                return std::nullopt;
            }

            const std::regex remaining_regex(
                "time_time_" + std::to_string(trade_id) + R"(\s*=\s*([0-9]+)\s*(?:;|$))");
            if (std::regex_search(row, match, remaining_regex) && match.size() >= 2) {
                if (auto remaining_sec = utils::parse_i64_strict(match[1].str())) {
                    return OPTIONX_TIMESTAMP_MS + time_shield::sec_to_ms(*remaining_sec);
                }
                return std::nullopt;
            }

            return std::nullopt;
        }

        inline void log_trade_open_response(
                const std::string& content,
                const std::string& reason) {
#           ifdef OPTIONX_LOG_UNIQUE_FILE_INDEX
            const int log_index = OPTIONX_LOG_UNIQUE_FILE_INDEX;
            LOGIT_STREAM_ERROR_TO(log_index) << content;
            LOGIT_PRINT_ERROR(
                reason,
                " Content log was written to file: ",
                LOGIT_GET_LAST_FILE_NAME(log_index)
            );
#           else
            LOGIT_PRINT_ERROR(reason);
#           endif
        }

        inline std::string empty_trade_open_response_error() {
            return "Trade open failed. Server returned an empty response; instrument may be closed or unavailable.";
        }


        struct HistoryMoney {
            double amount = 0.0;
            CurrencyType currency = CurrencyType::UNKNOWN;
        };

        inline std::vector<std::string> split_semicolon_line(const std::string& line) {
            std::vector<std::string> fields;
            std::string field;
            std::istringstream stream(line);
            while (std::getline(stream, field, ';')) {
                fields.push_back(utils::trim_copy(field));
            }
            return fields;
        }

        inline std::string lower_ascii_copy(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        inline int history_month_from_name(std::string month) {
            month = lower_ascii_copy(utils::trim_copy(month));
            static const std::unordered_map<std::string, int> months = {
                {"jan", 1}, {"feb", 2}, {"mar", 3}, {"apr", 4},
                {"may", 5}, {"jun", 6}, {"jul", 7}, {"aug", 8},
                {"sep", 9}, {"oct", 10}, {"nov", 11}, {"dec", 12}
            };
            const auto it = months.find(month);
            return it == months.end() ? 0 : it->second;
        }

        inline std::optional<int64_t> parse_history_datetime_ms(const std::string& raw) {
            std::smatch match;
            int hour = 0;
            int minute = 0;
            int second = 0;
            int day = 0;
            int month = 0;
            int year = 0;

            static const std::regex named_month(
                R"(^\s*([0-9]{1,2}):([0-9]{2}):([0-9]{2}),\s*([0-9]{1,2})\s+([A-Za-z]{3})\s+([0-9]{2,4})\s*$)");
            static const std::regex numeric_month(
                R"(^\s*([0-9]{1,2}):([0-9]{2}):([0-9]{2}),\s*([0-9]{1,2})\.([0-9]{1,2})\.([0-9]{2,4})\s*$)");

            if (std::regex_match(raw, match, named_month) && match.size() == 7) {
                auto parsed_hour = utils::parse_int_strict(match[1].str());
                auto parsed_minute = utils::parse_int_strict(match[2].str());
                auto parsed_second = utils::parse_int_strict(match[3].str());
                auto parsed_day = utils::parse_int_strict(match[4].str());
                auto parsed_year = utils::parse_int_strict(match[6].str());
                if (!parsed_hour || !parsed_minute || !parsed_second || !parsed_day || !parsed_year) return std::nullopt;
                hour = *parsed_hour;
                minute = *parsed_minute;
                second = *parsed_second;
                day = *parsed_day;
                month = history_month_from_name(match[5].str());
                year = *parsed_year;
            } else if (std::regex_match(raw, match, numeric_month) && match.size() == 7) {
                auto parsed_hour = utils::parse_int_strict(match[1].str());
                auto parsed_minute = utils::parse_int_strict(match[2].str());
                auto parsed_second = utils::parse_int_strict(match[3].str());
                auto parsed_day = utils::parse_int_strict(match[4].str());
                auto parsed_month = utils::parse_int_strict(match[5].str());
                auto parsed_year = utils::parse_int_strict(match[6].str());
                if (!parsed_hour || !parsed_minute || !parsed_second || !parsed_day || !parsed_month || !parsed_year) return std::nullopt;
                hour = *parsed_hour;
                minute = *parsed_minute;
                second = *parsed_second;
                day = *parsed_day;
                month = *parsed_month;
                year = *parsed_year;
            } else {
                return std::nullopt;
            }

            if (year < 100) year += 2000;
            if (month < 1 || month > 12 || day < 1 || day > 31 ||
                hour > 23 || minute > 59 || second > 59) {
                return std::nullopt;
            }

            constexpr int64_t broker_offset_sec = 3 * time_shield::SEC_PER_HOUR;
            const int64_t timestamp_sec = time_shield::to_timestamp(year, month, day, hour, minute, second) - broker_offset_sec;
            return time_shield::sec_to_ms(timestamp_sec);
        }

        inline std::optional<HistoryMoney> parse_history_money(std::string raw) {
            raw = utils::trim_copy(raw);
            std::replace(raw.begin(), raw.end(), ',', '.');

            CurrencyType currency = CurrencyType::UNKNOWN;
            static const std::string rub_sign_utf8 = "\xE2\x82\xBD";
            if (raw.find("USD") != std::string::npos || raw.find('$') != std::string::npos) {
                currency = CurrencyType::USD;
            } else if (raw.find(rub_sign_utf8) != std::string::npos) {
                currency = CurrencyType::RUB;
            } else if (raw.find("RUB") != std::string::npos) {
                currency = CurrencyType::RUB;
            }

            std::smatch match;
            static const std::regex number_regex(R"([-+]?[0-9]+(?:\.[0-9]+)?)");
            if (!std::regex_search(raw, match, number_regex) || match.empty()) {
                return std::nullopt;
            }
            auto amount = utils::parse_double_strict(match[0].str());
            if (!amount) return std::nullopt;
            return HistoryMoney{*amount, currency};
        }

        inline void replace_all_inplace(
                std::string& value,
                const std::string& from,
                const std::string& to) {
            if (from.empty()) return;
            std::size_t pos = 0;
            while ((pos = value.find(from, pos)) != std::string::npos) {
                value.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        inline std::string html_block_from_element_id(
                const std::string& content,
                const std::string& tag_name,
                const std::string& element_id) {
            const std::string open_tag = "<" + tag_name;
            const std::string close_tag = "</" + tag_name + ">";

            std::size_t pos = 0;
            while ((pos = content.find(open_tag, pos)) != std::string::npos) {
                const std::size_t tag_end = content.find('>', pos + open_tag.size());
                if (tag_end == std::string::npos) break;

                const std::string tag_html = content.substr(pos, tag_end - pos + 1);
                if (auto id = utils::extract_html_attr(tag_html, "id")) {
                    if (*id == element_id) {
                        const std::size_t block_end = content.find(close_tag, tag_end + 1);
                        if (block_end == std::string::npos) {
                            return content.substr(pos);
                        }
                        return content.substr(pos, block_end + close_tag.size() - pos);
                    }
                }
                pos = tag_end + 1;
            }

            return {};
        }

        inline std::string html_block_from_marker(
                const std::string& content,
                const std::string& marker) {
            const std::size_t marker_pos = content.find(marker);
            if (marker_pos == std::string::npos) return {};

            std::size_t block_start = content.rfind("<tbody", marker_pos);
            if (block_start == std::string::npos) {
                block_start = content.rfind("<table", marker_pos);
            }
            if (block_start == std::string::npos) block_start = marker_pos;

            std::size_t block_end = content.find("</tbody>", marker_pos);
            if (block_end != std::string::npos) {
                block_end += 8;
            } else {
                block_end = content.find("</table>", marker_pos);
                if (block_end != std::string::npos) {
                    block_end += 8;
                } else {
                    block_end = content.size();
                }
            }
            return content.substr(block_start, block_end - block_start);
        }

        inline std::string history_block_from_html(const std::string& content) {
            std::string block = html_block_from_element_id(content, "tbody", "trade_close");
            if (!block.empty()) return block;

            block = html_block_from_element_id(content, "tbody", "trade_history");
            if (!block.empty()) return block;

            block = html_block_from_marker(content, "trade_close");
            if (!block.empty()) return block;

            block = html_block_from_marker(content, "trade_history");
            if (!block.empty()) return block;

            // trade_load_more2.php may return plain closed-trade rows plus a script.
            return content.find("<tr") == std::string::npos ? std::string() : content;
        }

        inline std::optional<int64_t> parse_first_i64_from_string(const std::string& value) {
            std::smatch match;
            static const std::regex number_regex(R"([0-9]+)");
            if (!std::regex_search(value, match, number_regex) || match.empty()) {
                return std::nullopt;
            }
            return utils::parse_i64_strict(match[0].str());
        }

        inline std::optional<std::string> parse_history_next_last(
                const std::string& content) {
            if (auto attr = utils::extract_html_attr(content, "data-last")) {
                return utils::trim_copy(*attr);
            }

            std::smatch match;
            static const std::regex script_data_last(
                R"(attr\s*\(\s*['"]data-last['"]\s*,\s*['"]([^'"]*)['"]\s*\))");
            if (std::regex_search(content, match, script_data_last) && match.size() >= 2) {
                return utils::trim_copy(match[1].str());
            }
            return std::nullopt;
        }

        inline std::optional<int64_t> parse_history_row_option_id(const std::string& row) {
            if (auto id = utils::parse_i64_attr(row, "data-id"); id && *id > 0) {
                return id;
            }
            if (auto id = utils::parse_i64_attr(row, "data-trade-id"); id && *id > 0) {
                return id;
            }
            if (auto row_id = utils::extract_html_attr(row, "id")) {
                if (row_id->rfind("trade_inv_", 0) == 0) {
                    return parse_first_i64_from_string(*row_id);
                }
            }
            return std::nullopt;
        }

        inline std::optional<int64_t> parse_history_row_time_ms(const std::string& row) {
            const char* attr_names[] = {
                "data-timeopen",
                "data-open-time",
                "data-open",
                "data-time"
            };
            for (const char* attr_name : attr_names) {
                if (auto value = utils::parse_i64_attr(row, attr_name); value && *value > 0) {
                    return time_shield::sec_to_ms(*value);
                }
            }
            return std::nullopt;
        }

        inline std::optional<int64_t> parse_history_row_close_time_ms(const std::string& row) {
            const char* attr_names[] = {
                "data-timeclose",
                "data-close-time",
                "data-timeend",
                "data-expiration"
            };
            for (const char* attr_name : attr_names) {
                if (auto value = utils::parse_i64_attr(row, attr_name); value && *value > 0) {
                    return time_shield::sec_to_ms(*value);
                }
            }
            return std::nullopt;
        }

        inline std::vector<std::string> extract_tag_contents(
                const std::string& html,
                const std::string& tag_name) {
            std::vector<std::string> cells;
            const std::string open_tag = "<" + tag_name;
            const std::string close_tag = "</" + tag_name + ">";

            std::size_t pos = 0;
            while ((pos = html.find(open_tag, pos)) != std::string::npos) {
                const std::size_t tag_end = html.find('>', pos + open_tag.size());
                if (tag_end == std::string::npos) break;

                const std::size_t content_start = tag_end + 1;
                const std::size_t content_end = html.find(close_tag, content_start);
                if (content_end == std::string::npos) break;

                cells.push_back(html.substr(content_start, content_end - content_start));
                pos = content_end + close_tag.size();
            }

            return cells;
        }

        inline std::string strip_html_tags(const std::string& html) {
            std::string text;
            text.reserve(html.size());

            bool in_tag = false;
            for (char ch : html) {
                if (ch == '<') {
                    in_tag = true;
                    continue;
                }
                if (ch == '>') {
                    in_tag = false;
                    continue;
                }
                if (!in_tag) text.push_back(ch);
            }

            replace_all_inplace(text, "&nbsp;", " ");
            replace_all_inplace(text, "&#160;", " ");
            replace_all_inplace(text, "&amp;", "&");
            replace_all_inplace(text, "&#36;", "$");
            replace_all_inplace(text, "&#8381;", "\xE2\x82\xBD");
            return text;
        }

        inline std::vector<std::string> html_cell_lines(std::string cell_html) {
            static const std::regex br_regex(R"(<\s*br\s*/?\s*>)", std::regex_constants::icase);
            cell_html = std::regex_replace(cell_html, br_regex, "\n");

            const std::string text = strip_html_tags(cell_html);
            std::vector<std::string> lines;
            std::istringstream stream(text);
            std::string line;
            while (std::getline(stream, line)) {
                line = utils::trim_copy(line);
                if (!line.empty()) lines.push_back(std::move(line));
            }
            return lines;
        }

        inline void apply_history_gross_result_to_record(
                TradeRecord& record,
                double gross_result_amount) {
            if (record.amount <= 0.0) {
                record.trade_state = record.live_state = TradeState::CHECK_ERROR;
                record.error_code = TradeErrorCode::INVALID_REQUEST;
                record.error_desc = "Trade amount is required to classify Intrade Bar history record.";
                return;
            }

            constexpr double money_tolerance = 0.01;
            if (std::abs(gross_result_amount - record.amount) < money_tolerance) {
                record.trade_state = record.live_state = TradeState::STANDOFF;
                record.profit = 0.0;
            } else if (gross_result_amount > record.amount) {
                record.trade_state = record.live_state = TradeState::WIN;
                record.profit = gross_result_amount - record.amount;
                record.payout = utils::normalize_double(record.profit / record.amount, 2);
            } else {
                record.trade_state = record.live_state = TradeState::LOSS;
                record.profit = -record.amount;
            }

            record.error_code = TradeErrorCode::SUCCESS;
            record.error_desc.clear();
        }

        inline void apply_intrade_bar_zero_spread(TradeRecord& record) {
            set_zero_spread_for_symbol(record.spread, record.symbol);
        }

        inline std::optional<TradeRecord> parse_history_attr_row(
                const std::string& row,
                AccountType account_type) {
            auto option_id = parse_history_row_option_id(row);
            if (!option_id || *option_id <= 0) return std::nullopt;

            TradeRecord record;
            record.option_id = *option_id;
            if (auto symbol = utils::extract_html_attr(row, "data-option")) {
                record.symbol = normalize_symbol_name(*symbol);
            }
            if (auto open_price = utils::parse_double_attr(row, "data-rate")) {
                record.open_price = *open_price;
            }
            if (auto close_price = utils::parse_double_attr(row, "data-close-rate")) {
                record.close_price = *close_price;
            }
            if (auto open_time = parse_history_row_time_ms(row)) {
                record.open_date = *open_time;
            }
            if (auto close_time = parse_history_row_close_time_ms(row)) {
                record.close_date = *close_time;
                if (record.open_date > 0 && record.close_date >= record.open_date) {
                    record.duration = duration_sec_from_ms_delta(record.close_date - record.open_date);
                }
            }
            if (auto status = utils::parse_int_attr(row, "data-status")) {
                if (*status == 1) record.order_type = OrderType::BUY;
                if (*status == 2) record.order_type = OrderType::SELL;
            }
            if (auto contract = utils::parse_int_attr(row, "data-contract")) {
                if (*contract == 0) record.option_type = OptionType::SPRINT;
                if (*contract == 1) record.option_type = OptionType::CLASSIC;
            }
            record.account_type = account_type;
            record.platform_type = PlatformType::INTRADE_BAR;
            apply_intrade_bar_zero_spread(record);
            return record;
        }

        inline std::optional<TradeRecord> parse_trade_close_table_row(
                const std::string& row,
                AccountType account_type) {
            const auto cells = extract_tag_contents(row, "th");
            if (cells.size() < 4) return std::nullopt;

            const auto id_lines = html_cell_lines(cells[1]);
            const auto price_lines = html_cell_lines(cells[2]);
            const auto money_lines = html_cell_lines(cells[3]);
            if (id_lines.size() < 3 || price_lines.size() < 3 || money_lines.size() < 2) {
                return std::nullopt;
            }

            auto option_id = parse_first_i64_from_string(id_lines[0]);
            auto open_time = parse_history_datetime_ms(id_lines[1]);
            auto close_time = parse_history_datetime_ms(id_lines[2]);
            auto open_price = utils::parse_double_strict(price_lines[1]);
            auto close_price = utils::parse_double_strict(price_lines[2]);
            auto amount = parse_history_money(money_lines[0]);
            auto gross_result = parse_history_money(money_lines[1]);
            if (!option_id || *option_id <= 0 ||
                !open_time || !close_time ||
                !open_price || !close_price ||
                !amount || !gross_result) {
                return std::nullopt;
            }

            TradeRecord record;
            record.option_id = *option_id;
            record.symbol = normalize_symbol_name(price_lines[0]);
            record.open_date = *open_time;
            record.close_date = *close_time;
            if (record.close_date >= record.open_date) {
                record.duration = duration_sec_from_ms_delta(record.close_date - record.open_date);
            }
            record.open_price = *open_price;
            record.close_price = *close_price;
            record.amount = amount->amount;
            record.currency = amount->currency;
            if (record.currency == CurrencyType::UNKNOWN) {
                record.currency = gross_result->currency;
            }
            if (row.find("trading-table__up-td") != std::string::npos) {
                record.order_type = OrderType::BUY;
            } else if (row.find("trading-table__down-td") != std::string::npos) {
                record.order_type = OrderType::SELL;
            }
            record.account_type = account_type;
            record.platform_type = PlatformType::INTRADE_BAR;
            apply_history_gross_result_to_record(record, gross_result->amount);
            apply_intrade_bar_zero_spread(record);
            return record;
        }

        inline OptionType parse_history_option_type(const std::string& value) {
            const std::string normalized = lower_ascii_copy(utils::trim_copy(value));
            if (normalized.find("sprint") != std::string::npos) return OptionType::SPRINT;
            if (normalized.find("classic") != std::string::npos) return OptionType::CLASSIC;
            return OptionType::UNKNOWN;
        }

        inline OrderType parse_history_order_type(const std::string& value) {
            const std::string normalized = lower_ascii_copy(utils::trim_copy(value));
            if (normalized == "up" || normalized == "buy" || normalized == "call") return OrderType::BUY;
            if (normalized == "down" || normalized == "sell" || normalized == "put") return OrderType::SELL;
            return OrderType::UNKNOWN;
        }

    } // namespace detail

    /// \brief Parsed closed trade history page or load-more fragment.
    struct TradeHistoryHtmlPage {
        std::vector<TradeRecord> records; ///< Closed trade records parsed from the fragment.
        std::string next_last;           ///< Next value for trade_load_more2.php last parameter.
    };

    /// \brief Parses closed trade rows and pagination marker from HTML.
    /// \param content Raw authenticated page or trade_load_more2.php fragment.
    /// \param account_type Account type used for the request.
    /// \return Parsed closed trades and next pagination cursor.
    inline TradeHistoryHtmlPage parse_trade_history_html_page(
            const std::string& content,
            AccountType account_type) {
        TradeHistoryHtmlPage page;
        if (auto next_last = detail::parse_history_next_last(content)) {
            page.next_last = *next_last;
        }

        const std::string history_html = detail::history_block_from_html(content);
        if (history_html.empty()) return page;

        std::size_t pos = 0;
        for (;;) {
            const std::size_t row_start = history_html.find("<tr", pos);
            if (row_start == std::string::npos) break;
            const std::size_t row_end = history_html.find("</tr>", row_start);
            if (row_end == std::string::npos) break;
            const std::string row = history_html.substr(row_start, row_end - row_start);
            pos = row_end + 5;

            if (auto trade = detail::parse_history_attr_row(row, account_type)) {
                page.records.push_back(std::move(*trade));
                continue;
            }
            if (auto trade = detail::parse_trade_close_table_row(row, account_type)) {
                page.records.push_back(std::move(*trade));
            }
        }

        return page;
    }

    /// \brief Parses closed trade rows from the authenticated HTML page.
    /// \param content Raw authenticated HTML page.
    /// \param account_type Account type used for the request.
    /// \return Best-effort history records; financial result fields may be unknown.
    inline std::vector<TradeRecord> parse_trade_history_html_snapshot(
            const std::string& content,
            AccountType account_type) {
        return parse_trade_history_html_page(content, account_type).records;
    }

    /// \brief Intersects CSV financial history with HTML broker identifiers.
    /// \param csv_records Financially complete CSV records.
    /// \param html_records Best-effort HTML records with broker IDs.
    /// \return CSV records enriched with HTML data only when both sources match.
    inline std::vector<TradeRecord> merge_trade_history_csv_with_html(
            std::vector<TradeRecord> csv_records,
            const std::vector<TradeRecord>& html_records) {
        std::vector<TradeRecord> merged;
        std::vector<bool> html_used(html_records.size(), false);
        constexpr int64_t time_tolerance_ms = time_shield::MS_PER_5_SEC;
        constexpr double price_tolerance = 0.00001;

        for (auto& csv_record : csv_records) {
            bool matched = false;
            for (std::size_t i = 0; i < html_records.size(); ++i) {
                if (html_used[i]) continue;
                const auto& html_record = html_records[i];
                if (csv_record.option_id > 0 && html_record.option_id > 0) {
                    if (csv_record.option_id != html_record.option_id) {
                        continue;
                    }
                    html_used[i] = true;
                } else {
                    if (csv_record.symbol.empty() ||
                        html_record.symbol.empty() ||
                        csv_record.symbol != html_record.symbol ||
                        csv_record.open_date <= 0 ||
                        html_record.open_date <= 0 ||
                        std::llabs(csv_record.open_date - html_record.open_date) > time_tolerance_ms) {
                        continue;
                    }
                    if (csv_record.open_price > 0.0 &&
                        html_record.open_price > 0.0 &&
                        std::abs(csv_record.open_price - html_record.open_price) > price_tolerance) {
                        continue;
                    }
                    html_used[i] = true;
                }

                if (csv_record.option_id == 0) csv_record.option_id = html_record.option_id;
                if (csv_record.symbol.empty()) csv_record.symbol = html_record.symbol;
                if (csv_record.open_price == 0.0) csv_record.open_price = html_record.open_price;
                if (csv_record.close_price == 0.0) csv_record.close_price = html_record.close_price;
                if (csv_record.open_date == 0) csv_record.open_date = html_record.open_date;
                if (csv_record.close_date == 0) csv_record.close_date = html_record.close_date;
                if (csv_record.duration == 0) csv_record.duration = html_record.duration;
                if (csv_record.option_type == OptionType::UNKNOWN) csv_record.option_type = html_record.option_type;
                if (csv_record.order_type == OrderType::UNKNOWN) csv_record.order_type = html_record.order_type;
                matched = true;
                break;
            }
            if (matched) merged.push_back(std::move(csv_record));
        }

        return merged;
    }

    /// \brief Parses closed trades returned by /stat_trade_export.php CSV export.
    /// \param content Raw semicolon-separated export body.
    /// \param account_type Account type used for the export request.
    /// \return Parsed closed trade records; malformed rows are skipped.
    inline std::vector<TradeRecord> parse_trade_history_csv_export(
            const std::string& content,
            AccountType account_type) {
        std::vector<TradeRecord> records;
        std::istringstream stream(content);
        std::string line;
        bool first_line = true;

        while (std::getline(stream, line)) {
            line = utils::trim_copy(line);
            if (line.empty()) continue;

            auto fields = detail::split_semicolon_line(line);
            if (first_line) {
                first_line = false;
                if (!fields.empty() && detail::lower_ascii_copy(fields[0]).find("id") != std::string::npos) {
                    continue;
                }
            }
            if (fields.size() < 10) continue;

            TradeRecord record;
            if (auto option_id = utils::parse_i64_strict(fields[0]); option_id && *option_id > 0) {
                record.option_id = *option_id;
            }
            record.option_type = detail::parse_history_option_type(fields[1]);
            record.symbol = normalize_symbol_name(fields[2]);
            record.order_type = detail::parse_history_order_type(fields[3]);

            auto open_time = detail::parse_history_datetime_ms(fields[4]);
            auto close_time = detail::parse_history_datetime_ms(fields[5]);
            auto open_price = utils::parse_double_strict(fields[6]);
            auto close_price = utils::parse_double_strict(fields[7]);
            auto amount = detail::parse_history_money(fields[8]);
            auto gross_result = detail::parse_history_money(fields[9]);
            if (!open_time || !close_time || !open_price || !close_price || !amount || !gross_result) {
                continue;
            }

            record.open_date = *open_time;
            record.close_date = *close_time;
            if (record.close_date >= record.open_date) {
                record.duration = duration_sec_from_ms_delta(record.close_date - record.open_date);
            }
            record.open_price = *open_price;
            record.close_price = *close_price;
            record.amount = amount->amount;
            record.currency = amount->currency;
            record.account_type = account_type;
            record.platform_type = PlatformType::INTRADE_BAR;

            detail::apply_history_gross_result_to_record(record, gross_result->amount);
            detail::apply_intrade_bar_zero_spread(record);
            records.push_back(std::move(record));
        }

        return records;
    }
    /// \brief Parses active trades from the authenticated main page.
    /// \param content Raw HTML of the authenticated main page.
    /// \return Active trades found in the trade_active block.
    inline std::vector<ActiveTradeInfo> parse_active_trades_snapshot(const std::string& content) {
        std::vector<ActiveTradeInfo> trades;

        std::string active_html;
        const std::size_t active_id = content.find("id=\"trade_active\"");
        if (active_id == std::string::npos) {
            throw std::runtime_error("Authenticated active trades block not found.");
        }
        std::size_t block_start = content.rfind("<tbody", active_id);
        if (block_start == std::string::npos) block_start = active_id;
        std::size_t block_end = content.find("</tbody>", active_id);
        if (block_end == std::string::npos) block_end = content.size();
        active_html = content.substr(block_start, block_end - block_start);

        std::size_t pos = 0;
        for (;;) {
            const std::size_t row_start = active_html.find("<tr", pos);
            if (row_start == std::string::npos) break;
            if (row_start + 3 < active_html.size()) {
                const unsigned char after_tr = static_cast<unsigned char>(active_html[row_start + 3]);
                if (std::isspace(after_tr) == 0 &&
                    active_html[row_start + 3] != '>' &&
                    active_html[row_start + 3] != '/') {
                    pos = row_start + 3;
                    continue;
                }
            }

            const std::size_t row_end = active_html.find("</tr>", row_start);
            if (row_end == std::string::npos) break;
            const std::string row = active_html.substr(row_start, row_end - row_start);
            pos = row_end + 5;

            auto row_id = utils::extract_html_attr(row, "id");
            if (!row_id || row_id->rfind("trade_inv_", 0) != 0) continue;

            auto id = utils::parse_i64_attr(row, "data-id");
            if (!id || *id <= 0) continue;

            ActiveTradeInfo trade;
            trade.id = *id;
            if (auto symbol = utils::extract_html_attr(row, "data-option")) {
                trade.symbol = *symbol;
            }
            if (auto open_price = utils::parse_double_attr(row, "data-rate")) {
                trade.open_price = *open_price;
            }
            if (auto open_time = utils::parse_i64_attr(row, "data-timeopen")) {
                trade.open_time_ms = time_shield::sec_to_ms(*open_time);
            }
            if (auto status = utils::parse_int_attr(row, "data-status")) {
                trade.status = *status;
            }
            if (auto contract = utils::parse_int_attr(row, "data-contract")) {
                trade.contract = *contract;
            }
            if (auto close_time = detail::parse_active_trade_close_time_ms(row, trade.id)) {
                trade.close_time_ms = *close_time;
            }

            trades.push_back(std::move(trade));
        }

        return trades;
    }

    /// \brief Parses account settings switch response from the broker.
    /// \param content Raw HTTP response body.
    /// \param status_code HTTP status code.
    /// \param operation_name Human-readable settings operation name.
    /// \return Typed switch result with retry diagnostics on broker rejection.
    inline SettingsSwitchResult parse_settings_switch_response(
            const std::string& content,
            long status_code,
            const std::string& operation_name) {
        const std::string normalized = utils::trim_copy(content);
        if (normalized == "ok") {
            return SettingsSwitchResult::ok(SettingsSwitch{}, status_code);
        }
        if (normalized == "error") {
            return make_settings_switch_failure(
                SettingsSwitchFailureReason::BROKER_REJECTED,
                "Broker rejected " + operation_name +
                    " switch; active trades may block settings changes.",
                status_code,
                content);
        }
        return make_settings_switch_failure(
            SettingsSwitchFailureReason::UNEXPECTED_RESPONSE,
            "Unexpected " + operation_name + " switch response.",
            status_code,
            content);
    }

    /// \brief Extracts user_id and user_hash from a cookies string.
    /// \param cookies The input string containing cookies.
    /// \param user_id [out] The extracted user_id.
    /// \param user_hash [out] The extracted user_hash.
    /// \return True if both user_id and user_hash are found, otherwise false.
    inline bool parse_cookies(const std::string& cookies, std::string& user_id, std::string& user_hash) {
        std::unordered_map<std::string, std::string> cookie_map;
        std::istringstream cookie_stream(cookies);
        std::string cookie;

        // Parse the cookies string
        while (std::getline(cookie_stream, cookie, ';')) {
            size_t pos = cookie.find('=');
            if (pos != std::string::npos) {
                std::string key = cookie.substr(0, pos);
                std::string value = cookie.substr(pos + 1);

                // Trim leading and trailing whitespace
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);

                cookie_map[key] = value;
            }
        }

        // Extract user_id and user_hash
        auto id_it = cookie_map.find("user_id");
        auto hash_it = cookie_map.find("user_hash");

        if (id_it != cookie_map.end() && hash_it != cookie_map.end()) {
            user_id = id_it->second;
            user_hash = hash_it->second;
            return true;
        }

        return false;
    }

    inline std::optional<std::tuple<std::string, std::string>> parse_cookies(const std::string& cookies) {
        std::unordered_map<std::string, std::string> cookie_map;
        std::istringstream cookie_stream(cookies);
        std::string cookie;

        // Parse the cookies string
        while (std::getline(cookie_stream, cookie, ';')) {
            size_t pos = cookie.find('=');
            if (pos != std::string::npos) {
                std::string key = cookie.substr(0, pos);
                std::string value = cookie.substr(pos + 1);

                // Trim leading and trailing whitespace
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);

                cookie_map[key] = value;
            }
        }

        // Extract user_id and user_hash
        auto id_it = cookie_map.find("user_id");
        auto hash_it = cookie_map.find("user_hash");

        if (id_it != cookie_map.end() && hash_it != cookie_map.end()) {
            return std::make_tuple(id_it->second, hash_it->second);
        }

        return std::nullopt;
    }

    /// \brief Parses the response content of a trade open request.
    /// \param content The server response content as a string.
    /// \param request A shared pointer to the TradeRequest containing request details.
    /// \param result A shared pointer to the TradeResult where the parsed result will be stored.
    /// \return True if the response was successfully parsed, false otherwise.
    inline bool parse_execute_trade(
            const std::string& content,
            std::shared_ptr<TradeRequest> request,
            std::shared_ptr<TradeResult> result) {
        if (!result || !request) {
            LOGIT_ERROR("TradeResult or TradeRequest is null.");
            return false;
        }
        const int64_t timestamp = OPTIONX_TIMESTAMP_MS;
        try {
            if (utils::is_blank_response(content)) {
                const std::string error_desc = detail::empty_trade_open_response_error();
                detail::log_trade_open_response(content, error_desc);
                result->trade_state = result->live_state = TradeState::OPEN_ERROR;
                result->error_code = TradeErrorCode::PARSING_ERROR;
                result->error_desc = error_desc;
                result->delay = timestamp - result->send_date;
                result->ping = result->delay / 2;
                result->open_date = timestamp;
                result->close_date = request->option_type == OptionType::SPRINT
                                     ? timestamp + time_shield::sec_to_ms(request->duration)
                                     : time_shield::sec_to_ms(request->expiry_time);
                return false;
            }

            // Check for error indicators in the response
            if (content.find("error") != std::string::npos) {
#               ifdef OPTIONX_LOG_UNIQUE_FILE_INDEX
                const int log_index = OPTIONX_LOG_UNIQUE_FILE_INDEX;
                LOGIT_STREAM_ERROR_TO(log_index) << content;
                LOGIT_PRINT_ERROR(
                    "Trade open failed. Response contains 'error'. Content log was written to file: ",
                    LOGIT_GET_LAST_FILE_NAME(log_index)
                );
#               else
                LOGIT_PRINT_ERROR("Trade open failed. Response contains 'error'.");
#               endif
                result->trade_state = result->live_state = TradeState::OPEN_ERROR;
                result->error_code = TradeErrorCode::PARSING_ERROR;
                result->error_desc = "Trade open failed. Response contains 'error'.";
                result->delay = timestamp - result->send_date;
                result->ping = result->delay / 2;
                result->open_date = timestamp;
                result->close_date = request->option_type == OptionType::SPRINT
                                     ? timestamp + time_shield::sec_to_ms(request->duration)
                                     : time_shield::sec_to_ms(request->expiry_time);
                return false;
            }

            if (content.find("alert") != std::string::npos) {
#               ifdef OPTIONX_LOG_UNIQUE_FILE_INDEX
                const int log_index = OPTIONX_LOG_UNIQUE_FILE_INDEX;
                LOGIT_STREAM_ERROR_TO(log_index) << content;
                LOGIT_PRINT_ERROR(
                    "Trade open failed. Response contains 'alert'. Content log was written to file: ",
                    LOGIT_GET_LAST_FILE_NAME(log_index)
                );
#               else
                LOGIT_PRINT_ERROR("Trade open failed. Response contains 'alert'.");
#               endif
                result->trade_state = result->live_state = TradeState::OPEN_ERROR;
                result->error_code = TradeErrorCode::PARSING_ERROR;
                result->error_desc = "Trade open failed. Response contains 'alert'.";
                result->delay = timestamp - result->send_date;
                result->ping = result->delay / 2;
                result->open_date = timestamp;
                result->close_date = request->option_type == OptionType::SPRINT
                                     ? timestamp + time_shield::sec_to_ms(request->duration)
                                     : time_shield::sec_to_ms(request->expiry_time);
                return false;
            }

            // Extract data fields
            std::string str_data_id, str_data_timeopen, str_data_rate;

            if (utils::extract_between(content, "data-id=\"", "\"", str_data_id) == std::string::npos) {
                detail::log_trade_open_response(content, "Failed to extract id from trade open response.");
                result->trade_state = result->live_state = TradeState::OPEN_ERROR;
                result->error_code = TradeErrorCode::PARSING_ERROR;
                result->error_desc = "Failed to extract id.";
                result->delay = timestamp - result->send_date;
                result->ping = result->delay / 2;
                result->open_date = timestamp;
                result->close_date = request->option_type == OptionType::SPRINT
                                     ? timestamp + time_shield::sec_to_ms(request->duration)
                                     : time_shield::sec_to_ms(request->expiry_time);
                return false;
            }
            result->option_id = std::stoull(str_data_id);

            if (utils::extract_between(content, "data-timeopen=\"", "\"", str_data_timeopen) == std::string::npos) {
                detail::log_trade_open_response(content, "Failed to extract timeopen from trade open response.");
                result->trade_state = result->live_state = TradeState::OPEN_ERROR;
                result->error_code = TradeErrorCode::PARSING_ERROR;
                result->error_desc = "Failed to extract timeopen.";
                result->delay = timestamp - result->send_date;
                result->ping = result->delay / 2;
                result->open_date = timestamp;
                result->close_date = request->option_type == OptionType::SPRINT
                                     ? timestamp + time_shield::sec_to_ms(request->duration)
                                     : time_shield::sec_to_ms(request->expiry_time);
                return false;
            }
            result->open_date = time_shield::sec_to_ms(std::stoull(str_data_timeopen));
            result->delay = timestamp - result->open_date;
            result->ping = result->delay / 2;

            if (utils::extract_between(content, "data-rate=\"", "\"", str_data_rate) == std::string::npos) {
                detail::log_trade_open_response(content, "Failed to extract rate from trade open response.");
                result->trade_state = result->live_state = TradeState::OPEN_ERROR;
                result->error_code = TradeErrorCode::PARSING_ERROR;
                result->error_desc = "Failed to extract rate.";
                result->open_date = timestamp;
                result->close_date = request->option_type == OptionType::SPRINT
                 ? timestamp + time_shield::sec_to_ms(request->duration)
                 : time_shield::sec_to_ms(request->expiry_time);
                return false;
            }

            // Parse extracted values
            result->open_price = result->close_price = std::stod(str_data_rate);
            result->trade_state = result->live_state = TradeState::OPEN_SUCCESS;
            result->error_code = TradeErrorCode::SUCCESS;
            return true;

        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("Exception while parsing trade response: ", ex.what());
            result->trade_state = result->live_state = TradeState::OPEN_ERROR;
            result->error_code = TradeErrorCode::PARSING_ERROR;
            result->error_desc = "Exception while parsing trade response: " + std::string(ex.what());
            result->delay = timestamp - result->open_date;
            result->ping = result->delay / 2;
            result->open_date = timestamp;
            result->close_date = request->option_type == OptionType::SPRINT
             ? timestamp + time_shield::sec_to_ms(request->duration)
             : time_shield::sec_to_ms(request->expiry_time);
        }
        return false;
    }

    /// \brief Parses the response content of a trade open request.
    /// \param content The server response content as a string.
    /// \param status_code The HTTP status code returned by the server.
    /// \param result_callback A callback function that receives the parsing result:
    ///        - success: True if the response was successfully parsed, false otherwise.
    ///        - status_code: The HTTP status code returned by the server.
    ///        - option_id: The option ID extracted from the response.
    ///        - open_date: The trade open date as a timestamp.
    ///        - open_price: The trade open price.
    ///        - error_desc: A description of the error if parsing fails.
    inline void parse_execute_trade(
            const std::string& content,
            long status_code,
            std::function<void(
                bool success,
                long status_code,
                int64_t option_id,
                int64_t open_date,
                double open_price,
                const std::string& error_desc)> result_callback) {
        try {
            if (utils::is_blank_response(content)) {
                const std::string error_desc = detail::empty_trade_open_response_error();
                detail::log_trade_open_response(content, error_desc);
                result_callback(false, status_code, 0, 0, 0, error_desc);
                return;
            }

            // Check for error indicators in the response
            if (content.find("error") != std::string::npos) {
#               ifdef OPTIONX_LOG_UNIQUE_FILE_INDEX
                const int log_index = OPTIONX_LOG_UNIQUE_FILE_INDEX;
                LOGIT_STREAM_ERROR_TO(log_index) << content;
                LOGIT_PRINT_ERROR(
                    "Trade open failed. Response contains 'error'. Content log was written to file: ",
                    LOGIT_GET_LAST_FILE_NAME(log_index)
                );
#               else
                LOGIT_PRINT_ERROR("Trade open failed. Response contains 'error'.");
#               endif
                result_callback(false, status_code,
                    0, 0, 0, "Trade open failed. Response contains 'error'.");
                return;
            }

            if (content.find("alert") != std::string::npos) {
#               ifdef OPTIONX_LOG_UNIQUE_FILE_INDEX
                const int log_index = OPTIONX_LOG_UNIQUE_FILE_INDEX;
                LOGIT_STREAM_ERROR_TO(log_index) << content;
                LOGIT_PRINT_ERROR(
                    "Trade open failed. Response contains 'alert'. Content log was written to file: ",
                    LOGIT_GET_LAST_FILE_NAME(log_index)
                );
#               else
                LOGIT_PRINT_ERROR("Trade open failed. Response contains 'alert'.");
#               endif
                result_callback(false, status_code,
                    0, 0, 0, "Trade open failed. Response contains 'alert'.");
                return;
            }

            // Extract data fields
            std::string str_data_id, str_data_timeopen, str_data_rate;

            if (utils::extract_between(content, "data-id=\"", "\"", str_data_id) == std::string::npos) {
                detail::log_trade_open_response(content, "Failed to extract id from trade open response.");
                result_callback(false, status_code,
                    0, 0, 0, "Failed to extract id.");
                return;
            }

            if (utils::extract_between(content, "data-timeopen=\"", "\"", str_data_timeopen) == std::string::npos) {
                detail::log_trade_open_response(content, "Failed to extract timeopen from trade open response.");
                result_callback(false, status_code,
                    0, 0, 0, "Failed to extract timeopen.");
                return;
            }

            if (utils::extract_between(content, "data-rate=\"", "\"", str_data_rate) == std::string::npos) {
                detail::log_trade_open_response(content, "Failed to extract rate from trade open response.");
                result_callback(false, status_code,
                    0, 0, 0, "Failed to extract rate.");
                return;
            }

            result_callback(
                true,
                status_code,
                std::stoll(str_data_id),
                time_shield::sec_to_ms(std::stoll(str_data_timeopen)),
                std::stod(str_data_rate),
                std::string());
        } catch (const std::exception& ex) {
            LOGIT_ERROR("Exception while parsing trade response: ", ex.what());
            result_callback(false, status_code,
                0, 0, 0, "Exception while parsing trade response: " + std::string(ex.what()));
        }
    }

    namespace detail {

        inline double read_json_double(
                const nlohmann::json& value,
                const char* field_name) {
            if (value.is_number()) return value.get<double>();
            if (value.is_string()) return std::stod(value.get<std::string>());
            throw std::runtime_error(
                std::string("Expected numeric value for ") + field_name + ".");
        }

        inline std::int64_t read_json_int64(
                const nlohmann::json& value,
                const char* field_name) {
            if (value.is_number_integer()) return value.get<std::int64_t>();
            if (value.is_number()) return static_cast<std::int64_t>(value.get<double>());
            if (value.is_string()) return std::stoll(value.get<std::string>());
            throw std::runtime_error(
                std::string("Expected integer value for ") + field_name + ".");
        }

        inline double select_fx_history_price(
                double bid,
                double ask,
                BarPriceSource source) {
            switch (source) {
            case BarPriceSource::BID:
                return bid;
            case BarPriceSource::ASK:
                return ask;
            case BarPriceSource::MID:
                return (bid + ask) / 2.0;
            case BarPriceSource::UNKNOWN:
            case BarPriceSource::LAST:
            default:
                throw std::runtime_error("Unsupported Intrade FX bar price source.");
            }
        }

        inline BarSequence make_empty_bar_sequence(
                const BarHistoryRequest& request,
                BarPriceSource actual_source) {
            BarSequence sequence;
            sequence.symbol = normalize_symbol_name(request.symbol);
            sequence.provider = to_str(PlatformType::INTRADE_BAR);
            sequence.timeframe = request.timeframe;
            sequence.price_digits = price_digits_for_symbol(sequence.symbol);
            sequence.volume_digits = 0;
            sequence.price_source = actual_source;
            return sequence;
        }

    } // namespace detail

    /// \brief Parses the Intrade `/fxhis` response into a normalized bar sequence.
    /// \param content Raw JSON response body.
    /// \param request Original bar history request.
    /// \return Parsed bar sequence using the requested BID/ASK/MID price stream.
    /// \throws std::runtime_error When the payload is malformed or broker rejected the request.
    inline BarSequence parse_fxhis_bar_history(
            const std::string& content,
            const BarHistoryRequest& request) {
        if (request.price_source == BarPriceSource::UNKNOWN ||
            request.price_source == BarPriceSource::LAST) {
            throw std::runtime_error("Intrade FX history supports only BID, ASK, or MID bars.");
        }

        const auto j = nlohmann::json::parse(content);
        if (j.contains("response") && j["response"].is_object()) {
            const auto& response = j["response"];
            const bool executed = response.value("executed", false);
            const std::string error = response.value("error", std::string());
            if (!executed || !error.empty()) {
                throw std::runtime_error(
                    error.empty()
                        ? "Intrade FX history request was rejected."
                        : "Intrade FX history request was rejected: " + error);
            }
        }

        if (!j.contains("candles") || !j["candles"].is_array()) {
            throw std::runtime_error("Intrade FX history response does not contain a candles array.");
        }

        auto sequence = detail::make_empty_bar_sequence(request, request.price_source);
        const auto& candles = j["candles"];
        sequence.bars.reserve(candles.size());
        const auto price_type = market_price_type_from_bar_price_source(request.price_source);

        for (const auto& item : candles) {
            if (!item.is_array() || item.size() < 10) {
                throw std::runtime_error("Malformed Intrade FX history candle.");
            }

            const auto ts = detail::read_json_int64(item.at(0), "time");
            const auto bid_open = detail::read_json_double(item.at(1), "bid_open");
            const auto bid_close = detail::read_json_double(item.at(2), "bid_close");
            const auto bid_high = detail::read_json_double(item.at(3), "bid_high");
            const auto bid_low = detail::read_json_double(item.at(4), "bid_low");
            const auto ask_open = detail::read_json_double(item.at(5), "ask_open");
            const auto ask_close = detail::read_json_double(item.at(6), "ask_close");
            const auto ask_high = detail::read_json_double(item.at(7), "ask_high");
            const auto ask_low = detail::read_json_double(item.at(8), "ask_low");
            const auto volume = detail::read_json_double(item.at(9), "volume");

            Bar bar(
                detail::select_fx_history_price(bid_open, ask_open, request.price_source),
                detail::select_fx_history_price(bid_high, ask_high, request.price_source),
                detail::select_fx_history_price(bid_low, ask_low, request.price_source),
                detail::select_fx_history_price(bid_close, ask_close, request.price_source),
                volume,
                static_cast<std::uint64_t>(time_shield::sec_to_ms(ts)));
            bar.set_flag(MarketDataFlags::HISTORICAL);
            bar.set_flag(MarketDataFlags::FINALIZED);
            bar.set_price_type(price_type);
            sequence.bars.push_back(std::move(bar));
        }

        return sequence;
    }

    /// \brief Parses Binance klines into BTCUSDT historical bars.
    /// \param content Raw Binance JSON array.
    /// \param request Original bar history request.
    /// \return Parsed bar sequence using LAST trade prices.
    /// \throws std::runtime_error When the payload is malformed.
    inline BarSequence parse_binance_klines_bar_history(
            const std::string& content,
            const BarHistoryRequest& request) {
        if (request.price_source == BarPriceSource::BID ||
            request.price_source == BarPriceSource::ASK) {
            throw std::runtime_error("Binance kline history does not provide bid/ask bars.");
        }

        const auto j = nlohmann::json::parse(content);
        if (!j.is_array()) {
            throw std::runtime_error("Binance kline response is not an array.");
        }

        auto sequence = detail::make_empty_bar_sequence(request, BarPriceSource::LAST);
        sequence.bars.reserve(j.size());

        for (const auto& item : j) {
            if (!item.is_array() || item.size() < 6) {
                throw std::runtime_error("Malformed Binance kline entry.");
            }

            Bar bar(
                detail::read_json_double(item.at(1), "open"),
                detail::read_json_double(item.at(2), "high"),
                detail::read_json_double(item.at(3), "low"),
                detail::read_json_double(item.at(4), "close"),
                detail::read_json_double(item.at(5), "volume"),
                static_cast<std::uint64_t>(detail::read_json_int64(item.at(0), "open_time")));
            bar.set_flag(MarketDataFlags::HISTORICAL);
            bar.set_flag(MarketDataFlags::FINALIZED);
            bar.set_price_type(MarketPriceType::LAST);
            sequence.bars.push_back(std::move(bar));
        }

        return sequence;
    }

    /// \brief Parses the Intrade `/price_now` response into source tick batches.
    /// \param content JSON object keyed by broker symbol.
    /// \param received_ms Local receipt timestamp in milliseconds.
    /// \return Tick batches grouped by normalized symbol and precision metadata.
    inline std::vector<events::TickUpdateBatch> parse_price_snapshot_response(
            const std::string& content,
            std::uint64_t received_ms) {
        const auto j = nlohmann::json::parse(content);
        std::vector<events::TickUpdateBatch> batches;
        batches.reserve(j.size());

        for (const auto& el : j.items()) {
            const auto symbol = normalize_symbol_name(el.key());
            Tick tick;
            tick.ask = el.value().at("ask").get<double>();
            tick.bid = el.value().at("bid").get<double>();
            tick.time_ms = time_shield::sec_to_ms(
                static_cast<std::uint64_t>(
                    detail::read_json_int64(el.value().at("Updates"), "Updates")));
            tick.received_ms = received_ms;
            tick.set_flag(MarketDataFlags::INITIALIZED);
            mark_live_payload(tick.flags);
            batches.push_back(events::PriceUpdateEvent::make_tick_batch(
                std::move(tick),
                symbol,
                to_str(PlatformType::INTRADE_BAR),
                price_digits_for_symbol(symbol),
                0));
        }

        return batches;
    }

    /// \brief Parses a BTCUSDT tick message into a source batch.
    /// \param message The JSON-formatted string containing the tick data.
    /// \param batch Destination batch to fill.
    /// \return true if parsing is successful and the symbol matches BTCUSDT; false otherwise.
    inline bool parse_btcusdt_tick(
            const std::string& message,
            events::TickUpdateBatch& batch) {
        auto j = nlohmann::json::parse(message);

        if (j.contains("data")) {
            const auto& j_data = j["data"];
            if (j_data.value("s", "") != "BTCUSDT") return false;
            if (!j_data.contains("T")) return false;

            Tick tick;
            tick.last = std::stod(j_data.value("p", "0.0"));
            tick.volume = std::stod(j_data.value("q", "0.0"));
            tick.time_ms = static_cast<std::uint64_t>(
                detail::read_json_int64(j_data.at("T"), "T"));
            tick.received_ms = OPTIONX_TIMESTAMP_MS;
            tick.set_flag(TickUpdateFlags::LAST_UPDATED);
            tick.set_flag(TickUpdateFlags::VOLUME_UPDATED);
            tick.set_flag(MarketDataFlags::INITIALIZED);
            mark_live_payload(tick.flags);
            batch = events::PriceUpdateEvent::make_tick_batch(
                std::move(tick),
                "BTCUSDT",
                to_str(PlatformType::INTRADE_BAR),
                2,
                5);
            return true;
        }

        return false;
    }

    /// \brief Parses an Intrade `/fxconnect` tick message.
    /// \param message JSON-formatted message received from the FX websocket.
    /// \param batch Destination source batch to fill.
    /// \return True when the payload contains a supported FX symbol and bid/ask prices.
    inline bool parse_fxconnect_tick(
            const std::string& message,
            events::TickUpdateBatch& batch) {
        const auto j = nlohmann::json::parse(message);

        if (!j.contains("symbol") || !j.contains("ask") || !j.contains("bid")) {
            return false;
        }

        const auto normalized_symbol = normalize_symbol_name(j.at("symbol").get<std::string>());
        if (!is_fxconnect_supported_symbol(normalized_symbol)) {
            return false;
        }

        Tick tick;
        tick.ask = detail::read_json_double(j.at("ask"), "ask");
        tick.bid = detail::read_json_double(j.at("bid"), "bid");
        tick.time_ms = j.contains("Updates")
            ? static_cast<std::uint64_t>(
                detail::read_json_int64(j.at("Updates"), "Updates")) * time_shield::MS_PER_SEC
            : 0;
        tick.received_ms = OPTIONX_TIMESTAMP_MS;
        tick.set_flag(TickUpdateFlags::ASK_UPDATED);
        tick.set_flag(TickUpdateFlags::BID_UPDATED);
        tick.set_flag(MarketDataFlags::INITIALIZED);
        mark_live_payload(tick.flags);
        batch = events::PriceUpdateEvent::make_tick_batch(
            std::move(tick),
            normalized_symbol,
            to_str(PlatformType::INTRADE_BAR),
            price_digits_for_symbol(normalized_symbol),
            0);
        return true;
    }

}

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_HTTP_PARSERS_HPP_INCLUDED
