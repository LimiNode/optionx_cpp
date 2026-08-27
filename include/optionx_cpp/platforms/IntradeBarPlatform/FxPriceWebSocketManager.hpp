#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_FX_PRICE_WEB_SOCKET_MANAGER_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_FX_PRICE_WEB_SOCKET_MANAGER_HPP_INCLUDED

/// \file FxPriceWebSocketManager.hpp
/// \brief Defines the Intrade Bar FX websocket tick source manager.

namespace optionx::platforms::intrade_bar {

    /// \class FxPriceWebSocketManager
    /// \brief Manages `/fxconnect` websocket tick streams for subscribed FX symbols.
    /// \details Intrade Bar accepts one FX symbol per websocket connection. This
    /// manager keeps a refcount per normalized symbol, opens the required
    /// connections, sends the slash-separated stream symbol after `WS_OPEN`, and
    /// publishes parsed ticks as `PriceUpdateEvent`.
    class FxPriceWebSocketManager final : public components::BaseComponent {
    public:
        /// \brief Constructs the FX websocket source manager.
        /// \param platform Owning platform facade.
        explicit FxPriceWebSocketManager(BaseTradingPlatform& platform)
                : BaseComponent(platform.event_bus()) {
            subscribe<events::AuthDataEvent>();
            subscribe<events::ConnectRequestEvent>();
            subscribe<events::DisconnectRequestEvent>();
            subscribe<events::AccountInfoUpdateEvent>();
            subscribe<events::AutoDomainSelectedEvent>();
            platform.register_component(this);
        }

        /// \brief Destructor that closes all active websocket connections.
        ~FxPriceWebSocketManager() override {
            shutdown();
        }

        /// \brief Adds one consumer reference for an FX symbol websocket stream.
        /// \param symbol Public or broker symbol name.
        /// \return True if the symbol maps to an FX websocket stream.
        bool add_symbol_subscription(const std::string& symbol);

        /// \brief Removes one consumer reference for an FX symbol websocket stream.
        /// \param symbol Public or broker symbol name.
        void remove_symbol_subscription(const std::string& symbol);

        /// \brief Sets the optional status callback sink used by the public market-data API.
        /// \param provider_id Runtime market-data provider ID.
        /// \param callback Callback reference owned by the provider facade.
        void set_status_sink(
                market_data::ProviderInstanceId provider_id,
                market_data::BaseMarketDataProvider::status_callback_t* callback);

        /// \brief Handles platform lifecycle/configuration events.
        void on_event(const utils::Event* const event) override;

        /// \brief Disconnects and removes all managed FX websocket streams.
        void shutdown() override;

    private:
        /// \brief Runtime state for one `/fxconnect` websocket.
        struct FxStreamState {
            std::string symbol;        ///< Normalized internal symbol, such as `EURUSD`.
            std::string stream_symbol; ///< Broker websocket symbol, such as `EUR/USD`.
            std::shared_ptr<kurlyk::WebSocketClient> client; ///< Websocket client.
            std::size_t ref_count = 0; ///< Number of active subscriptions using this stream.
            bool connect_requested = false; ///< Whether connect/reconnect is already in flight.
            bool connect_start_failed = false; ///< Suppresses the WS_ERROR produced by connect(false).
            bool is_error = false;     ///< Suppresses repeated error logging.
        };

        std::string m_ws_host = make_websocket_host(AuthData{}.host); ///< Websocket host.
        std::string m_user_agent = OPTIONX_DEFAULT_BROWSER_USER_AGENT; ///< User-Agent header.
        std::string m_accept_language = OPTIONX_DEFAULT_ACCEPT_LANGUAGE; ///< Accept-Language header.
        std::string m_proxy_server; ///< Proxy address in <ip:port> format.
        std::string m_proxy_auth;   ///< Proxy authentication in <username:password> format.
        kurlyk::ProxyType m_proxy_type = kurlyk::ProxyType::PROXY_HTTP; ///< Proxy type.
        std::unordered_map<std::string, std::shared_ptr<FxStreamState>> m_streams; ///< Active FX streams.
        std::mutex m_mutex; ///< Protects stream state and websocket settings.
        market_data::ProviderInstanceId m_provider_id =
            market_data::kInvalidProviderInstanceId; ///< Optional status provider ID.
        market_data::BaseMarketDataProvider::status_callback_t* m_status_callback = nullptr; ///< Optional status sink.
        bool m_platform_connected = false; ///< Whether the trading account lifecycle is connected.

