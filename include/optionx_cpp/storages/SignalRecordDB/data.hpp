#pragma once
#ifndef OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_DATA_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_DATA_HPP_INCLUDED

/// \file data.hpp
/// \brief Defines SignalRecordDB operation result DTOs.

#include <type_traits>

#include <optionx_cpp/data/trading.hpp>
#include "enums.hpp"

namespace optionx::storage {

    /// \struct SignalRecordDBMeta
    /// \brief Singleton metadata stored via ValueTable in SignalRecordDB.
    struct SignalRecordDBMeta {
        std::uint64_t db_version = 1;       ///< Database schema version.
        std::uint32_t next_signal_id = 1;   ///< Next monotonic signal ID (1..UINT32_MAX).
        std::int64_t  last_update_ms = 0;   ///< Last wall-clock update time in milliseconds.
    };
    static_assert(std::is_trivially_copyable_v<SignalRecordDBMeta>,
                  "SignalRecordDBMeta must be trivially copyable for ValueTable storage");

    /// \brief Result of a SignalRecord write operation.
    using SignalRecordDBWriteResult = StorageWriteResult<SignalRecord>;

    /// \brief Result of a single-record SignalRecord read operation.
    using SignalRecordDBReadResult = StorageReadResult<SignalRecord>;

    /// \brief Result of a multi-record SignalRecord read operation.
    using SignalRecordDBListResult = StorageListResult<SignalRecord>;

} // namespace optionx::storage

#endif // OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_DATA_HPP_INCLUDED
