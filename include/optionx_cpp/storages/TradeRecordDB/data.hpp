#pragma once
#ifndef OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_DATA_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_DATA_HPP_INCLUDED

/// \file data.hpp
/// \brief Defines TradeRecordDB operation result DTOs.

#include <type_traits>

#include <optionx_cpp/data/trading.hpp>
#include "enums.hpp"

namespace optionx::storage {

    /// \struct TradeRecordDBMeta
    /// \brief Singleton metadata stored via ValueTable in TradeRecordDB.
    struct TradeRecordDBMeta {
        std::uint64_t db_version = 1;      ///< Database schema version.
        std::uint32_t next_trade_uid = 1;  ///< Next monotonic trade ID (1..UINT32_MAX), stored in low 32 bits of composite key.
        std::int64_t  last_update_ms = 0;  ///< Last wall-clock update time in milliseconds.
    };
    static_assert(std::is_trivially_copyable_v<TradeRecordDBMeta>,
                  "TradeRecordDBMeta must be trivially copyable for ValueTable storage");

    /// \brief Result of a TradeRecord write operation.
    using TradeRecordDBWriteResult = StorageWriteResult<TradeRecord>;

    /// \brief Result of a single-record TradeRecord read operation.
    using TradeRecordDBReadResult = StorageReadResult<TradeRecord>;

    /// \brief Result of a multi-record TradeRecord read operation.
    using TradeRecordDBListResult = StorageListResult<TradeRecord>;

} // namespace optionx::storage

#endif // OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_DATA_HPP_INCLUDED
