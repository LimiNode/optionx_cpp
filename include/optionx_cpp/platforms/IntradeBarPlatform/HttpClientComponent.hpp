#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_HTTP_CLIENT_COMPONENT_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_HTTP_CLIENT_COMPONENT_HPP_INCLUDED

/// \file HttpClientComponent.hpp
/// \brief Defines the HTTP client component for the Intrade Bar platform.

namespace optionx::platforms::intrade_bar {

    /// \enum RateLimitType
    /// \brief Defines the types of rate limits for HTTP requests.
    enum class RateLimitType : uint32_t {
        GENERAL,                ///< The general limit for all requests.
        TRADE_EXECUTION,        ///< The limit for trade opening requests.
        TRADE_RESULT,           ///< The limit for trade result requests.
        BALANCE,                ///< The limit for balance requests.
        ACCOUNT_INFO,           ///< The limit for account information requests.
        ACCOUNT_SETTINGS,       ///< The limit for account type or currency change requests.
        TICK_DATA,              ///< The limit for tick data requests.
        FX_BAR_HISTORY,         ///< The limit for Intrade FX bar-history requests.
        BTC_BAR_HISTORY,        ///< The limit for Binance BTCUSDT bar-history requests.
        COUNT                   ///< The total number of rate limit types.
    };

    /// \brief Conservative throttle for the Intrade /fxhis history endpoint.
    inline constexpr uint32_t FX_BAR_HISTORY_REQUESTS_PER_SECOND = 1;

    /// \brief Conservative throttle for Binance kline history requests.
    inline constexpr uint32_t BTC_BAR_HISTORY_REQUESTS_PER_SECOND = 1;

    /// \brief Minimum interval between trade-result polling requests.
    inline constexpr uint32_t TRADE_RESULT_REQUEST_INTERVAL_MS = 2000;

    /// \class HttpClientComponent
    /// \brief Handles HTTP requests and manages rate limits for the Intrade Bar platform.
    class HttpClientComponent final : public components::BaseHttpClientComponent {
    public:

        /// \brief Constructs the HTTP client component and initializes rate limits.
        /// \param platform Reference to the trading platform.
        explicit HttpClientComponent(
                BaseTradingPlatform& platform)
                : components::BaseHttpClientComponent(platform.event_bus()) {
            set_rate_limit_rpm(RateLimitType::GENERAL, 60);
            set_rate_limit_rps(RateLimitType::TRADE_EXECUTION, 1);
            set_rate_limit(
                RateLimitType::TRADE_RESULT,
                1,
                TRADE_RESULT_REQUEST_INTERVAL_MS);
            set_rate_limit_rps(RateLimitType::BALANCE, 1);
            set_rate_limit_rpm(RateLimitType::ACCOUNT_INFO, 6);
            set_rate_limit_rpm(RateLimitType::ACCOUNT_SETTINGS, 12);
            set_rate_limit_rps(RateLimitType::TICK_DATA, 1);
            set_rate_limit_rps(
                RateLimitType::FX_BAR_HISTORY,
                FX_BAR_HISTORY_REQUESTS_PER_SECOND);
            set_rate_limit_rps(
                RateLimitType::BTC_BAR_HISTORY,
                BTC_BAR_HISTORY_REQUESTS_PER_SECOND);
			get_http_client().assign_rate_limit_id(get_rate_limit(RateLimitType::GENERAL));
            platform.register_component(this);
        }

        /// \brief Default destructor.
        virtual ~HttpClientComponent() = default;

    }; // class HttpClientComponent

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_HTTP_CLIENT_COMPONENT_HPP_INCLUDED
