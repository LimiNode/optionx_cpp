#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_OPTIONS_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_OPTIONS_HPP_INCLUDED

/// \file MarketDataContinuityOptions.hpp
/// \brief Defines history prefill and gap-recovery options for market-data routes.

#include <cstddef>
#include <cstdint>

namespace optionx::market_data {

    /// \enum MarketDataContinuityMode
    /// \brief Selects how a routed market-data stream is initialized and recovered.
    enum class MarketDataContinuityMode {
        LIVE_ONLY = 0,       ///< Deliver live provider payloads immediately.
        PREFILL,             ///< Deliver startup history without later gap recovery.
        PREFILL_AND_RECOVER  ///< Prefill and repair live/reconnect timestamp gaps.
    };

    /// \enum MarketDataTickDeduplicationMode
    /// \brief Selects which tick fields identify an already delivered observation.
    enum class MarketDataTickDeduplicationMode {
        PROVIDER_DEFAULT = 0, ///< Use the provider's identity contract.
        TIMESTAMP,            ///< Treat one timestamp as one observation.
        TIME_AND_PRICES,      ///< Match timestamp, ask, bid, and last price.
        EXACT_OBSERVATION     ///< Match timestamp, prices, and volume.
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

    /// \struct MarketDataTickContinuityOptions
    /// \brief Configures history prefill and recovery for a tick route.
    ///
    /// Tick streams do not have a dense timeframe grid. `expected_interval_ms`
    /// is therefore only a hint for detecting a suspicious live gap; a history
    /// result's `range_complete` value remains the authority for recovery.
    struct MarketDataTickContinuityOptions {
        MarketDataContinuityMode mode = MarketDataContinuityMode::LIVE_ONLY;
        std::uint64_t prefill_lookback_ms = 0;
        std::uint64_t expected_interval_ms = 1000;
        std::uint64_t max_backfill_ms = 60000; ///< Maximum time span per history request; zero is unbounded.
        MarketDataContinuityRetryPolicy retry;
        std::size_t max_buffered_batches = 1024;
        std::size_t max_buffered_items = 100000;
        /// Identity policy used to remove inclusive history overlap.
        /// Provider default keeps the policy provider-specific.
        MarketDataTickDeduplicationMode deduplication_mode =
            MarketDataTickDeduplicationMode::PROVIDER_DEFAULT;

        /// \brief Returns true when the option combination is usable.
        [[nodiscard]] bool valid() const noexcept {
            if (!retry.valid()) return false;
            switch (deduplication_mode) {
            case MarketDataTickDeduplicationMode::PROVIDER_DEFAULT:
            case MarketDataTickDeduplicationMode::TIMESTAMP:
            case MarketDataTickDeduplicationMode::TIME_AND_PRICES:
            case MarketDataTickDeduplicationMode::EXACT_OBSERVATION:
                break;
            default:
                return false;
            }
            if (mode == MarketDataContinuityMode::LIVE_ONLY) {
                return prefill_lookback_ms == 0;
            }
            if (mode == MarketDataContinuityMode::PREFILL) {
                return prefill_lookback_ms > 0;
            }
            if (mode == MarketDataContinuityMode::PREFILL_AND_RECOVER) {
                return expected_interval_ms > 0;
            }
            return false;
        }

        /// \brief Returns true when history work is enabled.
        [[nodiscard]] bool enabled() const noexcept {
            return mode != MarketDataContinuityMode::LIVE_ONLY;
        }

        /// \brief Returns true when live gaps and reconnects are recovered.
        [[nodiscard]] bool recovers_gaps() const noexcept {
            return mode == MarketDataContinuityMode::PREFILL_AND_RECOVER;
        }
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_OPTIONS_HPP_INCLUDED
