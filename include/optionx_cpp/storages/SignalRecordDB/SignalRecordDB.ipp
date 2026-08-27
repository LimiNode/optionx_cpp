#pragma once
#ifndef OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_SIGNAL_RECORD_DB_IPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_SIGNAL_RECORD_DB_IPP_INCLUDED

/// \file SignalRecordDB.ipp
/// \brief Inline implementation of SignalRecordDB.

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

#include <time_shield.hpp>

#include <optionx_cpp/utils.hpp>

namespace optionx::storage {

    inline mdbxc::Config SignalRecordDB::default_config() {
        mdbxc::Config config;
        config.pathname = OPTIONX_SIGNAL_RECORD_DB_FILE;
        config.max_dbs = 4;
        config.no_subdir = false;
        config.relative_to_exe = true;
        return config;
    }

    inline SignalRecordDB::SignalRecordDB()
        : SignalRecordDB(default_config()) {}

    inline SignalRecordDB::SignalRecordDB(
        mdbxc::Config config,
        std::string records_table,
        std::string signal_id_index_table,
        std::string trade_id_index_table,
        std::string meta_table)
        : m_config(std::move(config)),
          m_records_table_name(std::move(records_table)),
          m_signal_id_index_table_name(std::move(signal_id_index_table)),
          m_trade_id_index_table_name(std::move(trade_id_index_table)),
          m_meta_table_name(std::move(meta_table)) {
        open();
    }

    inline SignalRecordDB::~SignalRecordDB() {
        shutdown();
    }

