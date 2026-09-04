#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_HPP_INCLUDED

/// \file MarketDataContinuity.hpp
/// \brief Defines route-scoped continuity status updates.

#include <cstddef>
#include <cstdint>
#include <string>

namespace optionx::market_data {

    /// \enum MarketDataContinuityStatus
    /// \brief Lifecycle state of history-to-live delivery for one route.
    enum class MarketDataContinuityStatus {
        UNKNOWN = 0,
        PREFILLING,       ///< Historical initialization is being requested.
        GAP_DETECTED,     ///< A timestamp gap was found in the live stream.
        BACKFILLING,      ///< Historical bars are being loaded for a gap.
        RETRYING,         ///< A failed history request will be attempted again.
        LIVE,             ///< No known unresolved history range remains for the route.
        FAILED,           ///< A specific history operation failed; the route may continue.
        STALE,            ///< Transport loss invalidated the route's continuity.
        DEGRADED          ///< Live delivery continues while continuity remains unverified; the status is sticky until the unresolved range is verified.
    };

    /// \brief Converts a continuity status to stable text.
    inline const char* to_str(MarketDataContinuityStatus status) noexcept {
        switch (status) {
        case MarketDataContinuityStatus::PREFILLING:
            return "PREFILLING";
        case MarketDataContinuityStatus::GAP_DETECTED:
            return "GAP_DETECTED";
        case MarketDataContinuityStatus::BACKFILLING:
            return "BACKFILLING";
        case MarketDataContinuityStatus::RETRYING:
            return "RETRYING";
        case MarketDataContinuityStatus::LIVE:
            return "LIVE";
        case MarketDataContinuityStatus::FAILED:
            return "FAILED";
        case MarketDataContinuityStatus::STALE:
            return "STALE";
        case MarketDataContinuityStatus::DEGRADED:
            return "DEGRADED";
        case MarketDataContinuityStatus::UNKNOWN:
        default:
            return "UNKNOWN";
        }
    }

    /// \struct MarketDataContinuityUpdate
    /// \brief Route-scoped progress or failure information for historical delivery.
    struct MarketDataContinuityUpdate {
        MarketDataSubscriptionHandle subscription; ///< Concrete provider subscription.
        MarketDataType type = MarketDataType::BARS; ///< Continuity payload type.
        std::string symbol; ///< Provider symbol.
        BarTimeframe timeframe = 0; ///< Bar timeframe in seconds.
        MarketDataContinuityStatus status = MarketDataContinuityStatus::UNKNOWN;
        std::uint64_t from_time_ms = 0; ///< Start of the requested history range, if known.
        std::uint64_t to_time_ms = 0; ///< End of the requested history range, if known.
        std::size_t requested_items = 0; ///< Number of requested bars, when count-based.
        std::size_t delivered_items = 0; ///< Number of history items delivered by the operation.
        std::string message; ///< Optional diagnostic text.
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_HPP_INCLUDED
