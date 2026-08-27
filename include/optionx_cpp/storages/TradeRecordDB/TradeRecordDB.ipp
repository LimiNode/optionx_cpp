#pragma once
#ifndef OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_RECORD_DB_IPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_RECORD_DB_IPP_INCLUDED

/// \file TradeRecordDB.ipp
/// \brief Inline implementation of TradeRecordDB.

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

#include <time_shield.hpp>

#include <optionx_cpp/utils.hpp>

namespace optionx::storage {

    inline mdbxc::Config TradeRecordDB::default_config() {
        mdbxc::Config config;
        config.pathname = OPTIONX_TRADE_RECORD_DB_FILE;
        config.max_dbs = 4;
        config.no_subdir = false;
        config.relative_to_exe = true;
        return config;
    }

    inline TradeRecordDB::TradeRecordDB()
        : TradeRecordDB(default_config()) {}

    inline TradeRecordDB::TradeRecordDB(
        mdbxc::Config config,
        std::string records_table,
        std::string uid_index_table,
        std::string trade_id_index_table,
        std::string meta_table)
        : m_config(std::move(config)),
          m_records_table_name(std::move(records_table)),
          m_uid_index_table_name(std::move(uid_index_table)),
          m_trade_id_index_table_name(std::move(trade_id_index_table)),
          m_meta_table_name(std::move(meta_table)) {
        open();
    }

    inline TradeRecordDB::~TradeRecordDB() {
        shutdown();
    }

