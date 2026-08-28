#pragma once
#ifndef OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_HPP_INCLUDED

/// \file TradeRecordDB.hpp
/// \brief Provides persistent storage for TradeRecord objects using MDBX containers.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <mdbx_containers/KeyValueTable.hpp>
#include <mdbx_containers/ValueTable.hpp>

#include <optionx_cpp/data/trading.hpp>
#include "common.hpp"
#include "TradeRecordDB/enums.hpp"
#include "TradeRecordDB/data.hpp"
#include "TradeRecordDB/detail.hpp"
#include "TradeRecordDB/TradeRecordFilterMatcher.hpp"
#include "TradeRecordDB/TradeRecordStatusFixer.hpp"
#include "TradeRecordDB/TradeStatsCalculator.hpp"
#include "TradeRecordDB/TradeMetaStatsCalculator.hpp"

#if defined(_WIN32) || defined(_WIN64)

#   ifndef OPTIONX_DATA_PATH
#   define OPTIONX_DATA_PATH "data"
#   endif

#   ifndef OPTIONX_DB_PATH
#   define OPTIONX_DB_PATH OPTIONX_DATA_PATH "\\db"
#   endif

#   ifndef OPTIONX_TRADE_RECORD_DB_FILE
#   define OPTIONX_TRADE_RECORD_DB_FILE OPTIONX_DB_PATH "\\trade_records"
#   endif

#else

#   ifndef OPTIONX_DATA_PATH
#   define OPTIONX_DATA_PATH "data"
#   endif

#   ifndef OPTIONX_DB_PATH
#   define OPTIONX_DB_PATH OPTIONX_DATA_PATH "/db"
#   endif

#   ifndef OPTIONX_TRADE_RECORD_DB_FILE
#   define OPTIONX_TRADE_RECORD_DB_FILE OPTIONX_DB_PATH "/trade_records"
#   endif

#endif

namespace optionx::storage {

    /// \class TradeRecordDB
    /// \brief Header-only MDBX-backed storage service for TradeRecord objects.
    ///
    /// TradeRecordDB owns the persistent linear trade ID sequence used by
    /// TradeRequest::trade_id, TradeResult::trade_id and TradeRecord::trade_id.
    /// The trade_id itself is a 32-bit monotonic value (1..UINT32_MAX) stored
    /// in the low 32 bits of the composite uint64_t primary key (high 32 bits
    /// store biased unix minutes).
    /// Direct methods execute synchronously and return result/status immediately.
    /// Buffered methods enqueue work with enqueue_*(); process() executes queued work
    /// on the caller thread when no worker is running, run() starts a worker, flush()
    /// waits for queued work, and shutdown() stops intake and closes storage.
    ///
    /// For correct chronological ordering and range queries, callers should set
    /// TradeRecord::place_date before writing. The AUTO timestamp selector uses
    /// place_date as the first priority (place -> send -> open -> close).
    ///
    /// Callbacks are delivered only by process(), flush(), or shutdown() on the
    /// calling thread. For broker execution, either call assign_trade_id() before
    /// place_trade() or install a platform trade ID provider such as
    /// `platform.on_trade_id() = [&db] { return db.get_trade_id(); };`. The same
    /// trade_id must then flow through TradeRequest -> TradeResult -> TradeRecord.
    class TradeRecordDB {
    public:
        using write_callback_t = std::function<void(TradeRecordDBWriteResult)>;
        using read_callback_t = std::function<void(TradeRecordDBReadResult)>;
        using list_callback_t = std::function<void(TradeRecordDBListResult)>;
        using status_callback_t = std::function<void(TradeRecordDBStatus)>;

        /// \brief Creates default MDBX configuration for the trade records database.
        /// \return MDBX config using OPTIONX_TRADE_RECORD_DB_FILE and at least four named databases.
        static mdbxc::Config default_config();

        /// \brief Constructs storage with the default configuration and table names.
        TradeRecordDB();

        /// \brief Constructs storage with custom MDBX configuration and optional table names.
        /// \param config MDBX connection configuration.
        /// \param records_table Main table name for composite_key -> TradeRecord.
        /// \param uid_index_table Optional unique_id -> composite_key index table.
        /// \param trade_id_index_table Optional trade_id -> composite_key index table.
        /// \param meta_table Metadata table for version and next trade ID.
        explicit TradeRecordDB(
            mdbxc::Config config,
            std::string records_table = "trade_records",
            std::string uid_index_table = "trade_record_uid_index",
            std::string trade_id_index_table = "trade_record_trade_id_index",
            std::string meta_table = "trade_record_meta");

        ~TradeRecordDB();

