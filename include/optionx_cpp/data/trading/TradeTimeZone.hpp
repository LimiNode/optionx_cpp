#pragma once
#ifndef OPTIONX_HEADER_DATA_TRADING_TRADE_TIME_ZONE_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TRADING_TRADE_TIME_ZONE_HPP_INCLUDED

/// \file TradeTimeZone.hpp
/// \brief Defines a local-time context for trade queries and statistics.

#include <cstdint>
#include <limits>

#include <time_shield/core/constants.hpp>
#include <time_shield/conversions/date_time_conversions.hpp>
#include <time_shield/core/enums.hpp>
#include <time_shield/timezone/time_zone_conversions.hpp>
#include <time_shield/core/types.hpp>

namespace optionx {

    namespace detail {

        inline bool checked_add_int64(
            std::int64_t lhs,
            std::int64_t rhs,
            std::int64_t& result) noexcept {
            const auto max_value = (std::numeric_limits<std::int64_t>::max)();
            const auto min_value = (std::numeric_limits<std::int64_t>::min)();

            if (rhs > 0) {
                if (lhs > max_value - rhs) {
                    return false;
                }
            } else if (rhs < 0) {
                if (rhs == min_value) {
                    if (lhs < 0) {
                        return false;
                    }
                } else if (lhs < min_value - rhs) {
                    return false;
                }
            }

            result = lhs + rhs;
            return true;
        }

        inline bool checked_subtract_int64(
            std::int64_t lhs,
            std::int64_t rhs,
            std::int64_t& result) noexcept {
            const auto max_value = (std::numeric_limits<std::int64_t>::max)();
            const auto min_value = (std::numeric_limits<std::int64_t>::min)();

            if (rhs > 0) {
                if (lhs < min_value + rhs) {
                    return false;
                }
            } else if (rhs < 0) {
                if (rhs == min_value) {
                    if (lhs >= 0) {
                        return false;
                    }
                } else if (lhs > max_value + rhs) {
                    return false;
                }
            }

            result = lhs - rhs;
            return true;
        }

        inline bool checked_seconds_to_ms(
            std::int64_t seconds,
            std::int64_t& milliseconds) noexcept {
            constexpr auto multiplier = time_shield::MS_PER_SEC;
            const auto max_value = (std::numeric_limits<std::int64_t>::max)();
            const auto min_value = (std::numeric_limits<std::int64_t>::min)();

            if (seconds > max_value / multiplier ||
                seconds < min_value / multiplier) {
                return false;
            }

            milliseconds = seconds * multiplier;
            return true;
        }

        inline bool is_time_shield_timestamp(std::int64_t timestamp_ms) noexcept {
            return timestamp_ms >= time_shield::MIN_TIMESTAMP_MS &&
                   timestamp_ms <= time_shield::MAX_TIMESTAMP_MS;
        }

        inline bool checked_start_of_period_ms(
            std::int64_t timestamp_ms,
            std::int64_t period_ms,
            std::int64_t& period_start_ms) noexcept {
            auto remainder_ms = timestamp_ms % period_ms;
            if (remainder_ms < 0) {
                remainder_ms += period_ms;
            }
            return checked_subtract_int64(
                timestamp_ms,
                remainder_ms,
                period_start_ms);
        }

    } // namespace detail

    /// \enum TradeTimeZoneMode
    /// \brief Selects whether local-time calculations use a fixed offset or a named zone.
    enum class TradeTimeZoneMode {
        FIXED_OFFSET, ///< Fixed UTC offset in seconds.
        NAMED_ZONE    ///< time-shield named zone with historical/DST offset resolution.
    };

    /// \class TradeTimeZone
    /// \brief Time-zone context used by local-time filters, buckets and day queries.
    ///
    /// Fixed-offset mode preserves the old `time_zone_sec` behavior. Named-zone
    /// mode delegates offset resolution to time-shield and therefore accounts for
    /// DST transitions supported by `time_shield::TimeZone`.
    ///
    /// Conversion helpers are total functions: if an unsupported named-zone
    /// conversion cannot be resolved, the context falls back to UTC/identity
    /// semantics instead of pushing error handling into filters, statistics and
    /// storage queries.
    class TradeTimeZone {
    public:
        /// \brief Creates UTC fixed-offset context.
        static TradeTimeZone utc() noexcept {
            return fixed_offset(0);
        }

