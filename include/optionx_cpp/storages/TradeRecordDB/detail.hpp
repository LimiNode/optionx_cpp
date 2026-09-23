#pragma once
#ifndef OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_DETAIL_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_DETAIL_HPP_INCLUDED

/// \file detail.hpp
/// \brief Internal helpers used only by TradeRecordDB.

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <time_shield/conversions/time_unit_conversions.hpp>

#include "data.hpp"

namespace optionx::storage::detail {

    /// \brief Selects canonical timestamp used by TradeRecordDB timestamp queries.
    ///
    /// Order of precedence for AUTO mode: place_date -> send_date -> open_date -> close_date.
    /// The backend must set place_date before writing a record; otherwise the record may not be found by range queries.
    inline std::int64_t selected_timestamp_ms(const TradeRecord& record) noexcept {
        if (record.place_date > 0) return record.place_date;
        if (record.send_date > 0) return record.send_date;
        if (record.open_date > 0) return record.open_date;
        if (record.close_date > 0) return record.close_date;
        return 0;
    }

    constexpr std::uint32_t kMinutesBias = 0x80000000u;

    /// \brief Biases a signed unix-minutes value for unsigned MDBX ordering.
    inline std::uint32_t bias_minutes(std::int32_t m) noexcept {
        return static_cast<std::uint32_t>(m) + kMinutesBias;
    }

    /// \brief Reverses bias_minutes to recover signed unix-minutes.
    inline std::int32_t unbias_minutes(std::uint32_t b) noexcept {
        return static_cast<std::int32_t>(b - kMinutesBias);
    }

    /// \brief Builds a composite uint64_t key from unix minutes and trade index.
    inline std::uint64_t make_composite_key(std::int32_t unix_minutes, std::uint32_t trade_index) noexcept {
        return (static_cast<std::uint64_t>(bias_minutes(unix_minutes)) << 32) | trade_index;
    }

    /// \brief Extracts unix minutes from a composite key.
    inline std::int32_t composite_key_minutes(std::uint64_t key) noexcept {
        return unbias_minutes(static_cast<std::uint32_t>(key >> 32));
    }

    /// \brief Converts milliseconds to unix minutes with int32 composite-key bounds.
    inline std::int32_t timestamp_ms_to_unix_minutes(std::int64_t timestamp_ms) noexcept {
        constexpr auto kMinMinutes = std::numeric_limits<std::int32_t>::min();
        constexpr auto kMaxMinutes = std::numeric_limits<std::int32_t>::max();
        const auto minutes = time_shield::ms_to_min<std::int64_t>(timestamp_ms);

        if (minutes <= static_cast<std::int64_t>(kMinMinutes)) return kMinMinutes;
        if (minutes >= static_cast<std::int64_t>(kMaxMinutes)) return kMaxMinutes;
        return static_cast<std::int32_t>(minutes);
    }

    /// \brief Extracts trade index from a composite key.
    inline std::uint32_t composite_key_index(std::uint64_t key) noexcept {
        return static_cast<std::uint32_t>(key & 0xFFFFFFFFu);
    }

    /// \brief Creates successful write result.
    inline TradeRecordDBWriteResult write_success(TradeRecord record) {
        TradeRecordDBWriteResult result;
        result.status = TradeRecordDBStatus::SUCCESS;
        result.record = std::move(record);
        return result;
    }

    /// \brief Creates failed write result.
    inline TradeRecordDBWriteResult write_error(TradeRecordDBStatus status, TradeRecord record, std::string message) {
        TradeRecordDBWriteResult result;
        result.status = status;
        result.record = std::move(record);
        result.message = std::move(message);
        return result;
    }

    /// \brief Creates failed read result.
    inline TradeRecordDBReadResult read_error(TradeRecordDBStatus status, std::string message) {
        TradeRecordDBReadResult result;
        result.status = status;
        result.message = std::move(message);
        return result;
    }

    /// \brief Creates failed list result.
    inline TradeRecordDBListResult list_error(TradeRecordDBStatus status, std::string message) {
        TradeRecordDBListResult result;
        result.status = status;
        result.message = std::move(message);
        return result;
    }

} // namespace optionx::storage::detail

#endif // OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_DETAIL_HPP_INCLUDED
