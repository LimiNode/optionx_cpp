#pragma once
#ifndef OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_HPP_INCLUDED

/// \file SignalRecordDB.hpp
/// \brief Provides persistent storage for SignalRecord objects using MDBX containers.

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
#include "SignalRecordDB/enums.hpp"
#include "SignalRecordDB/data.hpp"
#include "SignalRecordDB/detail.hpp"

#if defined(_WIN32) || defined(_WIN64)

#   ifndef OPTIONX_DATA_PATH
#   define OPTIONX_DATA_PATH "data"
#   endif

#   ifndef OPTIONX_DB_PATH
#   define OPTIONX_DB_PATH OPTIONX_DATA_PATH "\\db"
#   endif

#   ifndef OPTIONX_SIGNAL_RECORD_DB_FILE
#   define OPTIONX_SIGNAL_RECORD_DB_FILE OPTIONX_DB_PATH "\\signal_records"
#   endif

#else

#   ifndef OPTIONX_DATA_PATH
#   define OPTIONX_DATA_PATH "data"
#   endif

#   ifndef OPTIONX_DB_PATH
#   define OPTIONX_DB_PATH OPTIONX_DATA_PATH "/db"
#   endif

#   ifndef OPTIONX_SIGNAL_RECORD_DB_FILE
#   define OPTIONX_SIGNAL_RECORD_DB_FILE OPTIONX_DB_PATH "/signal_records"
#   endif

#endif

namespace optionx::storage {

    /// \class SignalRecordDB
    /// \brief Header-only MDBX-backed storage service for SignalRecord objects.
    ///
    /// SignalRecordDB owns the persistent linear signal_id sequence used by
    /// TradeSignal::signal_id, TradeRequest::signal_id, TradeRecord::signal_id
    /// and SignalRecord::signal_id. The signal_id itself is a 32-bit monotonic
    /// value; 0 means "not assigned".
    ///
    /// Direct methods execute synchronously. Buffered enqueue_* methods mirror
    /// TradeRecordDB: process() executes queued work on the caller thread when
    /// no worker is running, run() starts a worker, flush() waits for queued
    /// work, and callbacks are delivered by process(), flush() or shutdown().
    class SignalRecordDB {
    public:
        using write_callback_t = std::function<void(SignalRecordDBWriteResult)>;
        using read_callback_t = std::function<void(SignalRecordDBReadResult)>;
        using list_callback_t = std::function<void(SignalRecordDBListResult)>;
        using status_callback_t = std::function<void(SignalRecordDBStatus)>;

        /// \brief Creates default MDBX configuration for the signal records database.
        static mdbxc::Config default_config();

        /// \brief Constructs storage with the default configuration and table names.
        SignalRecordDB();

        /// \brief Constructs storage with custom MDBX configuration and optional table names.
        explicit SignalRecordDB(
            mdbxc::Config config,
            std::string records_table = "signal_records",
            std::string signal_id_index_table = "signal_record_signal_id_index",
            std::string trade_id_index_table = "signal_record_trade_id_index",
            std::string meta_table = "signal_record_meta");

        ~SignalRecordDB();

        SignalRecordDB(const SignalRecordDB&) = delete;
        SignalRecordDB& operator=(const SignalRecordDB&) = delete;
        SignalRecordDB(SignalRecordDB&&) = delete;
        SignalRecordDB& operator=(SignalRecordDB&&) = delete;

        /// \brief Returns true when the MDBX environment and all tables are open.
        bool is_open() const;

        /// \brief Reserves and persists a new linear signal ID.
        std::uint32_t get_signal_id();

        /// \brief Assigns a persistent signal ID to a signal when it does not have one.
        bool assign_signal_id(TradeSignal& signal);

        /// \brief Assigns a persistent signal ID to a signal record when it does not have one.
        bool assign_signal_id(SignalRecord& record);

        /// \brief Assigns a persistent signal ID to a request when it does not have one.
        bool assign_signal_id(TradeRequest& request);

        /// \brief Inserts or updates a signal record, allocating or reusing signal_id as needed.
        SignalRecordDBWriteResult upsert(SignalRecord record);

        /// \brief Writes a signal record that already has a signal_id.
        SignalRecordDBWriteResult write(SignalRecord record);

        /// \brief Finds a signal by persistent signal_id.
        SignalRecordDBReadResult find_by_signal_id(std::uint32_t signal_id) const;

        /// \brief Finds all signals with the user-defined unique_id value.
        /// \details Convenience full-scan search over all signal records.
        /// For large journals prefer the overload with an explicit time range.
        SignalRecordDBListResult find_by_uid(std::int64_t unique_id) const;

        /// \brief Finds all signals with unique_id inside [start_ms, stop_ms].
        SignalRecordDBListResult find_by_uid(
            std::int64_t unique_id,
            std::int64_t start_ms,
            std::int64_t stop_ms) const;

        /// \brief Finds the signal that currently owns the given produced trade_id.
        SignalRecordDBReadResult find_by_trade_id(std::uint32_t trade_id) const;

        /// \brief Finds all signal records whose canonical timestamp equals timestamp_ms.
        SignalRecordDBListResult find_by_timestamp(std::int64_t timestamp_ms) const;

        /// \brief Finds all signal records whose canonical timestamp is inside [start_ms, stop_ms].
        SignalRecordDBListResult find_range(std::int64_t start_ms, std::int64_t stop_ms) const;

        /// \brief Returns all signal records sorted by canonical timestamp and signal_id.
        SignalRecordDBListResult find_records() const;

        /// \brief Convenience overload returning only the record vector.
        std::vector<optionx::SignalRecord> find_records_vector() const;