        TradeRecordDB(const TradeRecordDB&) = delete;
        TradeRecordDB& operator=(const TradeRecordDB&) = delete;
        TradeRecordDB(TradeRecordDB&&) = delete;
        TradeRecordDB& operator=(TradeRecordDB&&) = delete;

        /// \brief Returns true when the MDBX environment and all tables are open.
        bool is_open() const;

        /// \brief Reserves and persists a new linear trade ID.
        /// \return New trade ID, or 0 if the DB is closed, read-only or failed.
        ///
        /// The returned ID is consumed even if no TradeRecord is written later. Use
        /// it before sending a request to a broker when callbacks must carry the
        /// persistent database identity.
        std::uint32_t get_trade_id();

        /// \brief Assigns a persistent trade ID to a request when it does not have one.
        /// \param request Trade request to initialize.
        /// \return True when request.trade_id is positive after the call.
        ///
        /// This method never changes TradeRequest::unique_id. If request.trade_id is
        /// already positive it is preserved.
        bool assign_trade_id(TradeRequest& request);

        /// \brief Legacy name for get_trade_id().
        /// \return New trade ID converted to int64_t, or 0 on failure/overflow.
        std::int64_t get_trade_uid();

        /// \brief Legacy name for assign_trade_id(); does not change TradeRequest::unique_id.
        /// \param request Trade request to initialize through request.trade_id.
        /// \return True when request.trade_id is positive after the call.
        bool assign_trade_uid(TradeRequest& request);

        /// \brief Inserts or updates a trade record, allocating or reusing trade_id as needed.
        /// \param record Full trade snapshot to store.
        /// \return Write result with the normalized record.
        ///
        /// Match order is: explicit trade_id, unique_id index, broker identity,
        /// then a newly reserved linear trade_id. The stored snapshot is not field-merged;
        /// incoming data replaces the previous record.
        TradeRecordDBWriteResult upsert(TradeRecord record);

        /// \brief Writes a record that already has a trade_id.
        /// \param record Full trade snapshot to store.
        /// \return Write result with the normalized record.
        ///
        /// The record must have a positive trade_id.
        TradeRecordDBWriteResult write(TradeRecord record);

        /// \brief Finds a record by its trade_id.
        /// \param trade_id Persistent linear trade ID.
        /// \return Read result with found=true when the record exists.
        TradeRecordDBReadResult find_by_trade_id(std::uint32_t trade_id) const;

        /// \brief Finds a record through the optional user unique_id index.
        /// \param unique_id Positive user-defined correlation ID copied from TradeRequest::unique_id.
        /// \return Read result with found=true when an index entry and record exist.
        ///
        /// TradeRecord::unique_id is not the database identity. Prefer find_by_trade_id(trade_id)
        /// for persistent trade lookup.
        TradeRecordDBReadResult find_by_uid(std::int64_t unique_id) const;

        /// \brief Finds all records whose selected trade timestamp equals timestamp_ms.
        /// \param timestamp_ms Millisecond timestamp matched against open/place/send/close date.
        /// \return List result sorted by selected timestamp and trade_id.
        TradeRecordDBListResult find_by_timestamp(std::int64_t timestamp_ms) const;

        /// \brief Finds all records whose selected trade timestamp is inside [start_ms, stop_ms].
        /// \param start_ms Inclusive range start in milliseconds.
        /// \param stop_ms Inclusive range end in milliseconds.
        /// \return List result sorted by selected timestamp and trade_id.
        TradeRecordDBListResult find_range(std::int64_t start_ms, std::int64_t stop_ms) const;

        /// \brief Finds records matching an advanced query with filters, offset, limit, and status fixing.
        /// \param query Query parameters including time range, field selection, filter, and pagination.
        /// \return List result sorted according to query.ascending.
        TradeRecordDBListResult find_records(const optionx::TradeRecordQuery& query) const;

        /// \brief Convenience overload returning only the record vector.
        /// \param query Query parameters.
        /// \return Vector of matched records.
        std::vector<optionx::TradeRecord> find_records_vector(const optionx::TradeRecordQuery& query) const;

        /// \brief Finds all records for the current calendar day (local time).
        /// \param now_ms Current wall-clock time in milliseconds (UTC).
        /// \param time_zone Local-time context used to resolve the day boundaries.
        /// \return List result for the day containing now_ms in local time.
        TradeRecordDBListResult find_today(
            std::int64_t now_ms,
            const optionx::TradeTimeZone& time_zone) const;

        /// \brief Finds all records for the current calendar day using a fixed offset.
        /// \param now_ms Current wall-clock time in milliseconds (UTC).
        /// \param time_zone_sec Time zone offset in seconds.
        /// \return List result for the day containing now_ms in local time.
        TradeRecordDBListResult find_today(std::int64_t now_ms, std::int64_t time_zone_sec = 0) const;

