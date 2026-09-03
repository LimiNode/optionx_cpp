#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_BTC_PRICE_MANAGER_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_BTC_PRICE_MANAGER_HPP_INCLUDED

/// \file BtcPriceManager.hpp
/// \brief Defines the BtcPriceManager class responsible for handling BTCUSDT price updates and related events.

namespace optionx::platforms::intrade_bar {

    /// \class BtcPriceManager
    /// \brief Manages BTCUSDT price updates and related events.
    ///
    /// The BtcPriceManager class is responsible for establishing a WebSocket connection
    /// to receive real-time BTCUSDT price updates. It processes incoming data and
    /// subscribes to events related to authentication, connection requests, disconnection
    /// requests, and account information updates to maintain up-to-date price information.
    class BtcPriceManager final : public components::BaseComponent {
    public:
        /// \brief Constructs the BTCUSDT price manager.
        /// \param platform Reference to the trading platform.
        explicit BtcPriceManager(
                BaseTradingPlatform& platform)
                : BaseComponent(platform.event_bus()), m_websocket_client() {
            subscribe<events::AuthDataEvent>();
            subscribe<events::ConnectRequestEvent>();
            subscribe<events::DisconnectRequestEvent>();
            subscribe<events::AccountInfoUpdateEvent>();
            subscribe<events::AutoDomainSelectedEvent>();
            platform.register_component(this);

            m_websocket_client.set_url(m_ws_host, "/bapi");
            m_websocket_client.set_user_agent(OPTIONX_DEFAULT_BROWSER_USER_AGENT);
            m_websocket_client.set_accept_language(OPTIONX_DEFAULT_ACCEPT_LANGUAGE);
            m_websocket_client.set_accept_encoding(true, true, true, true);
            m_websocket_client.set_reconnect(true);
            m_websocket_client.set_request_timeout(20);

            m_websocket_client.on_event([this](std::unique_ptr<kurlyk::WebSocketEventData> event) {
                if (!event) return;
                switch (event->event_type) {
                case kurlyk::WebSocketEventType::WS_OPEN:
                    LOGIT_INFO(event->status_code, event->error_code);
                    m_is_error = false;
                    emit_status(market_data::MarketDataStreamStatus::CONNECTED);
                    // `/bapi` is a fixed BTCUSDT stream; no subscribe frame is needed.
                    emit_status(market_data::MarketDataStreamStatus::READY);
                    break;
                case kurlyk::WebSocketEventType::WS_MESSAGE:
                    handle_message(event->message);
                    break;
                case kurlyk::WebSocketEventType::WS_CLOSE:
                    LOGIT_INFO(event->status_code, event->error_code);
                    m_is_error = false;
                    emit_status(market_data::MarketDataStreamStatus::DISCONNECTED);
                    break;
                case kurlyk::WebSocketEventType::WS_ERROR:
                    if (m_is_error) return;
                    LOGIT_ERROR(event->status_code, event->error_code);
                    m_is_error = true;
                    emit_status(
                        market_data::MarketDataStreamStatus::FAILED,
                        event->error_code.message());
                    break;
                default:
                    break;
                };
            });
        }

        /// \brief Default destructor.
        virtual ~BtcPriceManager() {
            shutdown();
        }

        /// \brief Processes incoming events and dispatches them to the appropriate handlers.
        /// \param event The received event.
        void on_event(const utils::Event* const event) override;

        /// \brief Periodically processes tasks related to BTCUSDT price updates.
        void process() override;

        /// \brief Shuts down the WebSocket connection and related operations.
        void shutdown() override;

        void set_max_send_queue_size(size_t max) {
            m_websocket_client.set_max_send_queue_size(max);
        }

        /// \brief Sets the optional status callback sink used by the public market-data API.
        /// \param provider_id Runtime market-data provider ID.
        /// \param callback Callback reference owned by the provider facade.
        void set_status_sink(
                market_data::ProviderInstanceId provider_id,
                market_data::BaseMarketDataProvider::status_callback_t* callback);

        /// \brief Adds one public market-data subscription reference.
        /// \return True when the BTC websocket source was accepted.
        bool add_market_data_subscription();

        /// \brief Removes one public market-data subscription reference.
        void remove_market_data_subscription();