        /// \brief Erases a signal record by signal_id and removes its indexes.
        SignalRecordDBStatus erase_by_signal_id(std::uint32_t signal_id);

        /// \brief Clears records, indices and meta table, then re-initializes meta.
        SignalRecordDBStatus clear();

        /// \brief Returns number of persisted signal records.
        std::size_t count() const;

        /// \brief Enqueues an upsert operation.
        SignalRecordDBStatus enqueue_upsert(SignalRecord record, write_callback_t callback = {});

        /// \brief Enqueues a write operation.
        SignalRecordDBStatus enqueue_write(SignalRecord record, write_callback_t callback = {});

        /// \brief Enqueues a find-by-signal-id operation.
        SignalRecordDBStatus enqueue_find_by_signal_id(std::uint32_t signal_id, read_callback_t callback = {});

        /// \brief Enqueues a full-scan find-by-UID operation.
        SignalRecordDBStatus enqueue_find_by_uid(std::int64_t unique_id, list_callback_t callback = {});

        /// \brief Enqueues a time-range find-by-UID operation.
        SignalRecordDBStatus enqueue_find_by_uid(
            std::int64_t unique_id,
            std::int64_t start_ms,
            std::int64_t stop_ms,
            list_callback_t callback = {});

        /// \brief Enqueues a find-by-trade-id operation.
        SignalRecordDBStatus enqueue_find_by_trade_id(std::uint32_t trade_id, read_callback_t callback = {});

        /// \brief Enqueues a range query operation.
        SignalRecordDBStatus enqueue_find_range(std::int64_t start_ms, std::int64_t stop_ms, list_callback_t callback = {});

        /// \brief Enqueues a find-all operation.
        SignalRecordDBStatus enqueue_find_records(list_callback_t callback = {});

        /// \brief Enqueues an erase-by-signal-id operation.
        SignalRecordDBStatus enqueue_erase_by_signal_id(std::uint32_t signal_id, status_callback_t callback = {});

        /// \brief Enqueues a clear operation.
        SignalRecordDBStatus enqueue_clear(status_callback_t callback = {});

        /// \brief Starts internal worker thread. Callbacks are still delivered only by process() or flush().
        bool run();

        /// \brief Executes queued work when no worker is running and delivers ready callbacks on the caller thread.
        void process();

        /// \brief Blocks until queued DB work is complete, then delivers callbacks on the caller thread.
        void flush();

        /// \brief Stops queue intake, waits for worker, delivers pending callbacks, and closes the database.
        void shutdown();

    private:
        using records_table_t = mdbxc::KeyValueTable<std::uint64_t, SignalRecord>;
        using signal_id_index_table_t = mdbxc::KeyValueTable<std::uint32_t, std::uint64_t>;
        using trade_id_index_table_t = mdbxc::KeyValueTable<std::uint32_t, std::uint64_t>;
        using meta_table_t = mdbxc::ValueTable<SignalRecordDBMeta>;

        mdbxc::Config m_config;
        std::string m_records_table_name;
        std::string m_signal_id_index_table_name;
        std::string m_trade_id_index_table_name;
        std::string m_meta_table_name;

        mutable std::mutex m_db_mutex;
        std::shared_ptr<mdbxc::Connection> m_connection;
        std::unique_ptr<records_table_t> m_records;
        std::unique_ptr<signal_id_index_table_t> m_signal_id_index;
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
        void close_storage_no_lock() noexcept;
        std::uint32_t reserve_signal_id_no_lock(MDBX_txn* txn);
        void bump_next_signal_id_no_lock(std::uint32_t used_signal_id, MDBX_txn* txn);
        void remove_indexes_no_lock(const SignalRecord& record, MDBX_txn* txn);
        void write_indexes_no_lock(const SignalRecord& record, std::uint64_t composite_key, MDBX_txn* txn);
        void store_record_no_lock(const SignalRecord& record, MDBX_txn* txn);
        std::uint32_t find_existing_signal_id_no_lock(const SignalRecord& record, MDBX_txn* txn);
        bool trade_id_conflicts_no_lock(const SignalRecord& record, MDBX_txn* txn, std::string* message = nullptr);

        SignalRecordDBWriteResult write_no_lock(SignalRecord record);
        SignalRecordDBWriteResult upsert_no_lock(SignalRecord record);
        SignalRecordDBReadResult find_by_signal_id_no_lock(std::uint32_t signal_id) const;
        SignalRecordDBListResult find_by_uid_no_lock(std::int64_t unique_id) const;
        SignalRecordDBListResult find_by_uid_no_lock(
            std::int64_t unique_id,
            std::int64_t start_ms,
            std::int64_t stop_ms) const;
        SignalRecordDBReadResult find_by_trade_id_no_lock(std::uint32_t trade_id) const;
        SignalRecordDBListResult find_by_timestamp_no_lock(std::int64_t timestamp_ms) const;
        SignalRecordDBListResult find_range_no_lock(std::int64_t start_ms, std::int64_t stop_ms) const;
        SignalRecordDBListResult find_records_no_lock() const;
        SignalRecordDBStatus erase_by_signal_id_no_lock(std::uint32_t signal_id);
        SignalRecordDBStatus clear_no_lock();

        bool worker_running() const;
        SignalRecordDBStatus enqueue_command(std::function<void()> command);
        void enqueue_callback(std::function<void()> callback);
        bool pop_command(std::function<void()>& command);
        void finish_command();
        void execute_command(std::function<void()> command);
        void drain_work_on_caller();
        void deliver_callbacks();
        void worker_loop();
    };

} // namespace optionx::storage

#include "SignalRecordDB/SignalRecordDB.ipp"

#endif // OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_HPP_INCLUDED