        /// \brief Finds all records for the calendar day containing day_start_ms.
        /// \param day_start_ms Any timestamp within the target day (UTC).
        /// \param time_zone Local-time context used to resolve the day boundaries.
        /// \return List result for that calendar day.
        TradeRecordDBListResult find_day(
            std::int64_t day_start_ms,
            const optionx::TradeTimeZone& time_zone) const;

        /// \brief Finds all records for the calendar day using a fixed offset.
        /// \param day_start_ms Any timestamp within the target day (UTC).
        /// \param time_zone_sec Time zone offset in seconds.
        /// \return List result for that calendar day.
        TradeRecordDBListResult find_day(std::int64_t day_start_ms, std::int64_t time_zone_sec = 0) const;

        /// \brief Erases a record by trade_id and removes its UID index entry when present.
        /// \param trade_id Persistent linear trade ID.
        /// \return Operation status.
        TradeRecordDBStatus erase_by_trade_id(std::uint32_t trade_id);

        /// \brief Clears records, indices and meta table, then re-initializes meta.
        /// \return Operation status; on success the next generated trade ID starts from 1 again.
        TradeRecordDBStatus clear();

        /// \brief Returns number of persisted trade records.
        /// \return Record count, or 0 when the DB is closed or count fails.
        std::size_t count() const;

        /// \brief Enqueues an upsert operation.
        /// \param record Full trade snapshot to store.
        /// \param callback Optional callback delivered by process(), flush() or shutdown().
        /// \return SUCCESS when the command was accepted, QUEUE_CLOSED otherwise.
        TradeRecordDBStatus enqueue_upsert(TradeRecord record, write_callback_t callback = {});

        /// \brief Enqueues a write operation.
        /// \param record Full trade snapshot with record_id/trade_id.
        /// \param callback Optional callback delivered by process(), flush() or shutdown().
        /// \return SUCCESS when the command was accepted, QUEUE_CLOSED otherwise.
        TradeRecordDBStatus enqueue_write(TradeRecord record, write_callback_t callback = {});

        /// \brief Enqueues a find-by-trade-id operation.
        /// \param trade_id Persistent linear trade ID.
        /// \param callback Optional callback delivered by process(), flush() or shutdown().
        /// \return SUCCESS when the command was accepted, QUEUE_CLOSED otherwise.
        TradeRecordDBStatus enqueue_find_by_trade_id(std::uint32_t trade_id, read_callback_t callback = {});

        /// \brief Enqueues a find-by-UID operation.
        /// \param unique_id Positive user-defined correlation ID copied from TradeRequest::unique_id.
        /// \param callback Optional callback delivered by process(), flush() or shutdown().
        /// \return SUCCESS when the command was accepted, QUEUE_CLOSED otherwise.
        TradeRecordDBStatus enqueue_find_by_uid(std::int64_t unique_id, read_callback_t callback = {});

        /// \brief Enqueues a find-by-timestamp operation.
        /// \param timestamp_ms Millisecond timestamp to match.
        /// \param callback Optional callback delivered by process(), flush() or shutdown().
        /// \return SUCCESS when the command was accepted, QUEUE_CLOSED otherwise.
        TradeRecordDBStatus enqueue_find_by_timestamp(std::int64_t timestamp_ms, list_callback_t callback = {});

        /// \brief Enqueues a range query operation.
        /// \param start_ms Inclusive range start in milliseconds.
        /// \param stop_ms Inclusive range end in milliseconds.
        /// \param callback Optional callback delivered by process(), flush() or shutdown().
        /// \return SUCCESS when the command was accepted, QUEUE_CLOSED otherwise.
        TradeRecordDBStatus enqueue_find_range(std::int64_t start_ms, std::int64_t stop_ms, list_callback_t callback = {});

        /// \brief Enqueues an advanced filtered query operation.
        /// \param query Query parameters.
        /// \param callback Optional callback delivered by process(), flush() or shutdown().
        /// \return SUCCESS when the command was accepted, QUEUE_CLOSED otherwise.
        TradeRecordDBStatus enqueue_find_records(optionx::TradeRecordQuery query, list_callback_t callback = {});

        /// \brief Enqueues an erase-by-trade-id operation.
        /// \param trade_id Persistent linear trade ID.
        /// \param callback Optional callback delivered by process(), flush() or shutdown().
        /// \return SUCCESS when the command was accepted, QUEUE_CLOSED otherwise.
        TradeRecordDBStatus enqueue_erase_by_trade_id(std::uint32_t trade_id, status_callback_t callback = {});

        /// \brief Enqueues a clear operation.
        /// \param callback Optional callback delivered by process(), flush() or shutdown().
        /// \return SUCCESS when the command was accepted, QUEUE_CLOSED otherwise.
        TradeRecordDBStatus enqueue_clear(status_callback_t callback = {});