    private:
        kurlyk::WebSocketClient m_websocket_client; ///< WebSocket client for BTCUSDT.
        std::string m_ws_host = make_websocket_host(AuthData{}.host); ///< Websocket host.
        bool m_is_error = false; ///< Flag indicating if an error has occurred.
        std::mutex m_source_mutex; ///< Protects subscription-driven source state.
        std::size_t m_market_data_ref_count = 0; ///< Public market-data subscriptions using BTC ticks.
        bool m_platform_connected = false; ///< Whether trading lifecycle wants the BTC stream connected.
        std::mutex m_status_mutex; ///< Protects status sink metadata.
        market_data::ProviderInstanceId m_provider_id =
            market_data::kInvalidProviderInstanceId; ///< Optional status provider ID.
        market_data::BaseMarketDataProvider::status_callback_t* m_status_callback = nullptr; ///< Optional status sink.

        /// \brief Handles incoming WebSocket messages.
        /// \param message The received message as a JSON string.
        void handle_message(const std::string& message);

        /// \brief Emits a public BTCUSDT stream status update when a callback is configured.
        void emit_status(
                market_data::MarketDataStreamStatus status,
                std::string message = {});

        /// \brief Returns true when any lifecycle path wants the websocket connected.
        bool should_connect_no_lock() const noexcept;

        /// \brief Reconnects the websocket if any lifecycle path is active.
        void reconnect_if_active();

        /// \brief Disconnects the websocket when no lifecycle path is active.
        void disconnect_if_idle();

        /// \brief Handles an authentication event.
        /// \param event The received authentication data event.
        void handle_event(const events::AuthDataEvent& event);

        /// \brief Handles an event triggered when a connection request is received.
        /// \param event The connection request event.
        void handle_event(const events::ConnectRequestEvent& event);

        /// \brief Handles an event triggered when a disconnection request is received.
        /// \param event The disconnection request event.
        void handle_event(const events::DisconnectRequestEvent& event);

        /// \brief Handles an account information update event.
        /// \param event The account info update event.
        void handle_event(const events::AccountInfoUpdateEvent& event);
        
        /// \brief Applies the selected auto-domain host to the BTC websocket.
        /// \param event Auto-domain selection result.
        void handle_event(const events::AutoDomainSelectedEvent& event);
    };

    inline void BtcPriceManager::set_status_sink(
            market_data::ProviderInstanceId provider_id,
            market_data::BaseMarketDataProvider::status_callback_t* callback) {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        m_provider_id = provider_id;
        m_status_callback = callback;
    }

    inline bool BtcPriceManager::add_market_data_subscription() {
        bool should_connect = false;
        {
            std::lock_guard<std::mutex> lock(m_source_mutex);
            should_connect = !should_connect_no_lock();
            ++m_market_data_ref_count;
        }
        if (should_connect) {
            m_websocket_client.connect();
        }
        return true;
    }

    inline void BtcPriceManager::remove_market_data_subscription() {
        {
            std::lock_guard<std::mutex> lock(m_source_mutex);
            if (m_market_data_ref_count > 0) {
                --m_market_data_ref_count;
            }
        }
        disconnect_if_idle();
    }

    inline void BtcPriceManager::on_event(const utils::Event* const event) {
        if (const auto* msg = dynamic_cast<const events::AuthDataEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::ConnectRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::DisconnectRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::AccountInfoUpdateEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::AutoDomainSelectedEvent*>(event)) {
            handle_event(*msg);
        }
    }

    inline void BtcPriceManager::handle_event(const events::AuthDataEvent& event) {
        if (auto auth_data = std::dynamic_pointer_cast<AuthData>(event.auth_data)) {
            LOGIT_TRACE0();

            auto [success, message] = auth_data->validate();
            if (!success) return;
            
            if (!auth_data->auto_find_domain && !auth_data->host.empty()) {
                m_ws_host = make_websocket_host(auth_data->host);
                m_websocket_client.set_url(m_ws_host, "/bapi");
            }

            m_websocket_client.set_user_agent(auth_data->user_agent);
            m_websocket_client.set_accept_language(auth_data->accept_language);
            m_websocket_client.set_accept_encoding(true, true, true, true);
            m_websocket_client.set_reconnect(true);
            m_websocket_client.set_request_timeout(20);
            m_websocket_client.set_proxy_server(auth_data->proxy_server);
            m_websocket_client.set_proxy_auth(auth_data->proxy_auth);
            m_websocket_client.set_proxy_type(auth_data->proxy_type);
            reconnect_if_active();
        }
    }

