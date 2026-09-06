#pragma once
#ifndef OPTIONX_HEADER_DATA_TICKS_TICK_HISTORY_REQUEST_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TICKS_TICK_HISTORY_REQUEST_HPP_INCLUDED

/// \file TickHistoryRequest.hpp
/// \brief Defines the timestamp-range request for historical ticks.

#include <cstdint>
#include <string>
#include <utility>

namespace optionx {

    /// \struct TickHistoryRequest
    /// \brief Requests historical ticks for an inclusive millisecond range.
    /// \details Tick history is timestamp-based. Unlike bar history, the
    ///          range does not imply a fixed number of expected samples. The
    ///          generic contract intentionally has no pagination or item-limit
    ///          field until a resumable tick cursor is defined.
    struct TickHistoryRequest {
        std::string symbol; ///< Provider symbol.
        std::uint64_t from_time_ms = 0; ///< Inclusive Unix start timestamp.
        std::uint64_t to_time_ms = 0; ///< Inclusive Unix end timestamp.

        /// \brief Constructs an empty invalid request.
        TickHistoryRequest() = default;

        /// \brief Constructs an inclusive timestamp-range request.
        TickHistoryRequest(
                std::string symbol,
                std::uint64_t from_time_ms,
                std::uint64_t to_time_ms)
                : symbol(std::move(symbol)),
                  from_time_ms(from_time_ms),
                  to_time_ms(to_time_ms) {}

        /// \brief Returns true when the symbol and range are usable.
        [[nodiscard]] bool valid() const noexcept {
            return !symbol.empty() &&
                from_time_ms > 0 &&
                to_time_ms >= from_time_ms;
        }
    };

} // namespace optionx

#endif // OPTIONX_HEADER_DATA_TICKS_TICK_HISTORY_REQUEST_HPP_INCLUDED