        /// \brief Configures a websocket client from current manager settings.
        void configure_client_no_lock(kurlyk::WebSocketClient& client) const;

        /// \brief Creates a configured websocket stream state for a normalized FX symbol.
        std::shared_ptr<FxStreamState> create_stream_no_lock(
                const std::string& symbol,
                const std::string& stream_symbol);

        /// \brief Marks a stream for a new connection request while the manager mutex is held.
        static bool mark_connect_requested_no_lock(const std::shared_ptr<FxStreamState>& stream);

        /// \brief Updates the pending connect flag while holding the manager mutex.
        void set_connect_requested(
                const std::shared_ptr<FxStreamState>& stream,
                bool requested);

        /// \brief Marks a startup-level connect rejection and allows future connect attempts.
        void mark_connect_start_failed(const std::shared_ptr<FxStreamState>& stream);

        /// \brief Marks reconnect as pending unless the error belongs to a startup rejection.
        void mark_reconnect_pending(const std::shared_ptr<FxStreamState>& stream);

        /// \brief Connects a stream if it has a websocket client.
        void connect_stream(const std::shared_ptr<FxStreamState>& stream);

        /// \brief Disconnects a stream if it has a websocket client.
        void disconnect_stream(const std::shared_ptr<FxStreamState>& stream);

        /// \brief Reconnects all active streams after a host/proxy/header change.
        void reconnect_all_streams();

        /// \brief Connects active streams that are not currently connected.
        void connect_all_streams();

        /// \brief Disconnects all active streams without changing subscription refcounts.
        void disconnect_all_streams();

        /// \brief Handles a websocket event for one FX stream.
        void handle_websocket_event(
                const std::shared_ptr<FxStreamState>& stream,
                const kurlyk::WebSocketEventData& event);

        /// \brief Parses and publishes a websocket message for one FX stream.
        void handle_message(
                const std::shared_ptr<FxStreamState>& stream,
                const std::string& message);

        /// \brief Emits a public stream status update when a callback is configured.
        void emit_status(
                const std::shared_ptr<FxStreamState>& stream,
                market_data::MarketDataStreamStatus status,
                std::string message = {});

        /// \brief Applies Intrade auth/config data to websocket settings.
        void handle_event(const events::AuthDataEvent& event);

        /// \brief Handles a platform connect request.
        void handle_event(const events::ConnectRequestEvent& event);

        /// \brief Handles a platform disconnect request.
        void handle_event(const events::DisconnectRequestEvent& event);

        /// \brief Handles account connection-state updates.
        void handle_event(const events::AccountInfoUpdateEvent& event);

        /// \brief Applies the selected auto-domain host to websocket settings.
        void handle_event(const events::AutoDomainSelectedEvent& event);
    };

    inline bool FxPriceWebSocketManager::add_symbol_subscription(
            const std::string& symbol) {
        const auto normalized = normalize_symbol_name(symbol);
        const auto stream_symbol = make_fxconnect_symbol(normalized);
        if (stream_symbol.empty()) return false;

        std::shared_ptr<FxStreamState> stream;
        bool should_connect = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_streams.find(normalized);
            if (it == m_streams.end()) {
                stream = create_stream_no_lock(normalized, stream_symbol);
                stream->ref_count = 1;
                m_streams.emplace(normalized, stream);
                should_connect = mark_connect_requested_no_lock(stream);
            } else {
                stream = it->second;
                ++stream->ref_count;
                should_connect = mark_connect_requested_no_lock(stream);
            }
        }

