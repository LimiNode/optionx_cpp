#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_HPP_INCLUDED

/// \file IntradeBarPlatform.hpp
/// \brief Defines the IntradeBarPlatform class, which provides an implementation of the trading platform API.

#include <optionx_cpp/config.hpp>
#include <optionx_cpp/market_data.hpp>
#include "common/ApiResult.hpp"
#include "common/BaseTradingPlatform.hpp"
#include "IntradeBarPlatform/TradeHistorySource.hpp"
#include "IntradeBarPlatform/symbol_utils.hpp"
#include "IntradeBarPlatform/AuthData.hpp"
#include "IntradeBarPlatform/AccountInfoData.hpp"
#include "IntradeBarPlatform/ApiResponses.hpp"
#include "IntradeBarPlatform/http_utils.hpp"
#include "IntradeBarPlatform/http_parsers.hpp"
#include "IntradeBarPlatform/HttpClientComponent.hpp"
#include "IntradeBarPlatform/RequestManager.hpp"
#include "IntradeBarPlatform/AuthManager.hpp"
#include "IntradeBarPlatform/TradeExecutionComponent.hpp"
#include "IntradeBarPlatform/BalanceManager.hpp"
#include "IntradeBarPlatform/ActiveTradesSyncManager.hpp"
#include "IntradeBarPlatform/TradingConditionManager.hpp"
#include "IntradeBarPlatform/PriceManager.hpp"
#include "IntradeBarPlatform/BtcPriceManager.hpp"
#include "IntradeBarPlatform/FxPriceWebSocketManager.hpp"
#include "IntradeBarPlatform/MarketDataSubscriptionManager.hpp"
#include "IntradeBarPlatform/TradeManager.hpp"

namespace optionx::platforms {

