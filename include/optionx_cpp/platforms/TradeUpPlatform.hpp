#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_TRADE_UP_PLATFORM_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_TRADE_UP_PLATFORM_HPP_INCLUDED

/// \file TradeUpPlatform.hpp
/// \brief Defines the TradeUpPlatform class implementing TradeUp broker API.

#include <optionx_cpp/utils.hpp>
#include <optionx_cpp/config.hpp>
#include <optionx_cpp/data.hpp>
#include <optionx_cpp/storages.hpp>
#include <optionx_cpp/components.hpp>

#include "common/BaseTradingPlatform.hpp"
#include "TradeUpPlatform/AuthData.hpp"
#include "TradeUpPlatform/AccountInfoData.hpp"
#include "TradeUpPlatform/http_utils.hpp"
#include "TradeUpPlatform/http_parsers.hpp"
#include "TradeUpPlatform/HttpClientComponent.hpp"
#include "TradeUpPlatform/AuthManager.hpp"
#include "TradeUpPlatform/BalanceManager.hpp"

namespace optionx::platforms {

    /// \class TradeUpPlatform
    /// \brief Minimal platform implementation for TradeUp broker.
    class TradeUpPlatform final : public BaseTradingPlatform {
    public:
        TradeUpPlatform()
            : BaseTradingPlatform(std::make_shared<tradeup::AccountInfoData>()),
              m_http_client(*this),
              m_auth_manager(*this, m_http_client, m_account_info),
              m_balance_manager(*this, m_http_client, m_account_info) {
        }

        ~TradeUpPlatform() override {
            shutdown();
        }

        bool place_trade(std::unique_ptr<TradeRequest> trade_request) override {
            (void)trade_request;
            return false;
        }

        PlatformType platform_type() const override {
            return PlatformType::TRADEUP;
        }

    private:
        tradeup::HttpClientComponent m_http_client;
        tradeup::AuthManager      m_auth_manager;
        tradeup::BalanceManager   m_balance_manager;
    };

} // namespace optionx::platforms

#endif // OPTIONX_HEADER_PLATFORMS_TRADE_UP_PLATFORM_HPP_INCLUDED
