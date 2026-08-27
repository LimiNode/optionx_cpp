#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TRADING_VIEW_TRADING_VIEW_EXTENSION_BRIDGE_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TRADING_VIEW_TRADING_VIEW_EXTENSION_BRIDGE_HPP_INCLUDED

/// \file TradingViewExtensionBridge.hpp
/// \brief Defines the TradingView browser-extension HTTP bridge.

namespace optionx::bridges::tradingview {

    /// \class TradingViewExtensionBridge
    /// \brief Receives TradingView extension alerts over local HTTP and publishes TradeSignal events.
    ///
    /// The bridge is intentionally narrow: it converts already-detected
    /// TradingView events into library signals. It does not execute trades and
    /// it does not assign meaning to level-alert crossings unless the user
    /// configured an explicit level alert rule.
    class TradingViewExtensionBridge final : public BaseBridge {
    private:
        class HttpServer final : public SimpleWeb::Server<SimpleWeb::HTTP> {
        public:
            unsigned short bound_port() const noexcept {
                return m_bound_port.load();
            }

        protected:
            void after_bind() override {
                m_bound_port.store(acceptor->local_endpoint().port());
            }

        private:
            std::atomic<unsigned short> m_bound_port{0};
        };

        struct RuntimeState {
            std::mutex mutex;
            std::condition_variable lifecycle_cv;
            bridge_status_callback_t status_callback;
            BaseBridge::trade_signal_callback_t trade_signal_callback;
            BaseBridge::signal_report_callback_t signal_report_callback;
            BaseBridge::signal_id_allocator_t signal_id_allocator;
            std::shared_ptr<HttpServer> server;
            std::thread server_thread;
            std::deque<std::string> dedupe_order;
            std::unordered_set<std::string> dedupe_keys;
            std::size_t active_http_requests = 0;
            bool running = false;
            bool stopping = false;
            bool stop_requested = false;
            bool pending_callback_shutdown = false;
        };

        inline static thread_local const RuntimeState* s_callback_state = nullptr;
        inline static thread_local std::size_t s_callback_depth = 0;

        class CallbackScope final {
        public:
            explicit CallbackScope(std::shared_ptr<RuntimeState> state)
                : m_state(std::move(state)),
                  m_identity(m_state.get()),
                  m_previous_state(s_callback_state),
                  m_previous_depth(s_callback_depth) {
                if (m_identity) {
                    s_callback_state = m_identity;
                    s_callback_depth =
                        m_previous_state == m_identity
                        ? m_previous_depth + 1
                        : 1;
                }
            }

            ~CallbackScope() {
                if (!m_identity) {
                    return;
                }
                s_callback_state = m_previous_state;
                s_callback_depth = m_previous_depth;
            }

        private:
            std::shared_ptr<RuntimeState> m_state;
            const RuntimeState* m_identity = nullptr;
            const RuntimeState* m_previous_state = nullptr;
            std::size_t m_previous_depth = 0;
        };

    public:
        /// \brief Constructs a bridge with empty runtime state.
        TradingViewExtensionBridge()
            : m_state(std::make_shared<RuntimeState>()) {}

        /// \brief Stops the HTTP server before destruction.
        ~TradingViewExtensionBridge() override {
            shutdown();
        }