    inline void BtcPriceManager::handle_event(const events::ConnectRequestEvent& event) {
        LOGIT_0TRACE();
        {
            std::lock_guard<std::mutex> lock(m_source_mutex);
            m_platform_connected = false;
        }
        m_websocket_client.disconnect();
    }

    inline void BtcPriceManager::handle_event(const events::DisconnectRequestEvent& event) {
        LOGIT_0TRACE();
        {
            std::lock_guard<std::mutex> lock(m_source_mutex);
            m_platform_connected = false;
        }
        m_websocket_client.disconnect();
    }

    inline void BtcPriceManager::handle_event(const events::AccountInfoUpdateEvent& event) {
        using Status = events::AccountInfoUpdateEvent::Status;
        if (event.status == Status::CONNECTED) {
            LOGIT_0TRACE();
            bool should_connect = false;
            {
                std::lock_guard<std::mutex> lock(m_source_mutex);
                should_connect = m_market_data_ref_count == 0;
                m_platform_connected = true;
            }
            if (should_connect && !m_websocket_client.is_connected()) {
                m_websocket_client.connect();
            }
        } else
        if (event.status == Status::DISCONNECTED) {
            LOGIT_0TRACE();
            {
                std::lock_guard<std::mutex> lock(m_source_mutex);
                m_platform_connected = false;
            }
            disconnect_if_idle();
        }
    }
    
    inline void BtcPriceManager::handle_event(const events::AutoDomainSelectedEvent& event) {
        LOGIT_0TRACE();
        if (event.success && !event.selected_host.empty()) {
            m_ws_host = make_websocket_host(event.selected_host);
            m_websocket_client.set_url(m_ws_host, "/bapi");
            reconnect_if_active();
        }
    }

    inline void BtcPriceManager::handle_message(const std::string& message) {
        events::TickUpdateBatch batch;
        if (parse_btcusdt_tick(message, batch)) {
            std::vector<events::TickUpdateBatch> batches;
            batches.push_back(std::move(batch));
            notify_async(std::make_unique<events::PriceUpdateEvent>(
                std::move(batches),
                MarketDataUpdateSource::WEBSOCKET));
        }
    }

    inline void BtcPriceManager::emit_status(
            market_data::MarketDataStreamStatus status,
            std::string message) {
        market_data::BaseMarketDataProvider::status_callback_t* callback = nullptr;
        market_data::ProviderInstanceId provider_id = market_data::kInvalidProviderInstanceId;
        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            callback = m_status_callback;
            provider_id = m_provider_id;
        }
        if (!callback || !*callback) return;

        market_data::MarketDataStatusUpdate update;
        update.provider_id = provider_id;
        update.type = market_data::MarketDataType::TICKS;
        update.symbol = "BTCUSDT";
        update.transport = market_data::MarketDataTransport::WEBSOCKET;
        update.status = status;
        update.message = std::move(message);
        (*callback)(std::move(update));
    }

    inline void BtcPriceManager::process() {

    }

    inline void BtcPriceManager::shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_source_mutex);
            m_market_data_ref_count = 0;
            m_platform_connected = false;
        }
        m_websocket_client.disconnect_and_wait();
    }

    inline bool BtcPriceManager::should_connect_no_lock() const noexcept {
        return m_platform_connected || m_market_data_ref_count > 0;
    }

    inline void BtcPriceManager::reconnect_if_active() {
        bool should_connect = false;
        {
            std::lock_guard<std::mutex> lock(m_source_mutex);
            should_connect = should_connect_no_lock();
        }
        if (!should_connect) return;

        m_websocket_client.disconnect();
        m_websocket_client.connect();
    }

    inline void BtcPriceManager::disconnect_if_idle() {
        bool should_disconnect = false;
        {
            std::lock_guard<std::mutex> lock(m_source_mutex);
            should_disconnect = !should_connect_no_lock();
        }
        if (should_disconnect) {
            m_websocket_client.disconnect();
        }
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_BTC_PRICE_MANAGER_HPP_INCLUDED