    inline bool TradeRecordDB::is_open() const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        return is_open_no_lock();
    }

    inline std::uint32_t TradeRecordDB::get_trade_id() {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            if (!is_open_no_lock()) return 0;
            if (m_read_only) return 0;

            auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);
            const auto trade_id = reserve_trade_id_no_lock(txn.handle());
            if (trade_id == 0) return 0;
            update_last_update_no_lock(txn.handle());
            txn.commit();
            return trade_id;
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB trade_id database error: ", ex);
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB trade_id error: ", ex);
        }
        return 0;
    }

    inline bool TradeRecordDB::assign_trade_id(TradeRequest& request) {
        if (request.trade_id > 0) return true;
        request.trade_id = get_trade_id();
        return request.trade_id > 0;
    }

    inline std::int64_t TradeRecordDB::get_trade_uid() {
        const auto trade_id = get_trade_id();
        return static_cast<std::int64_t>(trade_id);
    }

    inline bool TradeRecordDB::assign_trade_uid(TradeRequest& request) {
        return assign_trade_id(request);
    }

    inline TradeRecordDBWriteResult TradeRecordDB::upsert(TradeRecord record) {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return upsert_no_lock(std::move(record));
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB upsert database error: ", ex);
            return detail::write_error(TradeRecordDBStatus::DB_ERROR, std::move(record), ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB upsert error: ", ex);
            return detail::write_error(TradeRecordDBStatus::DB_ERROR, std::move(record), ex.what());
        }
    }

    inline TradeRecordDBWriteResult TradeRecordDB::write(TradeRecord record) {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return write_no_lock(std::move(record));
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB write database error: ", ex);
            return detail::write_error(TradeRecordDBStatus::DB_ERROR, std::move(record), ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB write error: ", ex);
            return detail::write_error(TradeRecordDBStatus::DB_ERROR, std::move(record), ex.what());
        }
    }

    inline TradeRecordDBReadResult TradeRecordDB::find_by_trade_id(std::uint32_t trade_id) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_by_trade_id_no_lock(trade_id);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_by_trade_id database error: ", ex);
            return detail::read_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_by_trade_id error: ", ex);
            return detail::read_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline TradeRecordDBReadResult TradeRecordDB::find_by_uid(std::int64_t unique_id) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_by_uid_no_lock(unique_id);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_by_uid database error: ", ex);
            return detail::read_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_by_uid error: ", ex);
            return detail::read_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline TradeRecordDBListResult TradeRecordDB::find_by_timestamp(std::int64_t timestamp_ms) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_by_timestamp_no_lock(timestamp_ms);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_by_timestamp database error: ", ex);
            return detail::list_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_by_timestamp error: ", ex);
            return detail::list_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline TradeRecordDBListResult TradeRecordDB::find_range(std::int64_t start_ms, std::int64_t stop_ms) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_range_no_lock(start_ms, stop_ms);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_range database error: ", ex);
            return detail::list_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_range error: ", ex);
            return detail::list_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline TradeRecordDBListResult TradeRecordDB::find_records(const optionx::TradeRecordQuery& query) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_records_no_lock(query);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_records database error: ", ex);
            return detail::list_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_records error: ", ex);
            return detail::list_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline std::vector<optionx::TradeRecord> TradeRecordDB::find_records_vector(const optionx::TradeRecordQuery& query) const {
        auto result = find_records(query);
        if (!result.ok()) {
            return {};
        }
        return std::move(result.records);
    }

    inline TradeRecordDBListResult TradeRecordDB::find_today(
            std::int64_t now_ms,
            const optionx::TradeTimeZone& time_zone) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_today_no_lock(now_ms, time_zone);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_today database error: ", ex);
            return detail::list_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_today error: ", ex);
            return detail::list_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline TradeRecordDBListResult TradeRecordDB::find_today(
            std::int64_t now_ms,
            std::int64_t time_zone_sec) const {
        return find_today(now_ms, optionx::TradeTimeZone::fixed_offset(time_zone_sec));
    }

    inline TradeRecordDBListResult TradeRecordDB::find_day(
            std::int64_t day_start_ms,
            const optionx::TradeTimeZone& time_zone) const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return find_day_no_lock(day_start_ms, time_zone);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_day database error: ", ex);
            return detail::list_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB find_day error: ", ex);
            return detail::list_error(TradeRecordDBStatus::DB_ERROR, ex.what());
        }
    }

    inline TradeRecordDBListResult TradeRecordDB::find_day(
            std::int64_t day_start_ms,
            std::int64_t time_zone_sec) const {
        return find_day(day_start_ms, optionx::TradeTimeZone::fixed_offset(time_zone_sec));
    }

    inline TradeRecordDBStatus TradeRecordDB::erase_by_trade_id(std::uint32_t trade_id) {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return erase_by_trade_id_no_lock(trade_id);
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB erase_by_trade_id database error: ", ex);
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB erase_by_trade_id error: ", ex);
        }
        return TradeRecordDBStatus::DB_ERROR;
    }

    inline TradeRecordDBStatus TradeRecordDB::clear() {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            return clear_no_lock();
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB clear database error: ", ex);
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB clear error: ", ex);
        }
        return TradeRecordDBStatus::DB_ERROR;
    }

    inline std::size_t TradeRecordDB::count() const {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            if (!is_open_no_lock()) return 0;
            return m_records->count();
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB count database error: ", ex);
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB count error: ", ex);
        }
        return 0;
    }

    inline TradeRecordDBStatus TradeRecordDB::enqueue_upsert(TradeRecord record, write_callback_t callback) {
        return enqueue_command([this, record = std::move(record), callback = std::move(callback)]() mutable {
            auto result = upsert(std::move(record));
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline TradeRecordDBStatus TradeRecordDB::enqueue_write(TradeRecord record, write_callback_t callback) {
        return enqueue_command([this, record = std::move(record), callback = std::move(callback)]() mutable {
            auto result = write(std::move(record));
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline TradeRecordDBStatus TradeRecordDB::enqueue_find_by_trade_id(std::uint32_t trade_id, read_callback_t callback) {
        return enqueue_command([this, trade_id, callback = std::move(callback)]() mutable {
            auto result = find_by_trade_id(trade_id);
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline TradeRecordDBStatus TradeRecordDB::enqueue_find_by_uid(
        std::int64_t unique_id,
        read_callback_t callback) {
        return enqueue_command([this, unique_id, callback = std::move(callback)]() mutable {
            auto result = find_by_uid(unique_id);
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline TradeRecordDBStatus TradeRecordDB::enqueue_find_by_timestamp(
        std::int64_t timestamp_ms,
        list_callback_t callback) {
        return enqueue_command([this, timestamp_ms, callback = std::move(callback)]() mutable {
            auto result = find_by_timestamp(timestamp_ms);
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline TradeRecordDBStatus TradeRecordDB::enqueue_find_range(
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

    inline TradeRecordDBStatus TradeRecordDB::enqueue_find_records(optionx::TradeRecordQuery query, list_callback_t callback) {
        return enqueue_command([this, query = std::move(query), callback = std::move(callback)]() mutable {
            auto result = find_records(query);
            enqueue_callback([callback = std::move(callback), result = std::move(result)]() mutable {
                if (callback) callback(std::move(result));
            });
        });
    }

    inline TradeRecordDBStatus TradeRecordDB::enqueue_erase_by_trade_id(std::uint32_t trade_id, status_callback_t callback) {
        return enqueue_command([this, trade_id, callback = std::move(callback)]() mutable {
            auto status = erase_by_trade_id(trade_id);
            enqueue_callback([callback = std::move(callback), status]() mutable {
                if (callback) callback(status);
            });
        });
    }

    inline TradeRecordDBStatus TradeRecordDB::enqueue_clear(status_callback_t callback) {
        return enqueue_command([this, callback = std::move(callback)]() mutable {
            auto status = clear();
            enqueue_callback([callback = std::move(callback), status]() mutable {
                if (callback) callback(status);
            });
        });
    }

    inline bool TradeRecordDB::run() {
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
            LOGIT_PRINT_ERROR("TradeRecordDB worker start error: ", ex);
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_worker_running = false;
            return false;
        }
        return true;
    }

    inline void TradeRecordDB::process() {
        if (!worker_running()) {
            drain_work_on_caller();
        }
        deliver_callbacks();
    }

    inline void TradeRecordDB::flush() {
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

    inline void TradeRecordDB::shutdown() {
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
        try {
            m_records.reset();
            m_uid_index.reset();
            m_trade_id_index.reset();
            m_meta.reset();
            if (m_connection && m_connection->is_connected()) {
                m_connection->shutdown();
            }
            m_connection.reset();
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB shutdown database error: ", ex);
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB shutdown error: ", ex);
        }
        m_open = false;
    }

    inline void TradeRecordDB::open() {
        std::lock_guard<std::mutex> lock(m_db_mutex);
        try {
            if (m_config.max_dbs < 4) {
                m_config.max_dbs = 4;
            }
            m_read_only = m_config.read_only;
            m_connection = mdbxc::Connection::create(m_config);
            m_records = std::make_unique<records_table_t>(m_connection, m_records_table_name);
            m_uid_index = std::make_unique<uid_index_table_t>(m_connection, m_uid_index_table_name);
            m_trade_id_index = std::make_unique<trade_id_index_table_t>(m_connection, m_trade_id_index_table_name);
            m_meta = std::make_unique<meta_table_t>(m_connection, m_meta_table_name);
            m_open = true;

            if (!m_read_only) {
                auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);
                init_meta_no_lock(txn.handle());
                txn.commit();
            }
        } catch (const mdbxc::MdbxException& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB connection error: ", ex);
            m_open = false;
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB initialization error: ", ex);
            m_open = false;
        }
    }

    inline bool TradeRecordDB::is_open_no_lock() const noexcept {
        return m_open && m_connection && m_connection->is_connected() &&
               m_records && m_uid_index && m_trade_id_index && m_meta;
    }

    inline void TradeRecordDB::init_meta_no_lock(MDBX_txn* txn) {
        auto meta = m_meta->get_or(TradeRecordDBMeta{}, txn);
        if (meta.db_version == 0) {
            meta.db_version = 1;
        }
        if (meta.next_trade_uid == 0) {
            meta.next_trade_uid = 1;
        }
        m_meta->set(meta, txn);
    }

    inline void TradeRecordDB::update_last_update_no_lock(MDBX_txn* txn) {
        auto meta = m_meta->get_or(TradeRecordDBMeta{}, txn);
        meta.last_update_ms = time_shield::timestamp_ms();
        m_meta->set(meta, txn);
    }

    inline std::uint32_t TradeRecordDB::reserve_trade_id_no_lock(MDBX_txn* txn) {
        constexpr auto max_id = std::numeric_limits<std::uint32_t>::max();
        auto meta = m_meta->get_or(TradeRecordDBMeta{}, txn);
        auto candidate = meta.next_trade_uid;
        if (candidate == 0) candidate = 1;

        const auto start = candidate;
        bool wrapped = false;
        while (candidate != 0 && m_trade_id_index->contains(candidate, txn)) {
            if (++candidate == 0) {
                candidate = 1;
                wrapped = true;
            }
            if (wrapped && candidate == start) {
                return 0;
            }
        }
        if (candidate == 0) return 0;

        meta.next_trade_uid = candidate + 1;
        if (meta.next_trade_uid == 0) meta.next_trade_uid = 1;
        m_meta->set(meta, txn);
        return candidate;
    }

    inline void TradeRecordDB::bump_next_trade_id_no_lock(std::uint32_t used_trade_id, MDBX_txn* txn) {
        constexpr auto max_uint32 = std::numeric_limits<std::uint32_t>::max();
        if (used_trade_id == 0) return;

        auto meta = m_meta->get_or(TradeRecordDBMeta{}, txn);
        if (meta.next_trade_uid == 0) meta.next_trade_uid = 1;
        const auto next = meta.next_trade_uid;
        if (next <= used_trade_id) {
            if (used_trade_id == max_uint32) {
                meta.next_trade_uid = 1;
            } else {
                meta.next_trade_uid = static_cast<std::uint32_t>(used_trade_id + 1);
            }
            m_meta->set(meta, txn);
        }
    }

    inline TradeRecordDBWriteResult TradeRecordDB::write_no_lock(TradeRecord record) {
        if (!is_open_no_lock()) {
            return detail::write_error(
                TradeRecordDBStatus::NOT_OPEN,
                std::move(record),
                "TradeRecordDB is not open");
        }
        if (m_read_only) {
            return detail::write_error(
                TradeRecordDBStatus::READ_ONLY,
                std::move(record),
                "TradeRecordDB is read-only");
        }
        if (record.trade_id == 0) {
            return detail::write_error(
                TradeRecordDBStatus::INVALID_ARGUMENT,
                std::move(record),
                "TradeRecord trade_id is required");
        }
        auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);

        const auto existing_composite_opt = m_trade_id_index->find(record.trade_id, txn);
        const auto ts_ms = detail::selected_timestamp_ms(record);
        const auto unix_minutes = detail::timestamp_ms_to_unix_minutes(ts_ms);
        const auto new_composite_key = detail::make_composite_key(
            unix_minutes,
            record.trade_id);

        if (existing_composite_opt && *existing_composite_opt != new_composite_key) {
            const auto old_record = m_records->find(*existing_composite_opt, txn);
            if (old_record && old_record->unique_id > 0 &&
                old_record->unique_id != record.unique_id) {
                m_uid_index->erase(old_record->unique_id, txn);
            }
            m_records->erase(*existing_composite_opt, txn);
        }

        m_records->insert_or_assign(new_composite_key, record, txn);
        if (record.unique_id > 0) {
            m_uid_index->insert_or_assign(record.unique_id, new_composite_key, txn);
        }
        m_trade_id_index->insert_or_assign(record.trade_id, new_composite_key, txn);
        bump_next_trade_id_no_lock(record.trade_id, txn.handle());
        update_last_update_no_lock(txn.handle());
        txn.commit();

        return detail::write_success(std::move(record));
    }

    inline TradeRecordDBWriteResult TradeRecordDB::upsert_no_lock(TradeRecord record) {
        if (!is_open_no_lock()) {
            return detail::write_error(
                TradeRecordDBStatus::NOT_OPEN,
                std::move(record),
                "TradeRecordDB is not open");
        }
        if (m_read_only) {
            return detail::write_error(
                TradeRecordDBStatus::READ_ONLY,
                std::move(record),
                "TradeRecordDB is read-only");
        }

        auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);

        std::uint32_t selected_trade_id = record.trade_id;

        if (selected_trade_id == 0 && record.unique_id > 0) {
            const auto existing_composite = m_uid_index->find(record.unique_id, txn);
            if (existing_composite) {
                const auto existing_record = m_records->find(*existing_composite, txn);
                if (existing_record) {
                    selected_trade_id = existing_record->trade_id;
                } else {
                    m_uid_index->erase(record.unique_id, txn);
                }
            }
        }

        if (selected_trade_id == 0 && record.has_broker_identity()) {
            selected_trade_id = find_broker_identity_key_no_lock(record, txn.handle());
        }

        if (selected_trade_id == 0) {
            selected_trade_id = reserve_trade_id_no_lock(txn.handle());
            if (selected_trade_id == 0) {
                return detail::write_error(
                    TradeRecordDBStatus::DB_ERROR,
                    std::move(record),
                    "TradeRecordDB could not reserve a trade_id");
            }
        }
        record.trade_id = selected_trade_id;

        const auto existing_composite_opt = m_trade_id_index->find(selected_trade_id, txn);
        const auto ts_ms = detail::selected_timestamp_ms(record);
        const auto unix_minutes = detail::timestamp_ms_to_unix_minutes(ts_ms);
        const auto new_composite_key = detail::make_composite_key(
            unix_minutes,
            selected_trade_id);

        if (existing_composite_opt && *existing_composite_opt != new_composite_key) {
            const auto old_record = m_records->find(*existing_composite_opt, txn);
            if (old_record && old_record->unique_id > 0 &&
                old_record->unique_id != record.unique_id) {
                m_uid_index->erase(old_record->unique_id, txn);
            }
            m_records->erase(*existing_composite_opt, txn);
        }

        m_records->insert_or_assign(new_composite_key, record, txn);
        if (record.unique_id > 0) {
            m_uid_index->insert_or_assign(record.unique_id, new_composite_key, txn);
        }
        m_trade_id_index->insert_or_assign(selected_trade_id, new_composite_key, txn);
        bump_next_trade_id_no_lock(selected_trade_id, txn.handle());
        update_last_update_no_lock(txn.handle());
        txn.commit();

        return detail::write_success(std::move(record));
    }

    inline TradeRecordDBReadResult TradeRecordDB::find_by_trade_id_no_lock(std::uint32_t trade_id) const {
        if (!is_open_no_lock()) {
            return detail::read_error(TradeRecordDBStatus::NOT_OPEN, "TradeRecordDB is not open");
        }
        if (trade_id == 0) {
            return detail::read_error(TradeRecordDBStatus::INVALID_ARGUMENT, "TradeRecord trade_id is required");
        }
        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        const auto composite_opt = m_trade_id_index->find(trade_id, txn);
        if (!composite_opt) {
            txn.commit();
            return detail::read_error(TradeRecordDBStatus::NOT_FOUND, "TradeRecord was not found");
        }

        const auto record = m_records->find(*composite_opt, txn);
        txn.commit();
        if (!record) {
            return detail::read_error(TradeRecordDBStatus::NOT_FOUND, "TradeRecord index points to missing record");
        }

        TradeRecordDBReadResult result;
        result.status = TradeRecordDBStatus::SUCCESS;
        result.record = *record;
        result.found = true;
        return result;
    }

    inline TradeRecordDBReadResult TradeRecordDB::find_by_uid_no_lock(std::int64_t unique_id) const {
        if (!is_open_no_lock()) {
            return detail::read_error(TradeRecordDBStatus::NOT_OPEN, "TradeRecordDB is not open");
        }
        if (unique_id <= 0) {
            return detail::read_error(
                TradeRecordDBStatus::INVALID_ARGUMENT,
                "TradeRecord unique_id is required");
        }

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        const auto composite_opt = m_uid_index->find(unique_id, txn);
        if (!composite_opt) {
            txn.commit();
            return detail::read_error(
                TradeRecordDBStatus::NOT_FOUND,
                "TradeRecord UID index entry was not found");
        }

        const auto record = m_records->find(*composite_opt, txn);
        txn.commit();
        if (!record) {
            return detail::read_error(
                TradeRecordDBStatus::NOT_FOUND,
                "TradeRecord UID index points to missing record");
        }
        if (record->unique_id != unique_id) {
            return detail::read_error(
                TradeRecordDBStatus::NOT_FOUND,
                "TradeRecord UID index points to a different unique_id");
        }

        TradeRecordDBReadResult result;
        result.status = TradeRecordDBStatus::SUCCESS;
        result.record = *record;
        result.found = true;
        return result;
    }

    inline TradeRecordDBListResult TradeRecordDB::find_by_timestamp_no_lock(std::int64_t timestamp_ms) const {
        if (!is_open_no_lock()) {
            return detail::list_error(TradeRecordDBStatus::NOT_OPEN, "TradeRecordDB is not open");
        }
        if (timestamp_ms < 0) {
            return detail::list_error(TradeRecordDBStatus::INVALID_ARGUMENT, "TradeRecord timestamp is invalid");
        }

        const auto target_min = detail::timestamp_ms_to_unix_minutes(timestamp_ms);
        const auto start_key = detail::make_composite_key(target_min, 0);
        const auto stop_key = detail::make_composite_key(target_min, 0xFFFFFFFFu);

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        TradeRecordDBListResult result;
        result.status = TradeRecordDBStatus::SUCCESS;

        m_records->for_each_range(start_key, stop_key, [&result, timestamp_ms](const std::uint64_t&, const TradeRecord& rec) {
            if (detail::selected_timestamp_ms(rec) == timestamp_ms) {
                result.records.push_back(rec);
            }
            return true;
        }, txn.handle());

        txn.commit();

        std::sort(result.records.begin(), result.records.end(), [](const TradeRecord& lhs, const TradeRecord& rhs) {
            const auto lhs_timestamp = detail::selected_timestamp_ms(lhs);
            const auto rhs_timestamp = detail::selected_timestamp_ms(rhs);
            if (lhs_timestamp != rhs_timestamp) return lhs_timestamp < rhs_timestamp;
            return lhs.trade_id < rhs.trade_id;
        });
        return result;
    }

    inline TradeRecordDBListResult TradeRecordDB::find_range_no_lock(std::int64_t start_ms, std::int64_t stop_ms) const {
        if (!is_open_no_lock()) {
            return detail::list_error(TradeRecordDBStatus::NOT_OPEN, "TradeRecordDB is not open");
        }
        if (start_ms < 0 || stop_ms < 0 || start_ms > stop_ms) {
            return detail::list_error(TradeRecordDBStatus::INVALID_ARGUMENT, "TradeRecord timestamp range is invalid");
        }

        const auto start_min = detail::timestamp_ms_to_unix_minutes(start_ms);
        const auto stop_min = detail::timestamp_ms_to_unix_minutes(stop_ms);
        const auto start_key = detail::make_composite_key(start_min, 0);
        const auto stop_key = detail::make_composite_key(stop_min, 0xFFFFFFFFu);

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);
        TradeRecordDBListResult result;
        result.status = TradeRecordDBStatus::SUCCESS;

        m_records->for_each_range(start_key, stop_key, [&result, start_ms, stop_ms](const std::uint64_t&, const TradeRecord& rec) {
            const auto timestamp = detail::selected_timestamp_ms(rec);
            if (timestamp >= start_ms && timestamp <= stop_ms) {
                result.records.push_back(rec);
            }
            return true;
        }, txn.handle());

        txn.commit();

        std::sort(result.records.begin(), result.records.end(), [](const TradeRecord& lhs, const TradeRecord& rhs) {
            const auto lhs_timestamp = detail::selected_timestamp_ms(lhs);
            const auto rhs_timestamp = detail::selected_timestamp_ms(rhs);
            if (lhs_timestamp != rhs_timestamp) return lhs_timestamp < rhs_timestamp;
            return lhs.trade_id < rhs.trade_id;
        });
        return result;
    }

    inline TradeRecordDBListResult TradeRecordDB::find_records_no_lock(const optionx::TradeRecordQuery& query) const {
        if (!is_open_no_lock()) {
            return detail::list_error(TradeRecordDBStatus::NOT_OPEN, "TradeRecordDB is not open");
        }

        TradeRecordDBListResult result;
        result.status = TradeRecordDBStatus::SUCCESS;

        // Determine coarse scan range for the MDBX composite-key index.
        std::int64_t coarse_start_ms = query.start_ms;
        std::int64_t coarse_stop_ms = query.stop_ms;

        if (query.range_mode == optionx::TimeRangeMode::NONE) {
            coarse_start_ms = 0;
            coarse_stop_ms = std::numeric_limits<std::int64_t>::max();
        } else if (query.time_field != optionx::TradeRecordTimeField::AUTO &&
                   query.time_field != optionx::TradeRecordTimeField::PLACE_DATE) {
            // Composite key is built from selected_timestamp_ms (place_date first).
            // When the user queries by a different field, the record's composite key
            // bucket may differ from the queried timestamp. Expand scan by
            // coarse_expansion_ms on both sides to avoid missing records.
            const auto exp = query.coarse_expansion_ms;
            coarse_start_ms = (query.start_ms > exp) ? (query.start_ms - exp) : 0LL;
            if (query.stop_ms < std::numeric_limits<std::int64_t>::max() - exp) {
                coarse_stop_ms = query.stop_ms + exp;
            } else {
                coarse_stop_ms = std::numeric_limits<std::int64_t>::max();
            }
        }

        const auto start_min =
            query.range_mode == optionx::TimeRangeMode::NONE
                ? std::numeric_limits<std::int32_t>::min()
                : detail::timestamp_ms_to_unix_minutes(coarse_start_ms);
        const auto stop_min =
            query.range_mode == optionx::TimeRangeMode::NONE
                ? std::numeric_limits<std::int32_t>::max()
                : detail::timestamp_ms_to_unix_minutes(coarse_stop_ms);
        const auto start_key = detail::make_composite_key(start_min, 0);
        const auto stop_key = detail::make_composite_key(stop_min, 0xFFFFFFFFu);

        auto txn = m_connection->transaction(mdbxc::TransactionMode::READ_ONLY);

        m_records->for_each_range(start_key, stop_key, [&](const std::uint64_t&, const TradeRecord& rec) {
            if (TradeRecordFilterMatcher::match(rec, query)) {
                result.records.push_back(rec);
            }
            return true;
        }, txn.handle());

        txn.commit();

        // Apply stale-status correction in-memory.
        if (query.fix_stale_status) {
            TradeRecordStatusFixer::fix_stale_statuses(result.records, query.wait_status_sec * 1000);
        }

        // Sort by the user-selected timestamp.
        if (query.ascending) {
            std::sort(result.records.begin(), result.records.end(),
                [&query](const TradeRecord& lhs, const TradeRecord& rhs) {
                    const auto lhs_ts = optionx::select_timestamp_ms(lhs, query.time_field);
                    const auto rhs_ts = optionx::select_timestamp_ms(rhs, query.time_field);
                    if (lhs_ts != rhs_ts) return lhs_ts < rhs_ts;
                    return lhs.trade_id < rhs.trade_id;
                });
        } else {
            std::sort(result.records.begin(), result.records.end(),
                [&query](const TradeRecord& lhs, const TradeRecord& rhs) {
                    const auto lhs_ts = optionx::select_timestamp_ms(lhs, query.time_field);
                    const auto rhs_ts = optionx::select_timestamp_ms(rhs, query.time_field);
                    if (lhs_ts != rhs_ts) return lhs_ts > rhs_ts;
                    return lhs.trade_id > rhs.trade_id;
                });
        }

        // Apply offset / limit pagination.
        if (query.offset > 0) {
            if (query.offset >= result.records.size()) {
                result.records.clear();
            } else {
                result.records.erase(result.records.begin(), result.records.begin() + query.offset);
            }
        }
        if (query.limit > 0 && result.records.size() > query.limit) {
            result.records.resize(query.limit);
        }

        return result;
    }

    inline TradeRecordDBListResult TradeRecordDB::find_today_no_lock(
            std::int64_t now_ms,
            const optionx::TradeTimeZone& time_zone) const {
        return find_day_no_lock(now_ms, time_zone);
    }

    inline TradeRecordDBListResult TradeRecordDB::find_day_no_lock(
            std::int64_t day_start_ms,
            const optionx::TradeTimeZone& time_zone) const {
        optionx::TradeRecordQuery query;
        query.time_field = optionx::TradeRecordTimeField::AUTO;
        query.range_mode = optionx::TimeRangeMode::HALF_OPEN;
        query.time_zone = time_zone;

        query.start_ms = time_zone.start_of_local_day_utc_ms(day_start_ms);
        query.stop_ms = time_zone.next_local_day_start_utc_ms(day_start_ms);

        return find_records_no_lock(query);
    }

    inline TradeRecordDBStatus TradeRecordDB::erase_by_trade_id_no_lock(std::uint32_t trade_id) {
        if (!is_open_no_lock()) return TradeRecordDBStatus::NOT_OPEN;
        if (m_read_only) return TradeRecordDBStatus::READ_ONLY;
        if (trade_id == 0) return TradeRecordDBStatus::INVALID_ARGUMENT;
        auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);
        const auto composite_opt = m_trade_id_index->find(trade_id, txn);
        if (!composite_opt) {
            txn.commit();
            return TradeRecordDBStatus::NOT_FOUND;
        }

        const auto record = m_records->find(*composite_opt, txn);
        if (record && record->unique_id > 0) {
            m_uid_index->erase(record->unique_id, txn);
        }
        m_records->erase(*composite_opt, txn);
        m_trade_id_index->erase(trade_id, txn);
        update_last_update_no_lock(txn.handle());
        txn.commit();
        return TradeRecordDBStatus::SUCCESS;
    }

    inline TradeRecordDBStatus TradeRecordDB::clear_no_lock() {
        if (!is_open_no_lock()) return TradeRecordDBStatus::NOT_OPEN;
        if (m_read_only) return TradeRecordDBStatus::READ_ONLY;

        auto txn = m_connection->transaction(mdbxc::TransactionMode::WRITABLE);
        m_records->clear(txn);
        m_uid_index->clear(txn);
        m_trade_id_index->clear(txn);
        m_meta->clear(txn);
        init_meta_no_lock(txn.handle());
        update_last_update_no_lock(txn.handle());
        txn.commit();
        return TradeRecordDBStatus::SUCCESS;
    }

    inline std::uint32_t TradeRecordDB::find_broker_identity_key_no_lock(
        const TradeRecord& record,
        MDBX_txn* txn) const {
        std::vector<std::pair<std::uint64_t, TradeRecord>> rows;
        m_records->load(rows, txn);
        for (const auto& row : rows) {
            if (record.same_broker_identity(row.second)) {
                return row.second.trade_id;
            }
        }
        return 0;
    }

    inline bool TradeRecordDB::worker_running() const {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        return m_worker_running;
    }

    inline TradeRecordDBStatus TradeRecordDB::enqueue_command(std::function<void()> command) {
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (!m_accepting) return TradeRecordDBStatus::QUEUE_CLOSED;
            m_work_queue.push_back(std::move(command));
        }
        m_work_cv.notify_one();
        return TradeRecordDBStatus::SUCCESS;
    }

    inline void TradeRecordDB::enqueue_callback(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_callback_queue.push_back(std::move(callback));
    }

    inline bool TradeRecordDB::pop_command(std::function<void()>& command) {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_work_queue.empty()) return false;
        command = std::move(m_work_queue.front());
        m_work_queue.pop_front();
        ++m_active_work;
        return true;
    }

    inline void TradeRecordDB::finish_command() {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_active_work > 0) --m_active_work;
        if (m_work_queue.empty() && m_active_work == 0) {
            m_idle_cv.notify_all();
        }
    }

    inline void TradeRecordDB::execute_command(std::function<void()> command) {
        try {
            if (command) command();
        } catch (const std::exception& ex) {
            LOGIT_PRINT_ERROR("TradeRecordDB queued command error: ", ex);
        } catch (...) {
            LOGIT_PRINT_ERROR("TradeRecordDB queued command unknown error");
        }
        finish_command();
    }

    inline void TradeRecordDB::drain_work_on_caller() {
        std::function<void()> command;
        while (pop_command(command)) {
            execute_command(std::move(command));
        }
    }

    inline void TradeRecordDB::deliver_callbacks() {
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
                LOGIT_PRINT_ERROR("TradeRecordDB callback error: ", ex);
            } catch (...) {
                LOGIT_PRINT_ERROR("TradeRecordDB callback unknown error");
            }
        }
    }

    inline void TradeRecordDB::worker_loop() {
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

#endif // OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_RECORD_DB_IPP_INCLUDED
