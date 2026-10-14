#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_TRADE_EXECUTION_COMPONENT_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_TRADE_EXECUTION_COMPONENT_HPP_INCLUDED

/// \file TradeExecutionComponent.hpp
/// \brief Implements trade execution functionality for the Intrade Bar platform.

#include <cstdint>
#include <limits>

#include "symbol_utils.hpp"

namespace optionx::platforms::intrade_bar {

    namespace detail {

        /// \brief Calculates relative CLASSIC duration from an absolute Intrade expiry.
        /// \param timestamp Current timestamp in seconds.
        /// \param expiry_time Intended closing timestamp in seconds.
        /// \return Duration in seconds, or 0 if the expiry is invalid for Intrade.
        inline std::int64_t calc_classic_expiration(
                const std::int64_t timestamp,
                const std::int64_t expiry_time) {
            if ((expiry_time % time_shield::SEC_PER_5_MIN) != 0) return 0;
            const std::int64_t diff = expiry_time - timestamp;
            if (diff <= time_shield::SEC_PER_3_MIN) return 0;
            return ((((diff - 1) / time_shield::SEC_PER_MIN - 3) / 5) * 5 + 5) *
                   time_shield::SEC_PER_MIN;
        }

        /// \brief Calculates an absolute Intrade CLASSIC expiry from duration minutes.
        /// \param timestamp Current timestamp in seconds.
        /// \param expiration Expiration duration in minutes.
        /// \return Closing timestamp in seconds, or 0 if the duration is invalid.
        inline std::int64_t calc_classic_expiry_time(
                const std::int64_t timestamp,
                const std::int64_t expiration) {
            if ((expiration % 5) != 0 || expiration < 5) return 0;
            const std::int64_t timestamp_future =
                timestamp + (expiration + 3) * time_shield::SEC_PER_MIN;
            return timestamp_future - timestamp_future % time_shield::SEC_PER_5_MIN;
        }

        /// \brief Normalizes an Intrade CLASSIC request to carry both expiry forms.
        /// \param trade_request Request to normalize.
        /// \param timestamp Current timestamp in seconds.
        /// \return True when the CLASSIC expiry is valid for Intrade.
        inline bool normalize_classic_expiry(
                TradeRequest& trade_request,
                const std::int64_t timestamp) {
            if (trade_request.expiry_time > 0) {
                const auto duration =
                    calc_classic_expiration(timestamp, trade_request.expiry_time);
                if (duration <= 0 ||
                    duration > static_cast<std::int64_t>(
                        (std::numeric_limits<std::uint32_t>::max)())) {
                    return false;
                }
                trade_request.duration = static_cast<std::uint32_t>(duration);
                return true;
            }

            if (trade_request.expiry_time == 0 &&
                trade_request.duration > 0) {
                if ((trade_request.duration % time_shield::SEC_PER_5_MIN) != 0) {
                    return false;
                }
                trade_request.expiry_time = calc_classic_expiry_time(
                    timestamp,
                    trade_request.duration / time_shield::SEC_PER_MIN);
                return trade_request.expiry_time > 0;
            }

            return false;
        }

    } // namespace detail

    /// \class TradeExecutionComponent
    /// \brief Handles trade execution operations for the Intrade Bar platform.
    ///
    /// This component is responsible for executing trade requests, interacting with
    /// the HTTP client, and managing trade execution workflows.
    class TradeExecutionComponent final : public components::BaseTradeExecutionComponent {
    public:

        /// \brief Constructs the trade execution component.
        /// \param platform Reference to the trading platform.
        /// \param account_info Shared pointer to account information data.
        explicit TradeExecutionComponent(
                BaseTradingPlatform& platform,
                std::shared_ptr<BaseAccountInfoData> account_info)
                : components::BaseTradeExecutionComponent(
                    platform.event_bus(),
                    std::move(account_info),
                    true) {
            platform.register_component(this);
        }

        /// \brief Default destructor.
        virtual ~TradeExecutionComponent() = default;

        /// \brief Returns the platform type.
        /// \return Platform type identifier (`PlatformType::INTRADE_BAR`).
        PlatformType platform_type() const override final {
            return PlatformType::INTRADE_BAR;
        }

    private:

        /// \brief Preprocesses a trade request before placing it into the queue.
        /// \param trade_request Unique pointer to a trade request.
        /// \param trade_result Unique pointer to a trade result.
        /// \return True if the request is valid after preprocessing; false otherwise.
        bool preprocess_trade_request(
                std::unique_ptr<TradeRequest> &trade_request,
                std::unique_ptr<TradeResult> &trade_result) override final {
            if (trade_request) {
                trade_request->symbol = normalize_symbol_name(std::move(trade_request->symbol));
            }
            if (trade_request &&
                trade_request->option_type == OptionType::CLASSIC) {
                if (!detail::normalize_classic_expiry(
                        *trade_request,
                        time_shield::ms_to_sec(trade_result->place_date))) {
                    return false;
                }
            }
            return true;
        }
    }; // class TradeExecutionComponent

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_TRADE_EXECUTION_COMPONENT_HPP_INCLUDED
