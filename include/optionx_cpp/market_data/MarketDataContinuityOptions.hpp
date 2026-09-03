#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_OPTIONS_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_OPTIONS_HPP_INCLUDED

/// \file MarketDataContinuityOptions.hpp
/// \brief Defines history prefill and gap-recovery options for bar routes.

#include <cstddef>

namespace optionx::market_data {

    /// \enum MarketDataContinuityMode
    /// \brief Selects how a routed bar stream is initialized and recovered.
    enum class MarketDataContinuityMode {
        LIVE_ONLY = 0,       ///< Deliver live provider payloads immediately.
        PREFILL,             ///< Deliver historical bars before live payloads.
        PREFILL_AND_RECOVER  ///< Prefill and repair timestamp gaps in live bars.
    };

    /// \struct MarketDataContinuityOptions
    /// \brief Configures history prefill and timestamp-gap recovery for a bar route.
    struct MarketDataContinuityOptions {
        MarketDataContinuityMode mode = MarketDataContinuityMode::LIVE_ONLY;
        std::size_t prefill_bars = 0; ///< Number of historical bars requested before live delivery.
        std::size_t max_backfill_bars = 1000; ///< Maximum bars per detected gap; zero is unbounded.

        /// \brief Returns true when the option combination is usable.
        [[nodiscard]] bool valid() const noexcept {
            switch (mode) {
            case MarketDataContinuityMode::LIVE_ONLY:
                return prefill_bars == 0;
            case MarketDataContinuityMode::PREFILL:
                return prefill_bars > 0;
            case MarketDataContinuityMode::PREFILL_AND_RECOVER:
                return true;
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