        /// \brief Configures the bridge with TradingView HTTP settings.
        /// \param config Configuration object. Must be `TradingViewExtensionBridgeConfig`.
        /// \return `true` when configuration is valid and accepted.
        bool configure(std::unique_ptr<IBridgeConfig> config) override {
            if (!config) return false;

            const auto* typed =
                dynamic_cast<const TradingViewExtensionBridgeConfig*>(config.get());
            if (!typed) {
                config->dispatch_callbacks(false, "Invalid TradingView extension bridge config type.");
                return false;
            }

            auto next_config = std::make_shared<TradingViewExtensionBridgeConfig>(*typed);
            const auto validation = next_config->validate();
            config->dispatch_callbacks(validation.first, validation.second);
            if (!validation.first) {
                return false;
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            m_config = std::move(next_config);
            return true;
        }

        /// \brief Returns the status update callback slot.
        bridge_status_callback_t& on_status_update() override {
            return m_state->status_callback;
        }

        /// \brief Returns the trade signal callback slot.
        trade_signal_callback_t& on_trade_signal() override {
            return m_state->trade_signal_callback;
        }

        /// \brief Returns the signal diagnostic report callback slot.
        signal_report_callback_t& on_signal_report() override {
            return m_state->signal_report_callback;
        }

        /// \brief Returns the signal ID allocator slot.
        signal_id_allocator_t& on_signal_id() override {
            return m_state->signal_id_allocator;
        }

        /// \brief Ignores account updates; sizing percent is resolved downstream.
        /// \param info Account update received from the trading platform.
        void update_account_info(const AccountInfoUpdate& info) override {
            (void)info;
        }

        /// \brief Starts the local HTTP server.
        void run() override {
            auto config = get_config_or_throw();
            if (!get_signal_id_allocator()) {
                notify_status(
                    BridgeStatus::SERVER_START_FAILED,
                    {},
                    "TradingView extension bridge requires a signal ID allocator.");
                return;
            }
            if (!get_trade_signal_callback()) {
                notify_status(
                    BridgeStatus::SERVER_START_FAILED,
                    {},
                    "TradingView extension bridge requires a trade signal callback.");
                return;
            }

            {
                std::unique_lock<std::mutex> lock(m_state->mutex);
                if (m_state->stopping) {
                    if (is_inside_callback(m_state.get())) {
                        return;
                    }
                    m_state->lifecycle_cv.wait(
                        lock,
                        [state = m_state]() { return !state->stopping; });
                }
                if (m_state->running) {
                    return;
                }
                m_state->running = true;
                m_state->stop_requested = false;
                m_state->pending_callback_shutdown = false;
                m_state->active_http_requests = 0;
                m_state->dedupe_order.clear();
                m_state->dedupe_keys.clear();
            }

            try {
                auto server = std::make_shared<HttpServer>();
                server->config.address = config->address;
                server->config.port = config->port;
                server->config.thread_pool_size = 1;
                configure_routes(server, config);

                std::lock_guard<std::mutex> lock(m_state->mutex);
                if (!m_state->running) {
                    return;
                }
                m_state->server = server;
                m_state->server_thread = std::thread([state = m_state, server]() {
                    try {
                        server->start([state](unsigned short port) {
                            notify_status(
                                state,
                                BridgeStatus::SERVER_STARTED,
                                std::to_string(port));
                        });
                    } catch (const std::exception& ex) {
                        finalize_failed_start(state, ex.what());
                    } catch (...) {
                        finalize_failed_start(
                            state,
                            "Unknown TradingView HTTP server start failure.");
                    }
                });
            } catch (const std::exception& ex) {
                finalize_failed_start(m_state, ex.what());
            } catch (...) {
                finalize_failed_start(
                    m_state,
                    "Unknown TradingView HTTP server initialization failure.");
            }
        }

        /// \brief Stops the HTTP server and joins the server thread.
        void shutdown() override {
            shutdown_impl();
        }

        /// \brief Returns the currently bound HTTP port.
        /// \return Non-zero bound port when the server has started.
        unsigned short bound_port() const noexcept {
            std::shared_ptr<HttpServer> server;
            {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                server = m_state->server;
            }
            return server ? server->bound_port() : 0;
        }

    private:
        mutable std::mutex m_mutex;
        std::shared_ptr<RuntimeState> m_state;
        std::shared_ptr<TradingViewExtensionBridgeConfig> m_config;

        std::shared_ptr<TradingViewExtensionBridgeConfig> get_config_or_throw() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_config) {
                throw std::invalid_argument("TradingView extension bridge is not configured.");
            }
            return m_config;
        }

        signal_id_allocator_t get_signal_id_allocator() const {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_state->signal_id_allocator;
        }

        trade_signal_callback_t get_trade_signal_callback() const {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            return m_state->trade_signal_callback;
        }

        void notify_status(
                BridgeStatus status,
                std::string connection_id = {},
                std::string message = {}) const {
            notify_status(m_state, status, std::move(connection_id), std::move(message));
        }

