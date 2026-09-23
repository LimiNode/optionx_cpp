#pragma once
#ifndef OPTIONX_HEADER_DATA_TRADING_TRADE_TIME_ZONE_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TRADING_TRADE_TIME_ZONE_HPP_INCLUDED

/// \file TradeTimeZone.hpp
/// \brief Defines a local-time context for trade queries and statistics.

#include <cstdint>

#include <time_shield/core/constants.hpp>
#include <time_shield/conversions/date_time_conversions.hpp>
#include <time_shield/core/enums.hpp>
#include <time_shield/timezone/time_zone_conversions.hpp>
#include <time_shield/core/types.hpp>

namespace optionx {

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
        /// \param value time-shield zone value. UNKNOWN falls back to UTC.
        static TradeTimeZone named(time_shield::TimeZone value) noexcept {
            if (value == time_shield::UNKNOWN) {
                return utc();
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
            if (!is_named_zone()) {
                return m_offset_sec;
            }

            time_shield::tz_t resolved = 0;
            return time_shield::zone_offset_at_utc_ms(
                static_cast<time_shield::ts_ms_t>(utc_ms),
                m_zone,
                resolved)
                    ? static_cast<std::int64_t>(resolved)
                    : 0;
        }

        /// \brief Converts UTC milliseconds to this context's local civil milliseconds.
        std::int64_t to_local_ms(std::int64_t utc_ms) const noexcept {
            return utc_ms + offset_at_utc_ms(utc_ms) * time_shield::MS_PER_SEC;
        }

        /// \brief Converts local civil milliseconds in this context back to UTC.
        /// \details Unsupported named-zone conversions fall back to treating the
        ///          supplied civil timestamp as UTC.
        std::int64_t to_utc_ms(std::int64_t local_ms) const noexcept {
            if (!is_named_zone()) {
                return local_ms - m_offset_sec * time_shield::MS_PER_SEC;
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
            return to_utc_ms(time_shield::start_of_day_ms(local_ms));
        }

        /// \brief Returns UTC timestamp for next local day start after the day containing utc_ms.
        std::int64_t next_local_day_start_utc_ms(std::int64_t utc_ms) const noexcept {
            const auto local_ms = to_local_ms(utc_ms);
            const auto day_start_local = time_shield::start_of_day_ms(local_ms);
            return to_utc_ms(day_start_local + time_shield::MS_PER_DAY);
        }

        /// \brief Returns UTC timestamp for local hour start containing utc_ms.
        std::int64_t start_of_local_hour_utc_ms(std::int64_t utc_ms) const noexcept {
            const auto offset_ms = offset_at_utc_ms(utc_ms) * time_shield::MS_PER_SEC;
            const auto local_ms = utc_ms + offset_ms;
            return time_shield::start_of_hour_ms(local_ms) - offset_ms;
        }

    private:
        TradeTimeZoneMode m_mode = TradeTimeZoneMode::FIXED_OFFSET;
        std::int64_t m_offset_sec = 0;
        time_shield::TimeZone m_zone = time_shield::UTC;
    };

} // namespace optionx

#endif // OPTIONX_HEADER_DATA_TRADING_TRADE_TIME_ZONE_HPP_INCLUDED