    inline bool SignalRecordDB::is_open() const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        return is_open_no_lock();
    }

    inline std::uint32_t SignalRecordDB::get_signal_id() {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            if (!is_open_no_lock()) return 0;
            if (m_read_only) return 0;

            auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);
            const auto signal_id = reserve_signal_id_no_lock(txn.handle());
            if (signal_id == 0) return 0;
            update_last_update_no_lock(txn.handle());
            txn.commit();
            return signal_id;
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB signal_id database error: ", ex);
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB signal_id error: ", ex);
        }
        return 0;
    }

    inline bool SignalRecordDB::assign_signal_id(TradeSignal& signal) {
        if (signal.signal_id > 0) return true;
        signal.signal_id = get_signal_id();
        return signal.signal_id > 0;
    }

    inline bool SignalRecordDB::assign_signal_id(SignalRecord& record) {
        if (record.signal_id > 0) return true;
        record.signal_id = get_signal_id();
        return record.signal_id > 0;
    }

    inline bool SignalRecordDB::assign_signal_id(TradeRequest& request) {
        if (request.signal_id > 0) return true;
        request.signal_id = get_signal_id();
        return request.signal_id > 0;
    }

    inline SignalRecordDBWriteResult SignalRecordDB::upsert(SignalRecord record) {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return upsert_no_lock(record);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB upsert database error: ", ex);
            return signal_detail::write_error(SignalRecordDBStatus::DB_ERROR, record, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB upsert error: ", ex);
            return signal_detail::write_error(SignalRecordDBStatus::DB_ERROR, record, ex.what());
        }
    }

    inline SignalRecordDBWriteResult SignalRecordDB::write(SignalRecord record) {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return write_no_lock(record);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB write database error: ", ex);
            return signal_detail::write_error(SignalRecordDBStatus::DB_ERROR, record, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB write error: ", ex);
            return signal_detail::write_error(SignalRecordDBStatus::DB_ERROR, record, ex.what());
        }
    }

    inline SignalRecordDBReadResult SignalRecordDB::find_by_signal_id(std::uint32_t signal_id) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_by_signal_id_no_lock(signal_id);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_by_signal_id database error: ", ex);
            return signal_detail::read_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_by_signal_id error: ", ex);
            return signal_detail::read_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline SignalRecordDBListResult SignalRecordDB::find_by_uid(std::int64_t unique_id) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_by_uid_no_lock(unique_id);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_by_uid database error: ", ex);
            return signal_detail::list_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_by_uid error: ", ex);
            return signal_detail::list_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline SignalRecordDBListResult SignalRecordDB::find_by_uid(
            std::int64_t unique_id,
            std::int64_t start_ms,
            std::int64_t stop_ms) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_by_uid_no_lock(unique_id, start_ms, stop_ms);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_by_uid range database error: ", ex);
            return signal_detail::list_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_by_uid range error: ", ex);
            return signal_detail::list_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline SignalRecordDBReadResult SignalRecordDB::find_by_trade_id(std::uint32_t trade_id) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_by_trade_id_no_lock(trade_id);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_by_trade_id database error: ", ex);
            return signal_detail::read_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_by_trade_id error: ", ex);
            return signal_detail::read_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline SignalRecordDBListResult SignalRecordDB::find_by_timestamp(std::int64_t timestamp_ms) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_by_timestamp_no_lock(timestamp_ms);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_by_timestamp database error: ", ex);
            return signal_detail::list_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_by_timestamp error: ", ex);
            return signal_detail::list_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline SignalRecordDBListResult SignalRecordDB::find_range(std::int64_t start_ms, std::int64_t stop_ms) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_range_no_lock(start_ms, stop_ms);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_range database error: ", ex);
            return signal_detail::list_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_range error: ", ex);
            return signal_detail::list_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline SignalRecordDBListResult SignalRecordDB::find_records() const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_records_no_lock();
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_records database error: ", ex);
            return signal_detail::list_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB find_records error: ", ex);
            return signal_detail::list_error(SignalRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline std::vector<optionx::SignalRecord> SignalRecordDB::find_records_vector() const {
        auto result = find_records();
        if (!result.ok()) {
            return {};
        }
        return std::move(result.records);
    }

    inline SignalRecordDBStatus SignalRecordDB::erase_by_signal_id(std::uint32_t signal_id) {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return erase_by_signal_id_no_lock(signal_id);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB erase_by_signal_id database error: ", ex);
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB erase_by_signal_id error: ", ex);
        }
        return SignalRecordDBStatus::DB_ERROR;
    }

    inline SignalRecordDBStatus SignalRecordDB::clear() {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return clear_no_lock();
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB clear database error: ", ex);
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB clear error: ", ex);
        }
        return SignalRecordDBStatus::DB_ERROR;
    }

    inline std::size_t SignalRecordDB::count() const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            if (!is_open_no_lock()) return 0;
            return m_records->count();
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB count database error: ", ex);
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB count error: ", ex);
        }
        return 0;
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_upsert(SignalRecord record, write_callback_t callback) {
        return enqueue_command([this, record = std::move(record), callback = std::move(callback)]() mutable {
            auto result = upsert(std::move(record));
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_write(SignalRecord record, write_callback_t callback) {
        return enqueue_command([this, record = std::move(record), callback = std::move(callback)]() mutable {
            auto result = write(std::move(record));
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_find_by_signal_id(
            std::uint32_t signal_id,
            read_callback_t callback) {
        return enqueue_command([this, signal_id, callback = std::move(callback)]() mutable {
            auto result = find_by_signal_id(signal_id);
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_find_by_uid(
            std::int64_t unique_id,
            list_callback_t callback) {
        return enqueue_command([this, unique_id, callback = std::move(callback)]() mutable {
            auto result = find_by_uid(unique_id);
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_find_by_uid(
            std::int64_t unique_id,
            std::int64_t start_ms,
            std::int64_t stop_ms,
            list_callback_t callback) {
        return enqueue_command([this, unique_id, start_ms, stop_ms, callback = std::move(callback)]() mutable {
            auto result = find_by_uid(unique_id, start_ms, stop_ms);
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_find_by_trade_id(
            std::uint32_t trade_id,
            read_callback_t callback) {
        return enqueue_command([this, trade_id, callback = std::move(callback)]() mutable {
            auto result = find_by_trade_id(trade_id);
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_find_range(
            std::int64_t start_ms,
            std::int64_t stop_ms,
            list_callback_t callback) {
        return enqueue_command([this, start_ms, stop_ms, callback = std::move(callback)]() mutable {
            auto result = find_range(start_ms, stop_ms);
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_find_records(list_callback_t callback) {
        return enqueue_command([this, callback = std::move(callback)]() mutable {
            auto result = find_records();
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_erase_by_signal_id(
            std::uint32_t signal_id,
            status_callback_t callback) {
        return enqueue_command([this, signal_id, callback = std::move(callback)]() mutable {
            auto status = erase_by_signal_id(signal_id);
            enqueue_callback([callback = std::move(callback), status]() mutable {
                if (callback) callback(status);
            });
        });
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_clear(status_callback_t callback) {
        return enqueue_command([this, callback = std::move(callback)]() mutable {
            auto status = clear();
            enqueue_callback([callback = std::move(callback), status]() mutable {
                if (callback) callback(status);
            });
        });
    }

    inline bool SignalRecordDB::run() {
        if (!is_open()) return false;
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (!m_accepting || m_worker_running) return false;
            m_worker_stop = false;
            m_worker_running = true;
        }

        try {
            m_worker_thread = std::thread([this]() { worker_loop(); });
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB worker start error: ", ex);
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_worker_running = false;
            return false;
        }
        return true;
    }

    inline void SignalRecordDB::process() {
        if (!worker_running()) {
            drain_work_on_caller();
        }
        deliver_callbacks();
    }

    inline void SignalRecordDB::flush() {
        if (worker_running()) {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_idle_cv.wait(lock, [this]() {
                return m_work_queue.empty() && m_active_work == 0;
            });
        } else {
            drain_work_on_caller();
        }
        deliver_callbacks();
    }

    inline void SignalRecordDB::shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (!m_accepting && !m_worker_running && !m_open) return;
            m_accepting = false;
            m_worker_stop = true;
        }
        m_work_cv.notify_all();

        if (m_worker_thread.joinable()) {
            m_worker_thread.join();
        }
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_worker_running = false;
        }

        drain_work_on_caller();
        deliver_callbacks();

        std::lock_guard<std::mutex> lock(m_db_mutex);
        close_storage_no_lock();
    }

    inline void SignalRecordDB::open() {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            if (m_config.max_dbs < 4) {
                m_config.max_dbs = 4;
            }
            m_read_only = m_config.read_only;
            m_connection = mdbxc::Connection::create(m_config);
            m_records = std::make_unique<records_table_t>(m_connection, m_records_table_name);
            m_signal_id_index = std::make_unique<signal_id_index_table_t>(
                m_connection,
                m_signal_id_index_table_name);
            m_trade_id_index = std::make_unique<trade_id_index_table_t>(m_connection, m_trade_id_index_table_name);
            m_meta = std::make_unique<meta_table_t>(m_connection, m_meta_table_name);
            m_open = true;

            if (!m_read_only) {
                auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);
                init_meta_no_lock(txn.handle());
                txn.commit();
            }
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB connection error: ", ex);
            close_storage_no_lock();
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB initialization error: ", ex);
            close_storage_no_lock();
        }
    }

    inline bool SignalRecordDB::is_open_no_lock() const noexcept {
        return m_open && m_connection && m_connection->is_connected() &&
               m_records && m_signal_id_index && m_trade_id_index && m_meta;
    }

    inline void SignalRecordDB::init_meta_no_lock(MDBX_txn* txn) {
        auto meta = m_meta->get_or(SignalRecordDBMeta{}, txn);
        if (meta.db_version == 0) {
            meta.db_version = 1;
        }
        if (meta.next_signal_id == 0) {
            meta.next_signal_id = 1;
        }
        m_meta->set(meta, txn);
    }

    inline void SignalRecordDB::update_last_update_no_lock(MDBX_txn* txn) {
        auto meta = m_meta->get_or(SignalRecordDBMeta{}, txn);
        meta.last_update_ms = time_shield::timestamp_ms();
        m_meta->set(meta, txn);
    }

    inline void SignalRecordDB::close_storage_no_lock() noexcept {
        m_records.reset();
        m_signal_id_index.reset();
        m_trade_id_index.reset();
        m_meta.reset();

        try {
            if (m_connection && m_connection->is_connected()) {
                m_connection->shutdown();
            }
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB cleanup database error: ", ex);
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB cleanup error: ", ex);
        } catch (...) {
            LOGIT_PRINT_ERROR("SignalRecordDB cleanup unknown error");
        }

        m_connection.reset();
        m_open = false;
    }

    inline std::uint32_t SignalRecordDB::reserve_signal_id_no_lock(MDBX_txn* txn) {
        auto meta = m_meta->get_or(SignalRecordDBMeta{}, txn);
        auto candidate = meta.next_signal_id;
        if (candidate == 0) candidate = 1;

        const auto start = candidate;
        bool wrapped = false;
        while (candidate != 0 && m_signal_id_index->contains(candidate, txn)) {
            if (++candidate == 0) {
                candidate = 1;
                wrapped = true;
            }
            if (wrapped && candidate == start) {
                return 0;
            }
        }
        if (candidate == 0) return 0;

        meta.next_signal_id = candidate + 1;
        if (meta.next_signal_id == 0) meta.next_signal_id = 1;
        m_meta->set(meta, txn);
        return candidate;
    }

    inline void SignalRecordDB::bump_next_signal_id_no_lock(std::uint32_t used_signal_id, MDBX_txn* txn) {
        constexpr auto max_uint32 = std::numeric_limits<std::uint32_t>::max();
        if (used_signal_id == 0) return;

        auto meta = m_meta->get_or(SignalRecordDBMeta{}, txn);
        if (meta.next_signal_id == 0) meta.next_signal_id = 1;
        const auto next = meta.next_signal_id;
        if (next <= used_signal_id) {
            if (used_signal_id == max_uint32) {
                meta.next_signal_id = 1;
            } else {
                meta.next_signal_id = static_cast<std::uint32_t>(used_signal_id + 1);
            }
            m_meta->set(meta, txn);
        }
    }

    inline void SignalRecordDB::remove_indexes_no_lock(const SignalRecord& record, MDBX_txn* txn) {
        const auto erase_if_same_signal =
            [this, &record, txn](auto& table, const auto& key) {
                const auto composite = table->find(key, txn);
                if (!composite) return;
                const auto indexed_record = m_records->find(*composite, txn);
                if (!indexed_record || indexed_record->signal_id == record.signal_id) {
                    table->erase(key, txn);
                }
            };

        if (record.signal_id > 0) {
            erase_if_same_signal(m_signal_id_index, record.signal_id);
        }
        for (const auto trade_id : record.trade_ids) {
            if (trade_id == 0) continue;
            erase_if_same_signal(m_trade_id_index, trade_id);
        }
    }

    inline void SignalRecordDB::write_indexes_no_lock(
            const SignalRecord& record,
            std::uint64_t composite_key,
            MDBX_txn* txn) {
        if (record.signal_id > 0) {
            m_signal_id_index->insert_or_assign(record.signal_id, composite_key, txn);
        }
        for (const auto trade_id : record.trade_ids) {
            if (trade_id == 0) continue;
            m_trade_id_index->insert_or_assign(trade_id, composite_key, txn);
        }
    }

    inline void SignalRecordDB::store_record_no_lock(const SignalRecord& record, MDBX_txn* txn) {
        const auto existing_composite = m_signal_id_index->find(record.signal_id, txn);
        const auto ts_ms = signal_detail::selected_timestamp_ms(record);
        const auto unix_minutes = signal_detail::timestamp_ms_to_unix_minutes(ts_ms);
        const auto new_composite_key = signal_detail::make_composite_key(unix_minutes, record.signal_id);

        if (existing_composite) {
            const auto old_record = m_records->find(*existing_composite, txn);
            if (old_record) {
                remove_indexes_no_lock(*old_record, txn);
            }
            if (*existing_composite != new_composite_key) {
                m_records->erase(*existing_composite, txn);
            }
        }

        m_records->insert_or_assign(new_composite_key, record, txn);
        write_indexes_no_lock(record, new_composite_key, txn);
        bump_next_signal_id_no_lock(record.signal_id, txn);
        update_last_update_no_lock(txn);
    }

    inline std::uint32_t SignalRecordDB::find_existing_signal_id_no_lock(
            const SignalRecord& record,
            MDBX_txn* txn) {
        if (record.signal_id > 0) return record.signal_id;

        for (const auto trade_id : record.trade_ids) {
            if (trade_id == 0) continue;
            const auto composite = m_trade_id_index->find(trade_id, txn);
            if (!composite) continue;
            const auto existing_record = m_records->find(*composite, txn);
            if (existing_record) return existing_record->signal_id;
            m_trade_id_index->erase(trade_id, txn);
        }

        return 0;
    }

    inline bool SignalRecordDB::trade_id_conflicts_no_lock(
            const SignalRecord& record,
            MDBX_txn* txn,
            std::string* message) {
        for (const auto trade_id : record.trade_ids) {
            if (trade_id == 0) continue;

            const auto composite = m_trade_id_index->find(trade_id, txn);
            if (!composite) continue;

            const auto existing_record = m_records->find(*composite, txn);
            if (!existing_record) {
                m_trade_id_index->erase(trade_id, txn);
                continue;
            }
            if (existing_record->signal_id == record.signal_id) continue;

            if (message) {
                *message = "SignalRecord trade_id already belongs to another signal";
            }
            return true;
        }
        return false;
    }

    inline SignalRecordDBWriteResult SignalRecordDB::write_no_lock(SignalRecord record) {
        if (!is_open_no_lock()) {
            return signal_detail::write_error(
                SignalRecordDBStatus::NOT_OPEN,
                std::move(record),
                "SignalRecordDB is not open");
        }
        if (m_read_only) {
            return signal_detail::write_error(
                SignalRecordDBStatus::READ_ONLY,
                std::move(record),
                "SignalRecordDB is read-only");
        }
        if (record.signal_id == 0) {
            return signal_detail::write_error(
                SignalRecordDBStatus::INVALID_ARGUMENT,
                std::move(record),
                "SignalRecord signal_id is required");
        }

        auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);
        std::string conflict_message;
        if (trade_id_conflicts_no_lock(record, txn.handle(), &conflict_message)) {
            return signal_detail::write_error(
                SignalRecordDBStatus::CONFLICT,
                std::move(record),
                conflict_message);
        }

        store_record_no_lock(record, txn.handle());
        txn.commit();
        return signal_detail::write_success(std::move(record));
    }

    inline SignalRecordDBWriteResult SignalRecordDB::upsert_no_lock(SignalRecord record) {
        if (!is_open_no_lock()) {
            return signal_detail::write_error(
                SignalRecordDBStatus::NOT_OPEN,
                std::move(record),
                "SignalRecordDB is not open");
        }
        if (m_read_only) {
            return signal_detail::write_error(
                SignalRecordDBStatus::READ_ONLY,
                std::move(record),
                "SignalRecordDB is read-only");
        }

        auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);

        auto selected_signal_id = find_existing_signal_id_no_lock(record, txn.handle());
        if (selected_signal_id == 0) {
            selected_signal_id = reserve_signal_id_no_lock(txn.handle());
            if (selected_signal_id == 0) {
                return signal_detail::write_error(
                    SignalRecordDBStatus::SEQUENCE_EXHAUSTED,
                    std::move(record),
                    "SignalRecordDB could not reserve a signal_id");
            }
        }

        record.signal_id = selected_signal_id;
        std::string conflict_message;
        if (trade_id_conflicts_no_lock(record, txn.handle(), &conflict_message)) {
            return signal_detail::write_error(
                SignalRecordDBStatus::CONFLICT,
                std::move(record),
                conflict_message);
        }

        store_record_no_lock(record, txn.handle());
        txn.commit();
        return signal_detail::write_success(std::move(record));
    }

    inline SignalRecordDBReadResult SignalRecordDB::find_by_signal_id_no_lock(std::uint32_t signal_id) const {
        if (!is_open_no_lock()) {
            return signal_detail::read_error(SignalRecordDBStatus::NOT_OPEN, "SignalRecordDB is not open");
        }
        if (signal_id == 0) {
            return signal_detail::read_error(SignalRecordDBStatus::INVALID_ARGUMENT, "SignalRecord signal_id is required");
        }

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        const auto composite = m_signal_id_index->find(signal_id, txn);
        if (!composite) {
            txn.commit();
            return signal_detail::read_error(SignalRecordDBStatus::NOT_FOUND, "SignalRecord was not found");
        }

        const auto record = m_records->find(*composite, txn);
        txn.commit();
        if (!record) {
            return signal_detail::read_error(
                SignalRecordDBStatus::NOT_FOUND,
                "SignalRecord signal_id index points to missing record");
        }
        if (record->signal_id != signal_id) {
            return signal_detail::read_error(
                SignalRecordDBStatus::NOT_FOUND,
                "SignalRecord signal_id index points to a different signal_id");
        }

        SignalRecordDBReadResult result;
        result.status = SignalRecordDBStatus::SUCCESS;
        result.record = *record;
        result.found = true;
        return result;
    }

    inline SignalRecordDBListResult SignalRecordDB::find_by_uid_no_lock(std::int64_t unique_id) const {
        if (!is_open_no_lock()) {
            return signal_detail::list_error(SignalRecordDBStatus::NOT_OPEN, "SignalRecordDB is not open");
        }

        const auto start_key = signal_detail::make_composite_key(std::numeric_limits<std::int32_t>::min(), 0);
        const auto stop_key = signal_detail::make_composite_key(
            std::numeric_limits<std::int32_t>::max(),
            0xFFFFFFFFu);

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        SignalRecordDBListResult result;
        result.status = SignalRecordDBStatus::SUCCESS;

        m_records->for_each_range(
            start_key,
            stop_key,
            [&result, unique_id](const std::uint64_t&, const SignalRecord& record) {
                if (record.unique_id == unique_id) {
                    result.records.push_back(record);
                }
                return true;
            },
            txn.handle());

        txn.commit();
        return result;
    }

    inline SignalRecordDBListResult SignalRecordDB::find_by_uid_no_lock(
            std::int64_t unique_id,
            std::int64_t start_ms,
            std::int64_t stop_ms) const {
        if (!is_open_no_lock()) {
            return signal_detail::list_error(SignalRecordDBStatus::NOT_OPEN, "SignalRecordDB is not open");
        }
        if (start_ms < 0 || stop_ms < 0 || start_ms > stop_ms) {
            return signal_detail::list_error(
                SignalRecordDBStatus::INVALID_ARGUMENT,
                "SignalRecord timestamp range is invalid");
        }

        const auto start_min = signal_detail::timestamp_ms_to_unix_minutes(start_ms);
        const auto stop_min = signal_detail::timestamp_ms_to_unix_minutes(stop_ms);
        const auto start_key = signal_detail::make_composite_key(start_min, 0);
        const auto stop_key = signal_detail::make_composite_key(stop_min, 0xFFFFFFFFu);

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        SignalRecordDBListResult result;
        result.status = SignalRecordDBStatus::SUCCESS;

        m_records->for_each_range(
            start_key,
            stop_key,
            [&result, unique_id, start_ms, stop_ms](const std::uint64_t&, const SignalRecord& record) {
                const auto timestamp = signal_detail::selected_timestamp_ms(record);
                if (record.unique_id == unique_id && timestamp >= start_ms && timestamp <= stop_ms) {
                    result.records.push_back(record);
                }
                return true;
            },
            txn.handle());

        txn.commit();
        return result;
    }

    inline SignalRecordDBReadResult SignalRecordDB::find_by_trade_id_no_lock(std::uint32_t trade_id) const {
        if (!is_open_no_lock()) {
            return signal_detail::read_error(SignalRecordDBStatus::NOT_OPEN, "SignalRecordDB is not open");
        }
        if (trade_id == 0) {
            return signal_detail::read_error(
                SignalRecordDBStatus::INVALID_ARGUMENT,
                "SignalRecord produced trade_id is required");
        }

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        const auto composite = m_trade_id_index->find(trade_id, txn);
        if (!composite) {
            txn.commit();
            return signal_detail::read_error(
                SignalRecordDBStatus::NOT_FOUND,
                "SignalRecord trade_id index entry was not found");
        }

        const auto record = m_records->find(*composite, txn);
        txn.commit();
        if (!record) {
            return signal_detail::read_error(
                SignalRecordDBStatus::NOT_FOUND,
                "SignalRecord trade_id index points to missing record");
        }
        if (!record->has_trade_id(trade_id)) {
            return signal_detail::read_error(
                SignalRecordDBStatus::NOT_FOUND,
                "SignalRecord trade_id index points to a record without that trade_id");
        }

        SignalRecordDBReadResult result;
        result.status = SignalRecordDBStatus::SUCCESS;
        result.record = *record;
        result.found = true;
        return result;
    }

    inline SignalRecordDBListResult SignalRecordDB::find_by_timestamp_no_lock(std::int64_t timestamp_ms) const {
        if (!is_open_no_lock()) {
            return signal_detail::list_error(SignalRecordDBStatus::NOT_OPEN, "SignalRecordDB is not open");
        }
        if (timestamp_ms < 0) {
            return signal_detail::list_error(SignalRecordDBStatus::INVALID_ARGUMENT, "SignalRecord timestamp is invalid");
        }

        const auto target_min = signal_detail::timestamp_ms_to_unix_minutes(timestamp_ms);
        const auto start_key = signal_detail::make_composite_key(target_min, 0);
        const auto stop_key = signal_detail::make_composite_key(target_min, 0xFFFFFFFFu);

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        SignalRecordDBListResult result;
        result.status = SignalRecordDBStatus::SUCCESS;

        m_records->for_each_range(
            start_key,
            stop_key,
            [&result, timestamp_ms](const std::uint64_t&, const SignalRecord& record) {
                if (signal_detail::selected_timestamp_ms(record) == timestamp_ms) {
                    result.records.push_back(record);
                }
                return true;
            },
            txn.handle());

        txn.commit();

        std::sort(result.records.begin(), result.records.end(), [](const SignalRecord& lhs, const SignalRecord& rhs) {
            const auto lhs_timestamp = signal_detail::selected_timestamp_ms(lhs);
            const auto rhs_timestamp = signal_detail::selected_timestamp_ms(rhs);
            if (lhs_timestamp != rhs_timestamp) return lhs_timestamp < rhs_timestamp;
            return lhs.signal_id < rhs.signal_id;
        });
        return result;
    }

    inline SignalRecordDBListResult SignalRecordDB::find_range_no_lock(
            std::int64_t start_ms,
            std::int64_t stop_ms) const {
        if (!is_open_no_lock()) {
            return signal_detail::list_error(SignalRecordDBStatus::NOT_OPEN, "SignalRecordDB is not open");
        }
        if (start_ms < 0 || stop_ms < 0 || start_ms > stop_ms) {
            return signal_detail::list_error(SignalRecordDBStatus::INVALID_ARGUMENT, "SignalRecord timestamp range is invalid");
        }

        const auto start_min = signal_detail::timestamp_ms_to_unix_minutes(start_ms);
        const auto stop_min = signal_detail::timestamp_ms_to_unix_minutes(stop_ms);
        const auto start_key = signal_detail::make_composite_key(start_min, 0);
        const auto stop_key = signal_detail::make_composite_key(stop_min, 0xFFFFFFFFu);

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        SignalRecordDBListResult result;
        result.status = SignalRecordDBStatus::SUCCESS;

        m_records->for_each_range(
            start_key,
            stop_key,
            [&result, start_ms, stop_ms](const std::uint64_t&, const SignalRecord& record) {
                const auto timestamp = signal_detail::selected_timestamp_ms(record);
                if (timestamp >= start_ms && timestamp <= stop_ms) {
                    result.records.push_back(record);
                }
                return true;
            },
            txn.handle());

        txn.commit();

        std::sort(result.records.begin(), result.records.end(), [](const SignalRecord& lhs, const SignalRecord& rhs) {
            const auto lhs_timestamp = signal_detail::selected_timestamp_ms(lhs);
            const auto rhs_timestamp = signal_detail::selected_timestamp_ms(rhs);
            if (lhs_timestamp != rhs_timestamp) return lhs_timestamp < rhs_timestamp;
            return lhs.signal_id < rhs.signal_id;
        });
        return result;
    }

    inline SignalRecordDBListResult SignalRecordDB::find_records_no_lock() const {
        if (!is_open_no_lock()) {
            return signal_detail::list_error(SignalRecordDBStatus::NOT_OPEN, "SignalRecordDB is not open");
        }

        const auto start_key = signal_detail::make_composite_key(std::numeric_limits<std::int32_t>::min(), 0);
        const auto stop_key = signal_detail::make_composite_key(
            std::numeric_limits<std::int32_t>::max(),
            0xFFFFFFFFu);

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        SignalRecordDBListResult result;
        result.status = SignalRecordDBStatus::SUCCESS;

        m_records->for_each_range(
            start_key,
            stop_key,
            [&result](const std::uint64_t&, const SignalRecord& record) {
                result.records.push_back(record);
                return true;
            },
            txn.handle());

        txn.commit();

        std::sort(result.records.begin(), result.records.end(), [](const SignalRecord& lhs, const SignalRecord& rhs) {
            const auto lhs_timestamp = signal_detail::selected_timestamp_ms(lhs);
            const auto rhs_timestamp = signal_detail::selected_timestamp_ms(rhs);
            if (lhs_timestamp != rhs_timestamp) return lhs_timestamp < rhs_timestamp;
            return lhs.signal_id < rhs.signal_id;
        });
        return result;
    }

    inline SignalRecordDBStatus SignalRecordDB::erase_by_signal_id_no_lock(std::uint32_t signal_id) {
        if (!is_open_no_lock()) return SignalRecordDBStatus::NOT_OPEN;
        if (m_read_only) return SignalRecordDBStatus::READ_ONLY;
        if (signal_id == 0) return SignalRecordDBStatus::INVALID_ARGUMENT;

        auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);
        const auto composite = m_signal_id_index->find(signal_id, txn);
        if (!composite) {
            txn.commit();
            return SignalRecordDBStatus::NOT_FOUND;
        }

        const auto record = m_records->find(*composite, txn);
        if (!record) {
            m_signal_id_index->erase(signal_id, txn);
            update_last_update_no_lock(txn.handle());
            txn.commit();
            return SignalRecordDBStatus::NOT_FOUND;
        }

        remove_indexes_no_lock(*record, txn.handle());
        m_records->erase(*composite, txn);
        update_last_update_no_lock(txn.handle());
        txn.commit();
        return SignalRecordDBStatus::SUCCESS;
    }

    inline SignalRecordDBStatus SignalRecordDB::clear_no_lock() {
        if (!is_open_no_lock()) return SignalRecordDBStatus::NOT_OPEN;
        if (m_read_only) return SignalRecordDBStatus::READ_ONLY;

        auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);
        m_records->clear(txn);
        m_signal_id_index->clear(txn);
        m_trade_id_index->clear(txn);
        m_meta->clear(txn);
        init_meta_no_lock(txn.handle());
        update_last_update_no_lock(txn.handle());
        txn.commit();
        return SignalRecordDBStatus::SUCCESS;
    }

    inline bool SignalRecordDB::worker_running() const {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        return m_worker_running;
    }

    inline SignalRecordDBStatus SignalRecordDB::enqueue_command(std::function<void()> command) {
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (!m_accepting) return SignalRecordDBStatus::QUEUE_CLOSED;
            m_work_queue.push_back(std::move(command));
        }
        m_work_cv.notify_one();
        return SignalRecordDBStatus::SUCCESS;
    }

    inline void SignalRecordDB::enqueue_callback(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_callback_queue.push_back(std::move(callback));
    }

    inline bool SignalRecordDB::pop_command(std::function<void()>& command) {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_work_queue.empty()) return false;
        command = std::move(m_work_queue.front());
        m_work_queue.pop_front();
        ++m_active_work;
        return true;
    }

    inline void SignalRecordDB::finish_command() {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_active_work > 0) --m_active_work;
        if (m_work_queue.empty() && m_active_work == 0) {
            m_idle_cv.notify_all();
        }
    }

    inline void SignalRecordDB::execute_command(std::function<void()> command) {
        try {
            if (command) command();
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("SignalRecordDB queued command error: ", ex);
        } catch (...) {
            LOGIT_PRINT_ERROR("SignalRecordDB queued command unknown error");
        }
        finish_command();
    }

    inline void SignalRecordDB::drain_work_on_caller() {
        std::function<void()> command;
        while (pop_command(command)) {
            execute_command(std::move(command));
        }
    }

    inline void SignalRecordDB::deliver_callbacks() {
        for (;;) {
            std::function<void()> callback;
            {
                std::lock_guard<std::mutex> lock(m_callback_mutex);
                if (m_callback_queue.empty()) break;
                callback = std::move(m_callback_queue.front());
                m_callback_queue.pop_front();
            }
            try {
                if (callback) callback();
            } catch (const std::exception& ex) {
                LOGIT_PRINT_ERROR("SignalRecordDB callback error: ", ex);
            } catch (...) {
                LOGIT_PRINT_ERROR("SignalRecordDB callback unknown error");
            }
        }
    }

    inline void SignalRecordDB::worker_loop() {
        for (;;) {
            std::function<void()> command;
            {
                std::unique_lock<std::mutex> lock(m_queue_mutex);
                m_work_cv.wait(lock, [this]() {
                    return m_worker_stop || !m_work_queue.empty();
                });
                if (m_worker_stop && m_work_queue.empty()) {
                    break;
                }
                command = std::move(m_work_queue.front());
                m_work_queue.pop_front();
                ++m_active_work;
            }
            execute_command(std::move(command));
        }

        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_worker_running = false;
        if (m_work_queue.empty() && m_active_work == 0) {
            m_idle_cv.notify_all();
        }
    }

} // namespace optionx::storage

#endif // OPTIONX_HEADER_STORAGES_SIGNAL_RECORD_DB_SIGNAL_RECORD_DB_IPP_INCLUDED