        static void notify_status(
                const std::shared_ptr<RuntimeState>& state,
                BridgeStatus status,
                std::string connection_id = {},
                std::string message = {}) {
            bridge_status_callback_t callback;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                callback = state->status_callback;
            }
            if (callback) {
                try {
                    CallbackScope scope(state);
                    callback(BridgeStatusUpdate(
                        status,
                        std::move(connection_id),
                        std::move(message)));
                } catch (...) {
                }
            }
        }

        static void notify_signal_report(
                const std::shared_ptr<RuntimeState>& state,
                const BridgeSignalReport& report) {
            signal_report_callback_t callback;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                callback = state->signal_report_callback;
            }
            if (callback) {
                try {
                    CallbackScope scope(state);
                    callback(report);
                } catch (...) {
                }
            }
        }

        static bool is_inside_callback(const RuntimeState* state) noexcept {
            return state && s_callback_state == state && s_callback_depth > 0;
        }

        bool is_runtime_thread_locked() const noexcept {
            return m_state->server_thread.joinable() &&
                   m_state->server_thread.get_id() == std::this_thread::get_id();
        }

        static void finalize_runtime_stop(
                const std::shared_ptr<RuntimeState>& state,
                std::shared_ptr<HttpServer> server,
                std::thread server_thread,
                const bool notify_stopped) {
            try {
                if (server) {
                    server->stop();
                }
                if (server_thread.joinable()) {
                    if (server_thread.get_id() == std::this_thread::get_id()) {
                        server_thread.detach();
                    } else {
                        server_thread.join();
                    }
                }
            } catch (...) {
                if (server_thread.joinable()) {
                    server_thread.detach();
                }
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->stopping = false;
                state->stop_requested = false;
                state->pending_callback_shutdown = false;
                state->active_http_requests = 0;
            }
            state->lifecycle_cv.notify_all();
            if (notify_stopped) {
                notify_status(state, BridgeStatus::SERVER_STOPPED);
            }
        }

        static void finalize_runtime_stop_async(
                std::shared_ptr<RuntimeState> state,
                std::shared_ptr<HttpServer> server,
                std::thread server_thread,
                const bool notify_stopped) {
            std::thread(
                [state = std::move(state),
                 server = std::move(server),
                 server_thread = std::move(server_thread),
                 notify_stopped]() mutable {
                    finalize_runtime_stop(
                        state,
                        std::move(server),
                        std::move(server_thread),
                        notify_stopped);
                }).detach();
        }

        static void finalize_failed_start(
                const std::shared_ptr<RuntimeState>& state,
                std::string message) {
            std::shared_ptr<HttpServer> server;
            std::thread server_thread;
            bool should_finalize = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                should_finalize =
                    !state->stopping &&
                    (state->running ||
                     static_cast<bool>(state->server) ||
                     state->server_thread.joinable());
                if (should_finalize) {
                    state->stopping = true;
                    state->running = false;
                    state->stop_requested = true;
                    server = std::move(state->server);
                    if (state->server_thread.joinable()) {
                        server_thread = std::move(state->server_thread);
                    }
                }
            }
            state->lifecycle_cv.notify_all();
            notify_status(
                state,
                BridgeStatus::SERVER_START_FAILED,
                {},
                std::move(message));
            if (should_finalize) {
                if (server_thread.joinable() &&
                    server_thread.get_id() == std::this_thread::get_id()) {
                    finalize_runtime_stop_async(
                        state,
                        std::move(server),
                        std::move(server_thread),
                        true);
                }
                else {
                    finalize_runtime_stop(
                        state,
                        std::move(server),
                        std::move(server_thread),
                        true);
                }
            }
        }

        static void drain_pending_callback_shutdown(
                const std::shared_ptr<RuntimeState>& state) {
            std::shared_ptr<HttpServer> server;
            std::thread server_thread;
            bool should_finalize = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                should_finalize =
                    state->pending_callback_shutdown &&
                    state->active_http_requests == 0 &&
                    state->stopping;
                if (should_finalize) {
                    state->pending_callback_shutdown = false;
                    server = std::move(state->server);
                    if (state->server_thread.joinable()) {
                        server_thread = std::move(state->server_thread);
                    }
                }
            }
            if (should_finalize) {
                finalize_runtime_stop_async(
                    state,
                    std::move(server),
                    std::move(server_thread),
                    true);
            }
        }

        void shutdown_impl() {
            std::shared_ptr<HttpServer> server;
            std::thread server_thread;
            bool should_notify = false;
            bool async_finalize = false;
            {
                std::unique_lock<std::mutex> lock(m_state->mutex);
                if (m_state->stopping) {
                    if (!is_runtime_thread_locked() &&
                        !is_inside_callback(m_state.get())) {
                        m_state->lifecycle_cv.wait(
                            lock,
                            [state = m_state]() { return !state->stopping; });
                    }
                    return;
                }

                should_notify =
                    m_state->running ||
                    static_cast<bool>(m_state->server) ||
                    m_state->server_thread.joinable();
                if (!should_notify) {
                    return;
                }

                async_finalize =
                    is_runtime_thread_locked() || is_inside_callback(m_state.get());
                m_state->stopping = true;
                m_state->running = false;
                m_state->stop_requested = true;
                m_state->pending_callback_shutdown = async_finalize;
                if (!async_finalize) {
                    server = std::move(m_state->server);
                    if (m_state->server_thread.joinable()) {
                        server_thread = std::move(m_state->server_thread);
                    }
                }
            }
            m_state->lifecycle_cv.notify_all();

            if (async_finalize) {
                drain_pending_callback_shutdown(m_state);
                return;
            }
            finalize_runtime_stop(
                m_state,
                std::move(server),
                std::move(server_thread),
                should_notify);
        }

        static bool begin_http_request(
                const std::shared_ptr<RuntimeState>& state) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->running || state->stopping || state->stop_requested) {
                return false;
            }
            ++state->active_http_requests;
            return true;
        }

        static void end_http_request(
                const std::shared_ptr<RuntimeState>& state) {
            bool should_drain = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->active_http_requests > 0) {
                    --state->active_http_requests;
                }
                should_drain =
                    state->active_http_requests == 0 &&
                    state->pending_callback_shutdown;
            }
            if (should_drain) {
                drain_pending_callback_shutdown(state);
            }
        }

        static std::int64_t unix_time_ms_now() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        static std::string json_string_value(
                const nlohmann::json& object,
                const char* key) {
            if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) {
                return {};
            }
            return object.at(key).get<std::string>();
        }

        static std::int64_t json_integer_value(
                const nlohmann::json& object,
                const char* key) {
            if (!object.is_object() || !object.contains(key)) {
                return 0;
            }
            const auto& value = object.at(key);
            if (value.is_number_integer()) {
                return value.get<std::int64_t>();
            }
            if (value.is_number_unsigned()) {
                return static_cast<std::int64_t>(value.get<std::uint64_t>());
            }
            if (value.is_number_float()) {
                return static_cast<std::int64_t>(value.get<double>());
            }
            return 0;
        }

        static std::shared_ptr<const TradeSignal> clone_candidate_signal(
                const std::unique_ptr<TradeSignal>& signal) {
            if (!signal) {
                return {};
            }
            return std::shared_ptr<const TradeSignal>(signal->clone());
        }

        static BridgeSignalReportStatus report_status_from_parse_result(
                const detail::TradingViewParseResult& result) {
            if (!result.authorized ||
                result.reason == "invalid_payload" ||
                result.reason == "missing_symbol") {
                return BridgeSignalReportStatus::INVALID;
            }
            if (result.reason == "ignored_state_message") {
                return BridgeSignalReportStatus::IGNORED;
            }
            return BridgeSignalReportStatus::REJECTED;
        }

        static BridgeSignalReport make_signal_report(
                const TradingViewExtensionBridgeConfig& config,
                BridgeSignalReportStatus status,
                std::string reason_code,
                std::string message,
                nlohmann::json raw_payload = nlohmann::json(),
                nlohmann::json parsed_payload = nlohmann::json(),
                std::string event_id = {},
                std::string dedupe_key = {},
                std::shared_ptr<const TradeSignal> candidate_signal = {},
                nlohmann::json context = nlohmann::json::object()) {
            BridgeSignalReport report;
            report.bridge_id = config.bridge_id;
            report.bridge_type = config.bridge_type();
            report.status = status;
            report.reason_code = std::move(reason_code);
            report.message = std::move(message);
            report.event_id = std::move(event_id);
            report.dedupe_key = std::move(dedupe_key);
            report.raw_payload = std::move(raw_payload);
            report.parsed_payload = std::move(parsed_payload);
            report.context = std::move(context);
            report.candidate_signal = std::move(candidate_signal);
            report.received_time_ms = unix_time_ms_now();
            report.source_time_ms = json_integer_value(report.parsed_payload, "time");

            report.symbol = json_string_value(report.parsed_payload, "symbol");
            if (report.symbol.empty()) {
                report.symbol = json_string_value(report.parsed_payload, "original_symbol");
            }
            report.signal_name = json_string_value(report.parsed_payload, "signal_name");
            if (report.signal_name.empty()) {
                report.signal_name = json_string_value(report.parsed_payload, "alert_name");
            }
            if (report.candidate_signal) {
                if (report.symbol.empty()) {
                    report.symbol = report.candidate_signal->symbol;
                }
                if (report.signal_name.empty()) {
                    report.signal_name = report.candidate_signal->signal_name;
                }
            }
            return report;
        }

        static BridgeSignalReport make_parse_report(
                const TradingViewExtensionBridgeConfig& config,
                const detail::TradingViewParseResult& result) {
            auto message = json_string_value(result.response, "message");
            if (message.empty()) {
                message = result.reason;
            }
            return make_signal_report(
                config,
                report_status_from_parse_result(result),
                result.reason,
                std::move(message),
                result.raw_payload,
                result.parsed_payload,
                result.event_id,
                result.dedupe_key,
                clone_candidate_signal(result.signal));
        }

        static std::string regex_path(const std::string& path) {
            std::string pattern = "^";
            for (const auto ch : path) {
                switch (ch) {
                case '.':
                case '\\':
                case '+':
                case '*':
                case '?':
                case '[':
                case '^':
                case ']':
                case '$':
                case '(':
                case ')':
                case '{':
                case '}':
                case '=':
                case '!':
                case '<':
                case '>':
                case '|':
                case ':':
                case '-':
                    pattern.push_back('\\');
                    break;
                default:
                    break;
                }
                pattern.push_back(ch);
            }
            pattern.push_back('$');
            return pattern;
        }

        static SimpleWeb::CaseInsensitiveMultimap json_headers(
                const TradingViewExtensionBridgeConfig& config) {
            SimpleWeb::CaseInsensitiveMultimap headers;
            headers.emplace("Content-Type", "application/json");
            headers.emplace("Connection", "close");
            if (config.allow_cors) {
                headers.emplace("Access-Control-Allow-Origin", config.allowed_origin);
                headers.emplace("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                headers.emplace(
                    "Access-Control-Allow-Headers",
                    "Content-Type, X-OptionX-Secret, Authorization");
            }
            return headers;
        }

        static void write_json(
                const std::shared_ptr<HttpServer::Response>& response,
                SimpleWeb::StatusCode status,
                nlohmann::json body,
                const TradingViewExtensionBridgeConfig& config) {
            response->close_connection_after_response = true;
            response->write(status, body.dump(), json_headers(config));
        }

        void configure_routes(
                const std::shared_ptr<HttpServer>& server,
                const std::shared_ptr<TradingViewExtensionBridgeConfig>& config) {
            server->resource[regex_path("/health")]["GET"] =
                [config](
                    std::shared_ptr<HttpServer::Response> response,
                    std::shared_ptr<HttpServer::Request>) {
                    write_json(
                        response,
                        SimpleWeb::StatusCode::success_ok,
                        nlohmann::json{{"ok", true}},
                        *config);
                };

            server->resource[regex_path(config->signal_path)]["OPTIONS"] =
                [config](
                    std::shared_ptr<HttpServer::Response> response,
                    std::shared_ptr<HttpServer::Request>) {
                    write_json(
                        response,
                        SimpleWeb::StatusCode::success_ok,
                        nlohmann::json{{"ok", true}},
                        *config);
                };

            server->resource[regex_path(config->signal_path)]["POST"] =
                [state = m_state, config](
                    std::shared_ptr<HttpServer::Response> response,
                    std::shared_ptr<HttpServer::Request> request) {
                    handle_signal_post(state, config, response, request);
                };
        }

        static void handle_signal_post(
                const std::shared_ptr<RuntimeState>& state,
                const std::shared_ptr<TradingViewExtensionBridgeConfig>& config,
                const std::shared_ptr<HttpServer::Response>& response,
                const std::shared_ptr<HttpServer::Request>& request) {
            if (!begin_http_request(state)) {
                write_json(
                    response,
                    SimpleWeb::StatusCode::server_error_service_unavailable,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "server_stopping"}
                    },
                    *config);
                return;
            }
            struct HttpRequestGuard {
                std::shared_ptr<RuntimeState> state;

                ~HttpRequestGuard() {
                    end_http_request(state);
                }
            } request_guard{state};

            const auto body = request->content.string();
            if (body.size() > config->request_body_limit) {
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INVALID,
                        "request_body_too_large",
                        "TradingView extension request body is too large.",
                        nlohmann::json(),
                        nlohmann::json(),
                        {},
                        {},
                        {},
                        nlohmann::json{{"body_size", body.size()}}));
                write_json(
                    response,
                    SimpleWeb::StatusCode::client_error_payload_too_large,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "request_body_too_large"}
                    },
                    *config);
                return;
            }

            nlohmann::json payload;
            try {
                payload = nlohmann::json::parse(body);
            } catch (const std::exception& ex) {
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INVALID,
                        "invalid_json",
                        ex.what(),
                        nlohmann::json(),
                        nlohmann::json(),
                        {},
                        {},
                        {},
                        nlohmann::json{{"body_size", body.size()}}));
                write_json(
                    response,
                    SimpleWeb::StatusCode::client_error_bad_request,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "invalid_json"},
                        {"message", ex.what()}
                    },
                    *config);
                return;
            }

            const auto request_secret =
                request_header_value(request->header, "X-OptionX-Secret");
            auto result =
                detail::parse_extension_payload(payload, request_secret, *config);
            if (!result.authorized) {
                notify_signal_report(state, make_parse_report(*config, result));
                write_json(
                    response,
                    SimpleWeb::StatusCode::client_error_unauthorized,
                    std::move(result.response),
                    *config);
                return;
            }

            if (!result.accepted) {
                notify_signal_report(state, make_parse_report(*config, result));
                write_json(
                    response,
                    SimpleWeb::StatusCode::success_ok,
                    std::move(result.response),
                    *config);
                return;
            }

            if (!remember_dedupe_key(state, result.dedupe_key, config->dedupe_cache_size)) {
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::DUPLICATE,
                        "duplicate",
                        "TradingView extension event duplicated a recently handled signal.",
                        result.raw_payload,
                        result.parsed_payload,
                        result.event_id,
                        result.dedupe_key,
                        clone_candidate_signal(result.signal)));
                write_json(
                    response,
                    SimpleWeb::StatusCode::success_ok,
                    nlohmann::json{
                        {"ok", true},
                        {"accepted", false},
                        {"duplicate", true},
                        {"reason", "duplicate"},
                        {"event_id", result.event_id},
                        {"dedupe_key", result.dedupe_key}
                    },
                    *config);
                return;
            }

            signal_id_allocator_t allocator;
            trade_signal_callback_t callback;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                allocator = state->signal_id_allocator;
                callback = state->trade_signal_callback;
            }
            if (!allocator) {
                forget_dedupe_key(state, result.dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "missing_signal_id_allocator",
                        "TradingView extension bridge signal ID allocator is not configured.",
                        result.raw_payload,
                        result.parsed_payload,
                        result.event_id,
                        result.dedupe_key,
                        clone_candidate_signal(result.signal)));
                write_json(
                    response,
                    SimpleWeb::StatusCode::server_error_internal_server_error,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "missing_signal_id_allocator"}
                    },
                    *config);
                return;
            }
            if (!callback) {
                forget_dedupe_key(state, result.dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "missing_trade_signal_callback",
                        "TradingView extension bridge trade signal callback is not configured.",
                        result.raw_payload,
                        result.parsed_payload,
                        result.event_id,
                        result.dedupe_key,
                        clone_candidate_signal(result.signal)));
                write_json(
                    response,
                    SimpleWeb::StatusCode::server_error_internal_server_error,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "missing_trade_signal_callback"}
                    },
                    *config);
                return;
            }

            SignalId signal_id = 0;
            try {
                CallbackScope scope(state);
                signal_id = allocator();
            } catch (const std::exception& ex) {
                forget_dedupe_key(state, result.dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "signal_id_allocation_failed",
                        ex.what(),
                        result.raw_payload,
                        result.parsed_payload,
                        result.event_id,
                        result.dedupe_key,
                        clone_candidate_signal(result.signal)));
                write_json(
                    response,
                    SimpleWeb::StatusCode::server_error_internal_server_error,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "signal_id_allocation_failed"}
                    },
                    *config);
                return;
            } catch (...) {
                forget_dedupe_key(state, result.dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "signal_id_allocation_failed",
                        "TradingView extension signal ID allocator threw an unknown exception.",
                        result.raw_payload,
                        result.parsed_payload,
                        result.event_id,
                        result.dedupe_key,
                        clone_candidate_signal(result.signal)));
                write_json(
                    response,
                    SimpleWeb::StatusCode::server_error_internal_server_error,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "signal_id_allocation_failed"}
                    },
                    *config);
                return;
            }
            if (signal_id == 0) {
                forget_dedupe_key(state, result.dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "signal_id_allocation_failed",
                        "TradingView extension bridge could not allocate signal ID.",
                        result.raw_payload,
                        result.parsed_payload,
                        result.event_id,
                        result.dedupe_key,
                        clone_candidate_signal(result.signal)));
                write_json(
                    response,
                    SimpleWeb::StatusCode::server_error_internal_server_error,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "signal_id_allocation_failed"}
                    },
                    *config);
                return;
            }

            result.signal->signal_id = signal_id;
            result.signal->bridge_id = config->bridge_id;
            const auto candidate = clone_candidate_signal(result.signal);
            try {
                CallbackScope scope(state);
                callback(std::move(result.signal));
            } catch (const std::exception& ex) {
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "ambiguous_dispatch_failure",
                        ex.what(),
                        result.raw_payload,
                        result.parsed_payload,
                        result.event_id,
                        result.dedupe_key,
                        candidate));
                write_json(
                    response,
                    SimpleWeb::StatusCode::server_error_internal_server_error,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "ambiguous_dispatch_failure"}
                    },
                    *config);
                return;
            } catch (...) {
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "ambiguous_dispatch_failure",
                        "TradingView extension trade signal callback threw an unknown exception.",
                        result.raw_payload,
                        result.parsed_payload,
                        result.event_id,
                        result.dedupe_key,
                        candidate));
                write_json(
                    response,
                    SimpleWeb::StatusCode::server_error_internal_server_error,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "ambiguous_dispatch_failure"}
                    },
                    *config);
                return;
            }
            write_json(
                response,
                SimpleWeb::StatusCode::success_ok,
                std::move(result.response),
                *config);
        }

        static std::string request_header_value(
                const SimpleWeb::CaseInsensitiveMultimap& headers,
                const std::string& name) {
            const auto it = headers.find(name);
            return it == headers.end() ? std::string() : it->second;
        }

        static bool remember_dedupe_key(
                const std::shared_ptr<RuntimeState>& state,
                const std::string& dedupe_key,
                std::size_t cache_size) {
            if (dedupe_key.empty()) {
                return true;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->dedupe_keys.find(dedupe_key) != state->dedupe_keys.end()) {
                return false;
            }
            state->dedupe_keys.insert(dedupe_key);
            state->dedupe_order.push_back(dedupe_key);
            while (state->dedupe_order.size() > cache_size) {
                state->dedupe_keys.erase(state->dedupe_order.front());
                state->dedupe_order.pop_front();
            }
            return true;
        }

        static void forget_dedupe_key(
                const std::shared_ptr<RuntimeState>& state,
                const std::string& dedupe_key) {
            if (dedupe_key.empty()) {
                return;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            state->dedupe_keys.erase(dedupe_key);
        }
    };

} // namespace optionx::bridges::tradingview

#endif // OPTIONX_HEADER_BRIDGES_TRADING_VIEW_TRADING_VIEW_EXTENSION_BRIDGE_HPP_INCLUDED
