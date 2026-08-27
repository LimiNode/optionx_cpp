#pragma once
#ifndef OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_RECORD_FILTER_MATCHER_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_RECORD_FILTER_MATCHER_HPP_INCLUDED

/// \file TradeRecordFilterMatcher.hpp
/// \brief Applies TradeRecordFilter predicates to individual TradeRecord objects.

#include <cstdint>
#include <cstddef>
#include <chrono>

#include <time_shield.hpp>

#include <optionx_cpp/data/trading.hpp>

namespace optionx::storage {

    /// \class TradeRecordFilterMatcher
    /// \brief Static helper for matching TradeRecord objects against a query or filter.
    class TradeRecordFilterMatcher {
    public:
        /// \brief Matches a record against a full query (time range + filter).
        /// \param record Trade record to test.
        /// \param query Query containing time range, time field, range mode, and filter.
        /// \return True if the record satisfies both time range and all active filter predicates.
        static bool match(const TradeRecord& record, const TradeRecordQuery& query) noexcept {
            const auto ts = optionx::select_timestamp_ms(record, query.time_field);

            // Time range check
            if (query.range_mode != optionx::TimeRangeMode::NONE) {
                if (query.start_ms != 0 || query.stop_ms != 0) {
                    if (ts < query.start_ms) return false;
                    if (query.range_mode == optionx::TimeRangeMode::CLOSED) {
                        if (ts > query.stop_ms) return false;
                    } else {
                        if (ts >= query.stop_ms) return false;
                    }
                }
            }

            return match_filter(record, query.filter, query.time_zone, ts);
        }

        /// \brief Matches a record against a TradeRecordFilter with optional timezone.
        /// \param record Trade record to test.
        /// \param filter Filter predicates.
        /// \param time_zone Local-time context for component filters.
        /// \param selected_ms Pre-computed selected timestamp (0 = derive from record).
        /// \return True if the record passes all active filter predicates.
        static bool match_filter(
                const TradeRecord& record,
                const TradeRecordFilter& filter,
                const optionx::TradeTimeZone& time_zone = optionx::TradeTimeZone::utc(),
                std::int64_t selected_ms = 0) noexcept {

            if (!filter.platforms.match(record.platform_type)) return false;
            if (!filter.accounts.match(record.account_type)) return false;
            if (!filter.currencies.match(record.currency)) return false;
            if (!filter.option_types.match(record.option_type)) return false;
            if (!filter.order_types.match(record.order_type)) return false;
            if (!filter.trade_states.match(record.trade_state)) return false;
            if (!filter.error_codes.match(record.error_code)) return false;

            if (!filter.symbols.match(record.symbol)) return false;
            if (!filter.signals.match(record.signal_name)) return false;
            if (!filter.user_data.match(record.user_data)) return false;
            if (!filter.mm_group_names.match(record.mm_group_name)) return false;

            if (!filter.account_ids.match(record.account_id)) return false;
            if (!filter.option_ids.match(record.option_id)) return false;
            if (!filter.trade_ids.match(record.trade_id)) return false;
            if (!filter.signal_ids.match(record.signal_id)) return false;
            if (!filter.unique_ids.match(record.unique_id)) return false;

            if (!filter.durations.match(record.duration)) return false;
            if (!filter.mm_steps.match(record.mm_step)) return false;

            if (filter.only_terminal && !optionx::is_terminal_trade_state(record.trade_state)) return false;
            if (filter.only_non_terminal && optionx::is_terminal_trade_state(record.trade_state)) return false;

            if (filter.only_first_mm_step && record.mm_step != 0) return false;

            // Scalar range checks
            if (filter.min_amount && record.amount < *filter.min_amount) return false;
            if (filter.max_amount && record.amount > *filter.max_amount) return false;
            if (filter.min_payout && record.payout < *filter.min_payout) return false;
            if (filter.max_payout && record.payout > *filter.max_payout) return false;
            if (filter.min_profit && record.profit < *filter.min_profit) return false;
            if (filter.max_profit && record.profit > *filter.max_profit) return false;
            if (filter.min_balance && record.close_balance < *filter.min_balance) return false;
            if (filter.max_balance && record.close_balance > *filter.max_balance) return false;

            if (filter.min_ping && record.ping < *filter.min_ping) return false;
            if (filter.max_ping && record.ping > *filter.max_ping) return false;
            if (filter.min_delay && record.delay < *filter.min_delay) return false;
            if (filter.max_delay && record.delay > *filter.max_delay) return false;

            // Local-time component filters
            if (selected_ms == 0) {
                selected_ms = optionx::select_timestamp_ms(record, optionx::TradeRecordTimeField::AUTO);
            }
            if (selected_ms > 0) {
                const auto local_ms = time_zone.to_local_ms(selected_ms);
                const auto sec = time_shield::ms_to_sec<time_shield::ts_t>(local_ms);
                const auto dt = time_shield::to_date_time<time_shield::DateTimeStruct>(sec);

                const auto hour = static_cast<std::uint32_t>(dt.hour);
                const auto weekday = static_cast<std::uint32_t>(time_shield::weekday_of_ts(sec));
                const auto month_day = static_cast<std::uint32_t>(dt.day);
                const auto month = static_cast<std::uint32_t>(dt.mon);
                const auto second_of_day = static_cast<std::int64_t>(
                    dt.hour * 3600 + dt.min * 60 + dt.sec);

                if (!filter.hours.empty() && !filter.hours.match(hour)) return false;
                if (!filter.weekdays.empty() && !filter.weekdays.match(weekday)) return false;
                if (!filter.month_days.empty() && !filter.month_days.match(month_day)) return false;
                if (!filter.months.empty() && !filter.months.match(month)) return false;

                if (filter.use_second_of_day) {
                    if (!optionx::match_second_of_day(second_of_day,
                            filter.start_second_of_day, filter.stop_second_of_day)) {
                        return false;
                    }
                }
            }

            return true;
        }
    };

} // namespace optionx::storage

#endif // OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_RECORD_FILTER_MATCHER_HPP_INCLUDED
