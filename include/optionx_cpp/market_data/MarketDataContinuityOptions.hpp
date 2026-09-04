#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_OPTIONS_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_OPTIONS_HPP_INCLUDED

/// \file MarketDataContinuityOptions.hpp
/// \brief Defines history prefill and gap-recovery options for bar routes.

#include <cstddef>
#include <cstdint>

namespace optionx::market_data {

    /// \enum MarketDataContinuityMode
    /// \brief Selects how a routed bar stream is initialized and recovered.
    enum class MarketDataContinuityMode {
        LIVE_ONLY = 0,       ///< Deliver live provider payloads immediately.
        PREFILL,             ///< Deliver startup history without later gap recovery.
        PREFILL_AND_RECOVER  ///< Prefill and repair live/reconnect timestamp gaps.
    };

    /// \struct MarketDataContinuityRetryPolicy
    /// \brief Configures bounded history retry attempts and exponential backoff.
    struct MarketDataContinuityRetryPolicy {
        std::size_t max_attempts = 1; ///< Total attempts, including the first request.
        std::uint64_t initial_backoff_ms = 0; ///< Delay before the second attempt.
        std::uint64_t max_backoff_ms = 30000; ///< Backoff cap; zero means no cap.

        /// \brief Returns true when retry settings can be applied safely.
        [[nodiscard]] bool valid() const noexcept {
            return max_attempts > 0 &&
                (max_backoff_ms == 0 || max_backoff_ms >= initial_backoff_ms);
        }

    };

    /// \struct MarketDataContinuityOptions
    /// \brief Configures history prefill, recovery, retries, and buffering.
    struct MarketDataContinuityOptions {
        MarketDataContinuityMode mode = MarketDataContinuityMode::LIVE_ONLY;
        std::size_t prefill_bars = 0; ///< Number of historical bars requested before live delivery.
        std::size_t max_backfill_bars = 1000; ///< Maximum bars per gap or reconnect request; zero is unbounded.
        MarketDataContinuityRetryPolicy retry;
        std::size_t max_buffered_batches = 1024; ///< Maximum live batches held during prefill or reconnect; zero is unbounded.
        std::size_t max_buffered_items = 100000; ///< Maximum live items held during prefill or reconnect; zero is unbounded.

        /// \brief Returns true when the option combination is usable.
        [[nodiscard]] bool valid() const noexcept {
            switch (mode) {
            case MarketDataContinuityMode::LIVE_ONLY:
                return prefill_bars == 0;
            case MarketDataContinuityMode::PREFILL:
                return prefill_bars > 0 && retry.valid();
            case MarketDataContinuityMode::PREFILL_AND_RECOVER:
                return retry.valid();
            default:
                return false;
            }
        }

        /// \brief Returns true when history work is enabled for this route.
        [[nodiscard]] bool enabled() const noexcept {
            return mode != MarketDataContinuityMode::LIVE_ONLY;
        }

        /// \brief Returns true when gap repair is enabled.
        [[nodiscard]] bool recovers_gaps() const noexcept {
            return mode == MarketDataContinuityMode::PREFILL_AND_RECOVER;
        }
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_OPTIONS_HPP_INCLUDED
