#pragma once
#ifndef OPTIONX_HEADER_DATA_TICKS_TICK_HISTORY_REQUEST_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TICKS_TICK_HISTORY_REQUEST_HPP_INCLUDED

/// \file TickHistoryRequest.hpp
/// \brief Defines the timestamp-range request for historical ticks.

#include <cstdint>
#include <string>
#include <utility>

namespace optionx {

    /// \enum TickHistorySource
    /// \brief Selects one provider-owned source of historical tick observations.
    enum class TickHistorySource : std::uint8_t {
        LOCAL_OBSERVED = 0, ///< Observations retained from the current live session.
        BROKER_HISTORICAL  ///< Historical observations loaded from the broker.
    };

    /// \brief Converts a tick-history source to a stable metadata token.
    inline const char* to_str(TickHistorySource source) noexcept {
        switch (source) {
        case TickHistorySource::LOCAL_OBSERVED:
            return "LOCAL_OBSERVED";
        case TickHistorySource::BROKER_HISTORICAL:
            return "BROKER_HISTORICAL";
        }
        return "LOCAL_OBSERVED";
    }

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
        TickHistorySource source = TickHistorySource::LOCAL_OBSERVED; ///< Requested source.

        /// \brief Constructs an empty invalid request.
        TickHistoryRequest() = default;

        /// \brief Constructs an inclusive timestamp-range request.
        TickHistoryRequest(
                std::string symbol,
                std::uint64_t from_time_ms,
                std::uint64_t to_time_ms,
                TickHistorySource source = TickHistorySource::LOCAL_OBSERVED)
                : symbol(std::move(symbol)),
                  from_time_ms(from_time_ms),
                  to_time_ms(to_time_ms),
                  source(source) {}

        /// \brief Returns true when the symbol and range are usable.
        [[nodiscard]] bool valid() const noexcept {
            return !symbol.empty() &&
                from_time_ms > 0 &&
                to_time_ms >= from_time_ms;
        }
    };

} // namespace optionx

#endif // OPTIONX_HEADER_DATA_TICKS_TICK_HISTORY_REQUEST_HPP_INCLUDED