        /// \brief Creates fixed-offset context.
        /// \param offset_seconds UTC offset in seconds.
        static TradeTimeZone fixed_offset(std::int64_t offset_seconds) noexcept {
            TradeTimeZone result;
            result.m_mode = TradeTimeZoneMode::FIXED_OFFSET;
            result.m_offset_sec = offset_seconds;
            result.m_zone = time_shield::UTC;
            return result;
        }

        /// \brief Creates named-zone context.
        /// \param value time-shield zone value. UNKNOWN keeps UTC conversion
        ///        fallback but remains detectable as an unsupported configuration.
        static TradeTimeZone named(time_shield::TimeZone value) noexcept {
            if (value == time_shield::UNKNOWN) {
                auto result = utc();
                result.m_named_zone_supported = false;
                return result;
            }

            TradeTimeZone result;
            result.m_mode = TradeTimeZoneMode::NAMED_ZONE;
            result.m_offset_sec = 0;
            result.m_zone = value;
            return result;
        }

        /// \brief Returns the stored timezone mode.
        TradeTimeZoneMode mode() const noexcept {
            return m_mode;
        }

        /// \brief Returns the fixed UTC offset in seconds, or 0 for named zones.
        std::int64_t offset_sec() const noexcept {
            return m_offset_sec;
        }

        /// \brief Returns the named zone, or UTC for fixed-offset mode.
        time_shield::TimeZone zone() const noexcept {
            return m_zone;
        }

        /// \brief Returns true when this context uses time-shield named-zone rules.
        bool is_named_zone() const noexcept {
            return m_mode == TradeTimeZoneMode::NAMED_ZONE &&
                   m_zone != time_shield::UNKNOWN;
        }

        /// \brief Resolves UTC offset for the provided UTC timestamp.
        /// \param utc_ms UTC timestamp in milliseconds.
        /// \return Resolved offset in seconds. Unsupported named-zone resolution
        ///         falls back to zero, i.e. UTC.
        std::int64_t offset_at_utc_ms(std::int64_t utc_ms) const noexcept {
            std::int64_t resolved = 0;
            return try_offset_at_utc_ms(utc_ms, resolved) ? resolved : 0;
        }

        /// \brief Tries to convert UTC milliseconds to local civil milliseconds.
        /// \param utc_ms UTC timestamp in milliseconds.
        /// \param local_ms Receives the converted local timestamp on success.
        /// \return False when a requested named zone cannot be resolved.
        bool try_to_local_ms(
            std::int64_t utc_ms,
            std::int64_t& local_ms) const noexcept {
            if (!m_named_zone_supported) {
                return false;
            }
            if (!is_named_zone()) {
                std::int64_t offset_ms = 0;
                return detail::checked_seconds_to_ms(m_offset_sec, offset_ms) &&
                       detail::checked_add_int64(utc_ms, offset_ms, local_ms);
            }

            std::int64_t resolved = 0;
            if (!try_offset_at_utc_ms(utc_ms, resolved)) {
                return false;
            }

            std::int64_t offset_ms = 0;
            return detail::checked_seconds_to_ms(
                       resolved,
                       offset_ms) &&
                   detail::checked_add_int64(utc_ms, offset_ms, local_ms);
        }

        /// \brief Converts UTC milliseconds to this context's local civil milliseconds.
        std::int64_t to_local_ms(std::int64_t utc_ms) const noexcept {
            std::int64_t local_ms = utc_ms;
            return try_to_local_ms(utc_ms, local_ms) ? local_ms : utc_ms;
        }

