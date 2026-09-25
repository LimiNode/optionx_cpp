#pragma once
#ifndef OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_DETAIL_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_DETAIL_HPP_INCLUDED

/// \file detail.hpp
/// \brief Internal helpers used only by SignalRecordDB.

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <time_shield/conversions/time_unit_conversions.hpp>

#include "data.hpp"

namespace optionx::storage::signal_detail {

    /// \brief Selects canonical timestamp used by SignalRecordDB range queries.
    /// \details Signal creation is the primary event; accepted/rejected/completed
    /// timestamps are fallbacks for imported or partially reconstructed records.
    inline std::int64_t selected_timestamp_ms(const SignalRecord& record) noexcept {
        if (record.create_date > 0) return record.create_date;
        if (record.accept_date > 0) return record.accept_date;
        if (record.reject_date > 0) return record.reject_date;
        if (record.complete_date > 0) return record.complete_date;
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

    /// \brief Builds a composite uint64_t key from unix minutes and signal_id.
    inline std::uint64_t make_composite_key(
            std::int32_t unix_minutes,
            std::uint32_t signal_id) noexcept {
        return (static_cast<std::uint64_t>(bias_minutes(unix_minutes)) << 32) | signal_id;
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

    /// \brief Extracts signal_id from a composite key.
    inline std::uint32_t composite_key_signal_id(std::uint64_t key) noexcept {
        return static_cast<std::uint32_t>(key & 0xFFFFFFFFu);
    }

    /// \brief Creates successful write result.
    inline SignalRecordDBWriteResult write_success(SignalRecord record) {
        SignalRecordDBWriteResult result;
        result.status = SignalRecordDBStatus::SUCCESS;
        result.record = std::move(record);
        return result;
    }

    /// \brief Creates failed write result.
    inline SignalRecordDBWriteResult write_error(
            SignalRecordDBStatus status,
            SignalRecord record,
            std::string message) {
        SignalRecordDBWriteResult result;
        result.status = status;
        result.record = std::move(record);
        result.message = std::move(message);
        return result;
    }

    /// \brief Creates failed read result.
    inline SignalRecordDBReadResult read_error(SignalRecordDBStatus status, std::string message) {
        SignalRecordDBReadResult result;
        result.status = status;
        result.message = std::move(message);
        return result;
    }

    /// \brief Creates failed list result.
    inline SignalRecordDBListResult list_error(SignalRecordDBStatus status, std::string message) {
        SignalRecordDBListResult result;
        result.status = status;
        result.message = std::move(message);
        return result;
    }

} // namespace optionx::storage::signal_detail

#endif // OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_DETAIL_HPP_INCLUDED