    /// \class IntradeBarPlatform
    /// \brief Implements the trading platform API for Intrade Bar.
    ///
    /// This class is responsible for handling all trading operations, including authentication,
    /// balance management, price retrieval, trade execution, and account information updates.
    /// It integrates various components that manage these aspects of trading.
    class IntradeBarPlatform final
            : public BaseTradingPlatform,
              public market_data::BaseMarketDataProvider {
    public:

        /// \brief Constructs the Intrade Bar trading platform.
        ///
        /// Initializes all required components, including HTTP communication, authentication,
        /// balance tracking, trade execution, and price updates.
        IntradeBarPlatform()
            : BaseTradingPlatform(std::make_shared<intrade_bar::AccountInfoData>()),
              m_http_client(*this),
              m_request_manager(*this, m_http_client),
              m_trade_execution(*this, m_account_info),
              m_auth_manager(*this, m_request_manager, m_account_info),
              m_balance_manager(*this, m_request_manager, m_account_info),
              m_active_trades_sync_manager(*this, m_request_manager, m_account_info),
              m_trading_condition_manager(*this, m_account_info),
              m_price_manager(*this, m_request_manager),
              m_btc_price_manager(*this),
              m_fx_price_websocket_manager(*this),
              m_market_data_subscriptions(
                  *this,
                  provider_id(),
                  m_tick_data_callback,
                  m_bar_data_callback,
                  m_market_data_status_callback,
                  &m_btc_price_manager,
                  &m_fx_price_websocket_manager),
              m_trade_manager(*this, m_request_manager, m_account_info) {
            m_btc_price_manager.set_status_sink(
                provider_id(),
                &m_market_data_subscriptions.on_source_status());
            m_fx_price_websocket_manager.set_status_sink(
                provider_id(),
                &m_market_data_subscriptions.on_source_status());
        }

        /// \brief Shuts down components while platform-owned component instances are still alive.
        ~IntradeBarPlatform() override {
            shutdown();
            m_btc_price_manager.set_status_sink(
                market_data::kInvalidProviderInstanceId,
                nullptr);
            m_fx_price_websocket_manager.set_status_sink(
                market_data::kInvalidProviderInstanceId,
                nullptr);
        }

        /// \brief Places a trade on the platform.
        ///
        /// This method forwards the trade request to the trade execution component.
        /// \param trade_request The trade request details.
        /// \return True if the trade was placed successfully; false otherwise.
        bool place_trade(std::unique_ptr<TradeRequest> trade_request) override {
            return m_trade_execution.place_trade(std::move(trade_request));
        }

        /// \brief Requests the final result for a previously opened Intrade Bar trade.
        /// \param query Broker-side trade identity and retry settings.
        /// \param result Partially filled result object to update with broker data.
        /// \param callback Callback receiving the updated result.
        /// \return True if the check was accepted for processing; false otherwise.
        bool fetch_trade_result(
                TradeResultQuery query,
                std::unique_ptr<TradeResult> result,
                trade_result_check_callback_t callback) override {
            return m_trade_manager.fetch_trade_result(
                std::move(query),
                std::move(result),
                std::move(callback));
        }

        /// \brief Requests closed Intrade Bar trade history.
        /// \param request Trade history range and timestamp field.
        /// \param callback Callback receiving closed trade history or an error.
        /// \return True if the history request was accepted for processing; false otherwise.
        bool fetch_trade_history(
                const TradeHistoryRequest& request,
                trade_history_callback_t callback) override {
            return m_trade_manager.fetch_trade_history(request, std::move(callback));
        }

        /// \brief Requests all available closed Intrade Bar trade history.
        /// \param callback Callback receiving closed trade history or an error.
        /// \return True if the history request was accepted for processing; false otherwise.
        bool fetch_trade_history(trade_history_callback_t callback) override {
            return m_trade_manager.fetch_trade_history(
                TradeHistoryRequest::all(),
                std::move(callback));
        }

        /// \brief Requests historical Intrade Bar candle data.
        /// \param request Symbol, timeframe, range, and preferred price source.
        /// \param callback Callback receiving parsed bars or a failure reason.
        /// \return True if the history request was accepted for processing; false otherwise.
        bool fetch_bar_history(
                const BarHistoryRequest& request,
                market_data::BaseMarketDataProvider::bar_history_callback_t callback) override {
            if (!callback) return false;
            m_request_manager.request_bar_history_result(
                request,
                [callback = std::move(callback)](intrade_bar::BarHistoryApiResult result) {
                    if (result) {
                        callback(BarHistoryResult::ok(
                            std::move(result.value),
                            result.status_code));
                        return;
                    }
                    callback(BarHistoryResult::fail(
                        std::move(result.error_message),
                        result.status_code));
                });
            return true;
        }

        /// \brief Returns the live bar data callback.
        market_data::BaseMarketDataProvider::bars_callback_t& on_bar_data() override {
            return m_bar_data_callback;
        }

        /// \brief Returns the live tick data callback.
        market_data::BaseMarketDataProvider::ticks_callback_t& on_tick_data() override {
            return m_tick_data_callback;
        }

        /// \brief Returns the live market-data stream status callback.
        market_data::BaseMarketDataProvider::status_callback_t& on_market_data_status() override {
            return m_market_data_status_callback;
        }

        /// \brief Requests an Intrade Bar live tick stream subscription.
        bool subscribe_ticks(
                market_data::TickSubscriptionRequest request,
                market_data::BaseMarketDataProvider::subscription_callback_t callback) override {
            return m_market_data_subscriptions.subscribe_ticks(
                std::move(request),
                std::move(callback));
        }

        /// \brief Requests an Intrade Bar live bar stream subscription.
        bool subscribe_bars(
                market_data::BarSubscriptionRequest request,
                market_data::BaseMarketDataProvider::subscription_callback_t callback) override {
            return m_market_data_subscriptions.subscribe_bars(
                std::move(request),
                std::move(callback));
        }

        /// \brief Stops an Intrade Bar live market-data subscription.
        bool unsubscribe(
                market_data::MarketDataSubscriptionHandle subscription,
                market_data::BaseMarketDataProvider::subscription_callback_t callback) override {
            return m_market_data_subscriptions.unsubscribe(
                std::move(subscription),
                std::move(callback));
        }

        /// \brief Applies an Intrade Bar live market-data subscription batch.
        bool apply_subscriptions(
                market_data::MarketDataSubscriptionBatch batch,
                market_data::BaseMarketDataProvider::subscription_batch_callback_t callback) override {
            return m_market_data_subscriptions.apply_subscriptions(
                std::move(batch),
                std::move(callback));
        }

        /// \brief Stops all Intrade Bar live market-data subscriptions.
        bool unsubscribe_all(
                market_data::BaseMarketDataProvider::subscription_batch_callback_t callback) override {
            return m_market_data_subscriptions.unsubscribe_all(std::move(callback));
        }

        /// \brief Returns the platform-level trade result callback.
        /// \return Mutable callback reference from the trade execution component.
        trade_result_callback_t& on_trade_result() override {
            return m_trade_execution.on_trade_result();
        }

        /// \brief Returns provider used to reserve persistent trade IDs before broker execution.
        /// \return Mutable provider callback from the trade execution component.
        trade_id_provider_t& on_trade_id() override {
            return m_trade_execution.on_trade_id();
        }

        /// \brief Returns the platform type.
        /// \return Platform type identifier (`PlatformType::INTRADE_BAR`).
        PlatformType platform_type() const override {
            return PlatformType::INTRADE_BAR;
        }

    private:
        intrade_bar::HttpClientComponent     m_http_client;      ///< Handles HTTP communication.
        intrade_bar::RequestManager       m_request_manager;  ///< Manages API requests.
        intrade_bar::TradeExecutionComponent m_trade_execution;  ///< Manages trade execution.
        intrade_bar::AuthManager          m_auth_manager;     ///< Handles authentication processes.
        intrade_bar::BalanceManager       m_balance_manager;  ///< Tracks account balance.
        intrade_bar::ActiveTradesSyncManager m_active_trades_sync_manager; ///< Syncs broker active trade snapshots.
        intrade_bar::TradingConditionManager m_trading_condition_manager; ///< Publishes current trading conditions.
        intrade_bar::PriceManager         m_price_manager;    ///< Retrieves and updates price data.
        intrade_bar::BtcPriceManager      m_btc_price_manager;///< Retrieves BTC/USDT quotes from the websocket stream.
        intrade_bar::FxPriceWebSocketManager m_fx_price_websocket_manager; ///< Retrieves FX quotes from websocket streams.
        market_data::BaseMarketDataProvider::bars_callback_t m_bar_data_callback; ///< Live bar data callback.
        market_data::BaseMarketDataProvider::ticks_callback_t m_tick_data_callback; ///< Live tick data callback.
        market_data::BaseMarketDataProvider::status_callback_t m_market_data_status_callback; ///< Live stream status callback.
        intrade_bar::MarketDataSubscriptionManager m_market_data_subscriptions; ///< Routes live market-data streams.
        intrade_bar::TradeManager         m_trade_manager;    ///< Manages trades and status updates.
    }; // IntradeBarPlatform

} // namespace optionx::platforms

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_HPP_INCLUDED