        /// \brief Converts local civil milliseconds in this context back to UTC.
        /// \details Unsupported named-zone conversions and checked-arithmetic
        ///          failures fall back to treating the supplied civil timestamp
        ///          as UTC.
        std::int64_t to_utc_ms(std::int64_t local_ms) const noexcept {
            if (!is_named_zone()) {
                std::int64_t offset_ms = 0;
                std::int64_t utc_ms = local_ms;
                return detail::checked_seconds_to_ms(m_offset_sec, offset_ms) &&
                               detail::checked_subtract_int64(
                                   local_ms,
                                   offset_ms,
                                   utc_ms)
                           ? utc_ms
                           : local_ms;
            }

            if (!detail::is_time_shield_timestamp(local_ms)) {
                return local_ms;
            }

            const auto utc_ms = time_shield::zone_to_gmt_ms(
                static_cast<time_shield::ts_ms_t>(local_ms),
                m_zone,
                time_shield::AmbiguousTimePolicy::first_occurrence,
                time_shield::NonexistentTimePolicy::shift_forward);
            if (utc_ms == time_shield::ERROR_TIMESTAMP) {
                return local_ms;
            }
            return static_cast<std::int64_t>(utc_ms);
        }

        /// \brief Returns UTC timestamp for local day start containing utc_ms.
        std::int64_t start_of_local_day_utc_ms(std::int64_t utc_ms) const noexcept {
            const auto local_ms = to_local_ms(utc_ms);
            std::int64_t day_start_local_ms = local_ms;
            if (!detail::checked_start_of_period_ms(
                    local_ms,
                    time_shield::MS_PER_DAY,
                    day_start_local_ms)) {
                return utc_ms;
            }
            return to_utc_ms(day_start_local_ms);
        }

        /// \brief Returns UTC timestamp for next local day start after the day containing utc_ms.
        std::int64_t next_local_day_start_utc_ms(std::int64_t utc_ms) const noexcept {
            const auto local_ms = to_local_ms(utc_ms);
            std::int64_t day_start_local_ms = local_ms;
            if (!detail::checked_start_of_period_ms(
                    local_ms,
                    time_shield::MS_PER_DAY,
                    day_start_local_ms)) {
                return utc_ms;
            }

            std::int64_t next_day_start_local_ms = day_start_local_ms;
            if (!detail::checked_add_int64(
                    day_start_local_ms,
                    time_shield::MS_PER_DAY,
                    next_day_start_local_ms)) {
                return utc_ms;
            }
            return to_utc_ms(next_day_start_local_ms);
        }

        /// \brief Returns UTC timestamp for local hour start containing utc_ms.
        std::int64_t start_of_local_hour_utc_ms(std::int64_t utc_ms) const noexcept {
            std::int64_t offset_sec = 0;
            if (!try_offset_at_utc_ms(utc_ms, offset_sec)) {
                return utc_ms;
            }

            std::int64_t offset_ms = 0;
            if (!detail::checked_seconds_to_ms(offset_sec, offset_ms)) {
                return utc_ms;
            }

            std::int64_t local_ms = utc_ms;
            if (!detail::checked_add_int64(utc_ms, offset_ms, local_ms)) {
                return utc_ms;
            }

            std::int64_t local_hour_start_ms = local_ms;
            if (!detail::checked_start_of_period_ms(
                    local_ms,
                    time_shield::MS_PER_HOUR,
                    local_hour_start_ms)) {
                return utc_ms;
            }

            std::int64_t utc_hour_start_ms = local_hour_start_ms;
            if (!detail::checked_subtract_int64(
                    local_hour_start_ms,
                    offset_ms,
                    utc_hour_start_ms)) {
                return utc_ms;
            }
            return utc_hour_start_ms;
        }

    private:
        bool try_offset_at_utc_ms(
            std::int64_t utc_ms,
            std::int64_t& offset_sec) const noexcept {
            if (!is_named_zone()) {
                offset_sec = m_offset_sec;
                return true;
            }

            if (!detail::is_time_shield_timestamp(utc_ms)) {
                return false;
            }

            time_shield::tz_t resolved = 0;
            if (!time_shield::zone_offset_at_utc_ms(
                    static_cast<time_shield::ts_ms_t>(utc_ms),
                    m_zone,
                    resolved)) {
                return false;
            }

            offset_sec = static_cast<std::int64_t>(resolved);
            return true;
        }

        TradeTimeZoneMode m_mode = TradeTimeZoneMode::FIXED_OFFSET;
        std::int64_t m_offset_sec = 0;
        time_shield::TimeZone m_zone = time_shield::UTC;
        bool m_named_zone_supported = true;
    };

} // namespace optionx

#endif // OPTIONX_HEADER_DATA_TRADING_TRADE_TIME_ZONE_HPP_INCLUDED
