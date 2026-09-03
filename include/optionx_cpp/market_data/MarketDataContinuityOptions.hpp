#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_OPTIONS_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_OPTIONS_HPP_INCLUDED

/// \file MarketDataContinuityOptions.hpp
/// \brief Defines history prefill and gap-recovery options for bar routes.

#include <cstddef>
#include <cstdint>
#include <limits>

namespace optionx::market_data {

    /// \enum MarketDataContinuityMode
    /// \brief Selects how a routed bar stream is initialized and recovered.
    enum class MarketDataContinuityMode {
        LIVE_ONLY = 0,       ///< Deliver live provider payloads immediately.
        PREFILL,             ///< Deliver historical bars before live payloads.
        PREFILL_AND_RECOVER  ///< Prefill and repair timestamp gaps in live bars.
    };

    /// \enum MarketDataContinuityBarPolicy
    /// \brief Selects how repeated or out-of-order bar timestamps are handled.
    enum class MarketDataContinuityBarPolicy {
        KEEP_ALL = 0,       ///< Preserve every provider bar, including revisions.
        DROP_NON_MONOTONIC  ///< Keep only strictly increasing timestamps per route.
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

        /// \brief Calculates the delay after a failed one-based attempt.
        [[nodiscard]] std::uint64_t delay_after_attempt(
                std::size_t attempt) const noexcept {
            if (attempt == 0 || initial_backoff_ms == 0) return 0;

            auto delay = initial_backoff_ms;
            for (std::size_t index = 1; index < attempt; ++index) {
                if (delay > std::numeric_limits<std::uint64_t>::max() / 2U) {
                    delay = std::numeric_limits<std::uint64_t>::max();
                    break;
                }
                delay *= 2U;
                if (max_backoff_ms > 0 && delay >= max_backoff_ms) {
                    delay = max_backoff_ms;
                    break;
                }
            }
            return max_backoff_ms > 0
                ? (delay < max_backoff_ms ? delay : max_backoff_ms)
                : delay;
        }
    };

    /// \struct MarketDataContinuityOptions
    /// \brief Configures history prefill, recovery, ordering, and retries.
    struct MarketDataContinuityOptions {
        MarketDataContinuityMode mode = MarketDataContinuityMode::LIVE_ONLY;
        std::size_t prefill_bars = 0; ///< Number of historical bars requested before live delivery.
        std::size_t max_backfill_bars = 1000; ///< Maximum bars per detected gap; zero is unbounded.
        MarketDataContinuityBarPolicy bar_policy =
            MarketDataContinuityBarPolicy::KEEP_ALL;
        MarketDataContinuityRetryPolicy retry;

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
