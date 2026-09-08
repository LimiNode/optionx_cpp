#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_HPP_INCLUDED

/// \file MarketDataContinuity.hpp
/// \brief Defines route-scoped continuity status updates.

#include <cstddef>
#include <cstdint>
#include <string>

#include "MarketDataRouterIds.hpp"

namespace optionx::market_data {

    /// \enum MarketDataContinuityStatus
    /// \brief Lifecycle state of history-to-live delivery for one route.
    enum class MarketDataContinuityStatus {
        UNKNOWN = 0,
        PREFILLING,       ///< Historical initialization is being requested.
        GAP_DETECTED,     ///< A timestamp gap was found in the live stream.
        BACKFILLING,      ///< Historical market data is being loaded for a gap.
        RETRYING,         ///< A failed history request will be attempted again.
        LIVE,             ///< No known unresolved history range remains for the route.
        FAILED = 6,       ///< A specific history operation failed; the route may continue.
        DEGRADED = 7,     ///< Live delivery continues while continuity remains unverified; the status is sticky until the unresolved range is verified.
        STALE = 8         ///< Transport loss invalidated the route's continuity.
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

    /// \enum MarketDataContinuityPhase
    /// \brief Delivery phase exposed by a continuity snapshot.
    enum class MarketDataContinuityPhase {
        UNKNOWN = 0,
        LIVE,
        PREFILLING,
        WAITING_FOR_READY,
        RECOVERING,
        FLUSHING,
        DEGRADED
    };

    /// \brief Converts a continuity phase to stable text.
    inline const char* to_str(MarketDataContinuityPhase phase) noexcept {
        switch (phase) {
        case MarketDataContinuityPhase::LIVE:
            return "LIVE";
        case MarketDataContinuityPhase::PREFILLING:
            return "PREFILLING";
        case MarketDataContinuityPhase::WAITING_FOR_READY:
            return "WAITING_FOR_READY";
        case MarketDataContinuityPhase::RECOVERING:
            return "RECOVERING";
        case MarketDataContinuityPhase::FLUSHING:
            return "FLUSHING";
        case MarketDataContinuityPhase::DEGRADED:
            return "DEGRADED";
        case MarketDataContinuityPhase::UNKNOWN:
        default:
            return "UNKNOWN";
        }
    }

    /// \enum MarketDataContinuityOperation
    /// \brief Historical operation tracked for one route.
    enum class MarketDataContinuityOperation {
        NONE = 0,
        PREFILL,
        GAP_BACKFILL,
        RECONNECT_BACKFILL
    };

    /// \brief Converts a continuity operation to stable text.
    inline const char* to_str(MarketDataContinuityOperation operation) noexcept {
        switch (operation) {
        case MarketDataContinuityOperation::PREFILL:
            return "PREFILL";
        case MarketDataContinuityOperation::GAP_BACKFILL:
            return "GAP_BACKFILL";
        case MarketDataContinuityOperation::RECONNECT_BACKFILL:
            return "RECONNECT_BACKFILL";
        case MarketDataContinuityOperation::NONE:
        default:
            return "NONE";
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
        std::size_t requested_items = 0; ///< Requested item count when the route uses count-based history; zero for timestamp ranges.
        std::size_t delivered_items = 0; ///< Number of history items delivered by the operation.
        std::string message; ///< Optional diagnostic text.
    };

    /// \struct MarketDataContinuitySnapshot
    /// \brief Read-only monitoring snapshot for one Router route.
    /// \details A snapshot is a copy of Router state. It is safe to inspect
    ///          from a monitoring thread and never invokes subscriber code.
    ///          Routes without continuity remain observable with `enabled=false`.
    struct MarketDataContinuitySnapshot {
        RoutedSubscriptionId route; ///< Logical route represented by the snapshot.
        MarketDataSubscriptionHandle subscription; ///< Concrete provider handle, if accepted.
        MarketDataType type = MarketDataType::UNKNOWN; ///< Routed payload type.
        std::string symbol; ///< Provider symbol.
        BarTimeframe timeframe = 0; ///< Bar timeframe, or zero for ticks.
        bool enabled = false; ///< Whether configured history continuity is enabled for this route.
        MarketDataContinuityStatus last_status = MarketDataContinuityStatus::UNKNOWN;
        MarketDataContinuityPhase phase = MarketDataContinuityPhase::UNKNOWN;
        MarketDataContinuityOperation last_operation = MarketDataContinuityOperation::NONE;
        bool request_in_flight = false; ///< True while a history request is outstanding.
        std::uint64_t last_observed_time_ms = 0; ///< Latest delivered or buffered payload timestamp.
        std::uint64_t expected_interval_ms = 0; ///< Tick gap-detection interval; zero for bar routes.
        std::uint64_t requested_from_time_ms = 0; ///< Last history range start.
        std::uint64_t requested_to_time_ms = 0; ///< Last history range end.
        std::size_t requested_items = 0; ///< Last history operation item count.
        std::uint64_t last_confirmed_from_time_ms = 0; ///< Last non-empty usable history range start.
        std::uint64_t last_confirmed_to_time_ms = 0; ///< Last non-empty usable history range end.
        std::size_t last_confirmed_items = 0; ///< Item count in the last non-empty usable history result.
        std::uint64_t verified_through_time_ms = 0; ///< Continuous verified watermark.
        std::uint64_t unverified_from_time_ms = 0; ///< Earliest unresolved trust boundary.
        std::size_t buffered_batches = 0; ///< Number of live batches held for continuity.
        std::size_t buffered_items = 0; ///< Number of live items held for continuity.
        std::size_t history_request_count = 0; ///< All issued history operations.
        std::size_t retry_count = 0; ///< Retry attempts after the first attempt.
        std::size_t failure_count = 0; ///< History operations that ended unusably.
        std::string last_failure; ///< Last terminal history failure reason.
        std::uint64_t stale_duration_ms = 0; ///< Accumulated time spent stale.
        std::uint64_t degraded_duration_ms = 0; ///< Accumulated time spent degraded.
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_HPP_INCLUDED