        /// \brief Starts internal worker thread. Callbacks are still delivered only by process() or flush().
        /// \return True when the worker was started.
        ///
        /// Use run() for background DB work when the application already calls process()
        /// periodically on its main/control thread.
        bool run();

        /// \brief Executes queued work when no worker is running and delivers ready callbacks on the caller thread.
        ///
        /// In synchronous-buffered mode, call process() periodically to perform both
        /// queued DB operations and callback delivery. With run() active, process()
        /// only delivers callbacks that the worker has prepared.
        void process();

        /// \brief Blocks until queued DB work is complete, then delivers callbacks on the caller thread.
        ///
        /// Use flush() before shutdown boundaries or tests that need all enqueued work
        /// and callbacks completed deterministically.
        void flush();

        /// \brief Stops queue intake, waits for worker, delivers pending callbacks, and closes the database.
        ///
        /// After shutdown(), enqueue_* methods return QUEUE_CLOSED and direct DB reads
        /// report NOT_OPEN.
        void shutdown();

    private:
        using records_table_t = mdbxc::KeyValueTable<std::uint64_t, TradeRecord>;
        using uid_index_table_t = mdbxc::KeyValueTable<std::int64_t, std::uint64_t>;
        using trade_id_index_table_t = mdbxc::KeyValueTable<std::uint64_t, std::uint64_t>;
        using meta_table_t = mdbxc::ValueTable<TradeRecordDBMeta>;

        mdbxc::Config m_config;
        std::string m_records_table_name;
        std::string m_uid_index_table_name;
        std::string m_trade_id_index_table_name;
        std::string m_meta_table_name;

        mutable std::mutex m_db_mutex;
        std::shared_ptr<mdbxc::Connection> m_connection;
        std::unique_ptr<records_table_t> m_records;
        std::unique_ptr<uid_index_table_t> m_uid_index;
        std::unique_ptr<trade_id_index_table_t> m_trade_id_index;
        std::unique_ptr<meta_table_t> m_meta;
        bool m_open = false;
        bool m_read_only = false;

        mutable std::mutex m_queue_mutex;
        std::condition_variable m_work_cv;
        std::condition_variable m_idle_cv;
        std::deque<std::function<void()>> m_work_queue;
        std::size_t m_active_work = 0;
        bool m_accepting = true;
        bool m_worker_stop = false;
        bool m_worker_running = false;
        std::thread m_worker_thread;

        std::mutex m_callback_mutex;
        std::deque<std::function<void()>> m_callback_queue;

        void open();
        bool is_open_no_lock() const noexcept;
        void init_meta_no_lock(MDBX_txn* txn);
        void update_last_update_no_lock(MDBX_txn* txn);
        std::uint32_t reserve_trade_id_no_lock(MDBX_txn* txn);
        void bump_next_trade_id_no_lock(std::uint32_t used_trade_id, MDBX_txn* txn);

        TradeRecordDBWriteResult write_no_lock(TradeRecord record);
        TradeRecordDBWriteResult upsert_no_lock(TradeRecord record);
        TradeRecordDBReadResult find_by_trade_id_no_lock(std::uint32_t trade_id) const;
        TradeRecordDBReadResult find_by_uid_no_lock(std::int64_t unique_id) const;
        TradeRecordDBListResult find_by_timestamp_no_lock(std::int64_t timestamp_ms) const;
        TradeRecordDBListResult find_range_no_lock(std::int64_t start_ms, std::int64_t stop_ms) const;
        TradeRecordDBListResult find_records_no_lock(const optionx::TradeRecordQuery& query) const;
        TradeRecordDBListResult find_today_no_lock(
            std::int64_t now_ms,
            const optionx::TradeTimeZone& time_zone) const;
        TradeRecordDBListResult find_day_no_lock(
            std::int64_t day_start_ms,
            const optionx::TradeTimeZone& time_zone) const;
        TradeRecordDBStatus erase_by_trade_id_no_lock(std::uint32_t trade_id);
        TradeRecordDBStatus clear_no_lock();

        std::uint32_t find_broker_identity_key_no_lock(
            const TradeRecord& record,
            MDBX_txn* txn) const;

        bool worker_running() const;
        TradeRecordDBStatus enqueue_command(std::function<void()> command);
        void enqueue_callback(std::function<void()> callback);
        bool pop_command(std::function<void()>& command);
        void finish_command();
        void execute_command(std::function<void()> command);
        void drain_work_on_caller();
        void deliver_callbacks();
        void worker_loop();
    };

} // namespace optionx::storage

#include "TradeRecordDB/TradeRecordDB.ipp"

#endif // OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_HPP_INCLUDED