        if (should_connect) {
            connect_stream(stream);
        }
        return true;
    }

    inline void FxPriceWebSocketManager::remove_symbol_subscription(
            const std::string& symbol) {
        const auto normalized = normalize_symbol_name(symbol);
        std::shared_ptr<FxStreamState> stream_to_disconnect;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_streams.find(normalized);
            if (it == m_streams.end()) return;

            if (it->second->ref_count > 1) {
                --it->second->ref_count;
                return;
            }

            stream_to_disconnect = it->second;
            m_streams.erase(it);
        }

        disconnect_stream(stream_to_disconnect);
    }

    inline void FxPriceWebSocketManager::set_status_sink(
            market_data::ProviderInstanceId provider_id,
            market_data::BaseMarketDataProvider::status_callback_t* callback) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_provider_id = provider_id;
        m_status_callback = callback;
    }

    inline void FxPriceWebSocketManager::on_event(const utils::Event* const event) {
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

    inline void FxPriceWebSocketManager::shutdown() {
        std::vector<std::shared_ptr<FxStreamState>> streams;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            streams.reserve(m_streams.size());
            for (auto& [symbol, stream] : m_streams) {
                (void)symbol;
                streams.push_back(std::move(stream));
            }
            m_streams.clear();
        }

        for (auto& stream : streams) {
            disconnect_stream(stream);
        }
    }

    inline void FxPriceWebSocketManager::configure_client_no_lock(
            kurlyk::WebSocketClient& client) const {
        client.set_url(m_ws_host, "/fxconnect");
        client.set_user_agent(m_user_agent);
        client.set_accept_language(m_accept_language);
        client.set_accept_encoding(true, true, true, true);
        client.set_reconnect(true);
        client.set_request_timeout(20);
        client.set_proxy_server(m_proxy_server);
        client.set_proxy_auth(m_proxy_auth);
        client.set_proxy_type(m_proxy_type);
    }

    inline std::shared_ptr<FxPriceWebSocketManager::FxStreamState>
    FxPriceWebSocketManager::create_stream_no_lock(
            const std::string& symbol,
            const std::string& stream_symbol) {
        auto stream = std::make_shared<FxStreamState>();
        stream->symbol = symbol;
        stream->stream_symbol = stream_symbol;
        stream->client = std::make_shared<kurlyk::WebSocketClient>();
        configure_client_no_lock(*stream->client);

        std::weak_ptr<FxStreamState> weak_stream = stream;
        stream->client->on_event(
            [this, weak_stream](std::unique_ptr<kurlyk::WebSocketEventData> event) {
                auto locked = weak_stream.lock();
                if (!locked || !event) return;
                handle_websocket_event(locked, *event);
            });
        return stream;
    }

    inline bool FxPriceWebSocketManager::mark_connect_requested_no_lock(
            const std::shared_ptr<FxStreamState>& stream) {
        if (!stream || !stream->client) return false;
        if (stream->connect_requested || stream->client->is_connected()) return false;
        stream->connect_requested = true;
        stream->connect_start_failed = false;
        return true;
    }

    inline void FxPriceWebSocketManager::set_connect_requested(
            const std::shared_ptr<FxStreamState>& stream,
            const bool requested) {
        if (!stream) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        stream->connect_requested = requested;
        if (!requested) {
            stream->connect_start_failed = false;
        }
    }

    inline void FxPriceWebSocketManager::mark_connect_start_failed(
            const std::shared_ptr<FxStreamState>& stream) {
        if (!stream) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        stream->connect_requested = false;
        stream->connect_start_failed = true;
    }

    inline void FxPriceWebSocketManager::mark_reconnect_pending(
            const std::shared_ptr<FxStreamState>& stream) {
        if (!stream) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (stream->connect_start_failed) {
            stream->connect_start_failed = false;
            stream->connect_requested = false;
            return;
        }
        stream->connect_requested = true;
    }

    inline void FxPriceWebSocketManager::connect_stream(
            const std::shared_ptr<FxStreamState>& stream) {
        if (!stream || !stream->client || stream->client->is_connected()) return;

        std::weak_ptr<FxStreamState> weak_stream = stream;
        stream->client->connect(
            [this, weak_stream](const bool accepted) {
                if (accepted) return;

                if (auto locked = weak_stream.lock()) {
                    mark_connect_start_failed(locked);
                }
            });
    }

    inline void FxPriceWebSocketManager::disconnect_stream(
            const std::shared_ptr<FxStreamState>& stream) {
        if (stream && stream->client) {
            stream->client->disconnect_and_wait();
        }
        set_connect_requested(stream, false);
    }

    inline void FxPriceWebSocketManager::reconnect_all_streams() {
        std::vector<std::shared_ptr<FxStreamState>> streams;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            streams.reserve(m_streams.size());
            for (auto& [symbol, stream] : m_streams) {
                (void)symbol;
                streams.push_back(stream);
            }
        }

        for (auto& stream : streams) {
            disconnect_stream(stream);
        }

        std::vector<std::shared_ptr<FxStreamState>> streams_to_connect;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const bool should_connect = m_platform_connected || !m_streams.empty();
            if (should_connect) {
                streams_to_connect.reserve(m_streams.size());
            }
            for (auto& [symbol, stream] : m_streams) {
                (void)symbol;
                configure_client_no_lock(*stream->client);
                if (should_connect && mark_connect_requested_no_lock(stream)) {
                    streams_to_connect.push_back(stream);
                }
            }
        }

        for (auto& stream : streams_to_connect) {
            connect_stream(stream);
        }
    }

    inline void FxPriceWebSocketManager::connect_all_streams() {
        std::vector<std::shared_ptr<FxStreamState>> streams;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            streams.reserve(m_streams.size());
            for (auto& [symbol, stream] : m_streams) {
                (void)symbol;
                if (mark_connect_requested_no_lock(stream)) {
                    streams.push_back(stream);
                }
            }
        }

        for (auto& stream : streams) {
            connect_stream(stream);
        }
    }

    inline void FxPriceWebSocketManager::handle_websocket_event(
            const std::shared_ptr<FxStreamState>& stream,
            const kurlyk::WebSocketEventData& event) {
        switch (event.event_type) {
        case kurlyk::WebSocketEventType::WS_OPEN:
            set_connect_requested(stream, false);
            stream->is_error = false;
            emit_status(stream, market_data::MarketDataStreamStatus::CONNECTED);
            if (event.sender) {
                const auto result = event.sender->submit_message(stream->stream_symbol);
                if (!result.accepted) {
                    LOGIT_WARN("Intrade Bar FX websocket: failed to submit stream symbol ",
                               stream->stream_symbol,
                               ": ",
                               result.error_code.message());
                    emit_status(
                        stream,
                        market_data::MarketDataStreamStatus::FAILED,
                        result.error_code.message());
                } else {
                    emit_status(stream, market_data::MarketDataStreamStatus::READY);
                }
            }
            break;
        case kurlyk::WebSocketEventType::WS_MESSAGE:
            handle_message(stream, event.message);
            break;
        case kurlyk::WebSocketEventType::WS_CLOSE:
            mark_reconnect_pending(stream);
            stream->is_error = false;
            emit_status(stream, market_data::MarketDataStreamStatus::DISCONNECTED);
            break;
        case kurlyk::WebSocketEventType::WS_ERROR:
            mark_reconnect_pending(stream);
            if (!stream->is_error) {
                LOGIT_ERROR("Intrade Bar FX websocket error for ",
                            stream->stream_symbol,
                            ": ",
                            event.error_code.message());
                stream->is_error = true;
            }
            emit_status(stream, market_data::MarketDataStreamStatus::FAILED, event.error_code.message());
            break;
        default:
            break;
        }
    }

    inline void FxPriceWebSocketManager::handle_message(
            const std::shared_ptr<FxStreamState>& stream,
            const std::string& message) {
        try {
            events::TickUpdateBatch batch;
            if (!parse_fxconnect_tick(message, batch)) return;
            if (batch.symbol != stream->symbol) return;

            std::vector<events::TickUpdateBatch> batches;
            batches.push_back(std::move(batch));
            notify_async(std::make_unique<events::PriceUpdateEvent>(
                std::move(batches),
                MarketDataUpdateSource::WEBSOCKET));
        } catch (const std::exception& ex) {
            LOGIT_WARN("Intrade Bar FX websocket: failed to parse tick for ",
                       stream ? stream->stream_symbol : std::string(),
                       ": ",
                       ex.what());
        }
    }

    inline void FxPriceWebSocketManager::emit_status(
            const std::shared_ptr<FxStreamState>& stream,
            market_data::MarketDataStreamStatus status,
            std::string message) {
        market_data::BaseMarketDataProvider::status_callback_t* callback = nullptr;
        market_data::ProviderInstanceId provider_id = market_data::kInvalidProviderInstanceId;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callback = m_status_callback;
            provider_id = m_provider_id;
        }
        if (!callback || !*callback || !stream) return;

        market_data::MarketDataStatusUpdate update;
        update.provider_id = provider_id;
        update.type = market_data::MarketDataType::TICKS;
        update.symbol = stream->symbol;
        update.transport = market_data::MarketDataTransport::WEBSOCKET;
        update.status = status;
        update.message = std::move(message);
        (*callback)(std::move(update));
    }

    inline void FxPriceWebSocketManager::handle_event(
            const events::AuthDataEvent& event) {
        auto auth_data = std::dynamic_pointer_cast<AuthData>(event.auth_data);
        if (!auth_data) return;
        const auto [success, message] = auth_data->validate();
        if (!success) {
            LOGIT_WARN("Intrade Bar FX websocket: ignored invalid auth data: ", message);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!auth_data->auto_find_domain && !auth_data->host.empty()) {
                m_ws_host = make_websocket_host(auth_data->host);
            }
            m_user_agent = auth_data->user_agent;
            m_accept_language = auth_data->accept_language;
            m_proxy_server = auth_data->proxy_server;
            m_proxy_auth = auth_data->proxy_auth;
            m_proxy_type = auth_data->proxy_type;
        }

        reconnect_all_streams();
    }

    inline void FxPriceWebSocketManager::handle_event(
            const events::ConnectRequestEvent& event) {
        (void)event;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_platform_connected = false;
        }
        disconnect_all_streams();
    }

    inline void FxPriceWebSocketManager::handle_event(
            const events::DisconnectRequestEvent& event) {
        (void)event;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_platform_connected = false;
        }
        disconnect_all_streams();
    }

    inline void FxPriceWebSocketManager::handle_event(
            const events::AccountInfoUpdateEvent& event) {
        using Status = events::AccountInfoUpdateEvent::Status;
        if (event.status == Status::CONNECTED) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_platform_connected = true;
            }
            connect_all_streams();
        } else if (event.status == Status::DISCONNECTED) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_platform_connected = false;
            }
        }
    }

    inline void FxPriceWebSocketManager::handle_event(
            const events::AutoDomainSelectedEvent& event) {
        if (!event.success || event.selected_host.empty()) return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ws_host = make_websocket_host(event.selected_host);
        }

        reconnect_all_streams();
    }

    inline void FxPriceWebSocketManager::disconnect_all_streams() {
        std::vector<std::shared_ptr<FxStreamState>> streams;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            streams.reserve(m_streams.size());
            for (auto& [symbol, stream] : m_streams) {
                (void)symbol;
                streams.push_back(stream);
            }
        }

        for (auto& stream : streams) {
            disconnect_stream(stream);
        }
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_FX_PRICE_WEB_SOCKET_MANAGER_HPP_INCLUDED
