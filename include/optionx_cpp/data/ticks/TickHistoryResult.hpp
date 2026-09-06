#pragma once
#ifndef OPTIONX_HEADER_DATA_TICKS_TICK_HISTORY_RESULT_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TICKS_TICK_HISTORY_RESULT_HPP_INCLUDED

/// \file TickHistoryResult.hpp
/// \brief Defines the typed result of a historical tick request.

#include <string>
#include <utility>

#include "TickSequence.hpp"

namespace optionx {

    /// \struct TickHistoryResult
    /// \brief Typed result for an authoritative historical tick range.
    /// \details `range_complete` is a provider assertion. It is true only
    ///          when the provider can account for the whole requested range;
    ///          an empty but authoritative range is therefore valid. A false
    ///          value means that the returned ticks are usable as observations,
    ///          but must not be treated as proof of continuity.
    struct TickHistoryResult {
        static constexpr long NO_HTTP_STATUS = 0;      ///< No HTTP status was captured.
        static constexpr long NO_RESPONSE_STATUS = -1; ///< The request produced no response.

        bool success = false; ///< Whether the provider completed the request.
        long status_code = NO_HTTP_STATUS; ///< Transport status, when available.
        bool range_complete = false; ///< Whether the provider proved range completeness.
        std::string error_desc; ///< Human-readable failure reason.
        TickSequence sequence; ///< Returned ticks and source metadata.

        /// \brief Creates a successful tick-history result.
        static TickHistoryResult ok(
                TickSequence tick_sequence,
                bool range_complete,
                long status = NO_HTTP_STATUS) {
            TickHistoryResult result;
            result.success = true;
            result.status_code = status;
            result.range_complete = range_complete;
            result.sequence = std::move(tick_sequence);
            return result;
        }

        /// \brief Creates a failed tick-history result.
        static TickHistoryResult fail(
                std::string message,
                long status = NO_RESPONSE_STATUS) {
            TickHistoryResult result;
            result.status_code = status;
            result.error_desc = std::move(message);
            return result;
        }

        /// \brief Checks whether a transport status was captured.
        [[nodiscard]] bool has_http_status() const noexcept {
            return status_code > NO_HTTP_STATUS;
        }

        /// \brief Allows concise success checks.
        explicit operator bool() const noexcept {
            return success;
        }
    };

} // namespace optionx

#endif // OPTIONX_HEADER_DATA_TICKS_TICK_HISTORY_RESULT_HPP_INCLUDED
