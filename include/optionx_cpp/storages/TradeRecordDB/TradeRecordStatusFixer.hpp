#pragma once
#ifndef OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_RECORD_STATUS_FIXER_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_RECORD_STATUS_FIXER_HPP_INCLUDED

/// \file TradeRecordStatusFixer.hpp
/// \brief In-memory correction of stale trade statuses.

#include <vector>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <limits>

#include <optionx_cpp/data/trading.hpp>

namespace optionx::storage {

    /// \class TradeRecordStatusFixer
    /// \brief Corrects statuses of trades that appear stuck in non-terminal states.
    ///
    /// This class operates strictly on in-memory vectors. It never writes back to the DB.
    class TradeRecordStatusFixer {
    public:
        /// \brief Callback type for attempting to resolve a stuck trade via external logic.
        /// \param record The stale trade record.
        /// \return A TradeResult that can be merged into the record, or an empty/invalid result if resolution failed.
        using resolve_callback_t = std::function<optionx::TradeResult(const optionx::TradeRecord&)>;

        /// \brief Fixes stale statuses in a vector of records without an external resolver.
        /// \param records Records to inspect and modify in place.
        /// \param wait_status_ms Maximum time to wait for a trade to reach terminal state.
        static void fix_stale_statuses(
                std::vector<optionx::TradeRecord>& records,
                std::int64_t wait_status_ms) {
            fix_stale_statuses(records, wait_status_ms, nullptr);
        }

        /// \brief Fixes stale statuses with an optional external resolver callback.
        /// \param records Records to inspect and modify in place.
        /// \param wait_status_ms Maximum time to wait for a trade to reach terminal state.
        /// \param callback Optional callback to attempt resolving the result of a stale trade.
        static void fix_stale_statuses(
                std::vector<optionx::TradeRecord>& records,
                std::int64_t wait_status_ms,
                resolve_callback_t callback) {
            if (records.empty()) return;

            // 1. Determine the maximum close_date among terminal trades.
            std::int64_t max_close_date = 0;
            for (const auto& rec : records) {
                if (optionx::is_terminal_trade_state(rec.trade_state) && rec.close_date > max_close_date) {
                    max_close_date = rec.close_date;
                }
            }
            if (max_close_date == 0) return;

            const std::int64_t stale_border = max_close_date - wait_status_ms;
            if (stale_border <= 0) return;

            for (auto& rec : records) {
                if (optionx::is_terminal_trade_state(rec.trade_state)) continue;
                if (rec.trade_state != optionx::TradeState::OPEN_SUCCESS &&
                    rec.trade_state != optionx::TradeState::IN_PROGRESS &&
                    rec.trade_state != optionx::TradeState::WAITING_CLOSE) {
                    continue;
                }

                bool should_check_error = false;
                if (rec.close_date > 0 && rec.close_date < stale_border) {
                    should_check_error = true;
                } else if (rec.open_date > 0 && rec.duration > 0 &&
                           (rec.open_date + rec.duration * 1000) < stale_border) {
                    should_check_error = true;
                }

                if (!should_check_error) continue;

                // Attempt callback resolution before forcing CHECK_ERROR.
                bool resolved = false;
                if (callback) {
                    const auto result = callback(rec);
                    if (result.trade_state != optionx::TradeState::UNKNOWN) {
                        resolved = try_merge_result(rec, result);
                    }
                }

                if (!resolved) {
                    rec.trade_state = optionx::TradeState::CHECK_ERROR;
                }
            }
        }

    private:
        /// \brief Selectively merges a resolved TradeResult into a TradeRecord.
        /// \param record Record to update.
        /// \param result Resolved result.
        /// \return True if any field was actually updated.
        static bool try_merge_result(optionx::TradeRecord& record, const optionx::TradeResult& result) {
            return record.merge_result_patch(result);
        }
    };

} // namespace optionx::storage

#endif // OPTIONX_HEADER_STORAGES_TRADE_RECORD_DB_TRADE_RECORD_STATUS_FIXER_HPP_INCLUDED
