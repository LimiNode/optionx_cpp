#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_PROVIDER_PROFILE_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_PROVIDER_PROFILE_HPP_INCLUDED

/// \file MarketDataProviderProfile.hpp
/// \brief Defines configuration defaults for a registered market-data provider.

#include "MarketDataContinuityOptions.hpp"

namespace optionx::market_data {

    /// \struct MarketDataProviderProfile
    /// \brief Stores application-level defaults for one registered provider.
    /// \details A profile is configuration data only. The Router does not apply
    ///          it implicitly to subscriptions; copy the defaults into a
    ///          BarSubscriptionRequest and adjust them for the route as needed.
    struct MarketDataProviderProfile {
        MarketDataContinuityOptions continuity_defaults;

        /// \brief Returns true when the provider defaults form a valid policy.
        [[nodiscard]] bool valid() const noexcept {
            return continuity_defaults.valid();
        }
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_PROVIDER_PROFILE_HPP_INCLUDED
