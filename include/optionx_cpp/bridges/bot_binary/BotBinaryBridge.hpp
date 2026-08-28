#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_BOT_BINARY_BOT_BINARY_BRIDGE_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_BOT_BINARY_BOT_BINARY_BRIDGE_HPP_INCLUDED

/// \file BotBinaryBridge.hpp
/// \brief Defines the BotBinary/BinaryBot compatibility bridge.

#include <optionx_cpp/bridges/detail/BridgeTradeSignalValidation.hpp>

namespace optionx::bridges::bot_binary {

    /// \class BotBinaryBridge
    /// \brief Receives BotBinary/BinaryBot HTTP and file signals and publishes TradeSignal events.
    class BotBinaryBridge final : public BaseBridge {
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
            std::condition_variable cv;
            bridge_status_callback_t status_callback;
            BaseBridge::trade_signal_callback_t trade_signal_callback;
            BaseBridge::signal_report_callback_t signal_report_callback;
            BaseBridge::signal_id_allocator_t signal_id_allocator;
            std::shared_ptr<HttpServer> http_server;
            std::thread http_thread;
            std::thread file_thread;
            std::deque<std::string> dedupe_order;
            std::unordered_set<std::string> dedupe_keys;
            std::deque<std::string> seen_file_order;
            std::unordered_set<std::string> seen_file_keys;
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
        BotBinaryBridge()
            : m_state(std::make_shared<RuntimeState>()) {}

        /// \brief Stops owned transport threads before destruction.
        ~BotBinaryBridge() override {
            shutdown();
        }

        /// \brief Configures the bridge with BotBinary settings.
        /// \param config Configuration object. Must be `BotBinaryBridgeConfig`.
        /// \return `true` when configuration is valid and accepted.
        bool configure(std::unique_ptr<IBridgeConfig> config) override {
            if (!config) return false;

            const auto* typed = dynamic_cast<const BotBinaryBridgeConfig*>(config.get());
            if (!typed) {
                config->dispatch_callbacks(false, "Invalid BotBinary bridge config type.");
                return false;
            }

            auto next_config = std::make_shared<BotBinaryBridgeConfig>(*typed);
            const auto validation = next_config->validate();
            config->dispatch_callbacks(validation.first, validation.second);
            if (!validation.first) {
                return false;
            }

            std::lock_guard<std::mutex> lock(m_config_mutex);
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

        /// \brief Ignores account updates; BotBinary signal intake has no account feed.
        void update_account_info(const AccountInfoUpdate& info) override {
            (void)info;
        }

        /// \brief Starts enabled BotBinary HTTP and file-signal transports.
        void run() override {
            auto config = get_config_or_throw();
            if (!get_signal_id_allocator()) {
                notify_status(
                    BridgeStatus::SERVER_START_FAILED,
                    {},
                    "BotBinary bridge requires a signal ID allocator.");
                return;
            }
            if (!get_trade_signal_callback()) {
                notify_status(
                    BridgeStatus::SERVER_START_FAILED,
                    {},
                    "BotBinary bridge requires a trade signal callback.");
                return;
            }

            {
                std::unique_lock<std::mutex> lock(m_state->mutex);
                if (m_state->stopping) {
                    if (is_inside_callback(m_state.get())) {
                        return;
                    }
                    m_state->cv.wait(lock, [state = m_state]() { return !state->stopping; });
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
                m_state->seen_file_order.clear();
                m_state->seen_file_keys.clear();
            }

            std::shared_ptr<HttpServer> http_server;
            if (config->enable_http) {
                http_server = std::make_shared<HttpServer>();
                http_server->config.address = config->address;
                http_server->config.port = config->port;
                http_server->config.thread_pool_size = 1;
                configure_routes(http_server, config);
            }

            try {
                if (config->enable_file_signal) {
                    std::filesystem::create_directories(
                        std::filesystem::u8path(config->file_signal_dir));
                }
            } catch (const std::exception& ex) {
                {
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    m_state->running = false;
                    m_state->stop_requested = true;
                }
                notify_status(BridgeStatus::SERVER_START_FAILED, "file", ex.what());
                return;
            }

            {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                if (!m_state->running) {
                    return;
                }
                m_state->http_server = http_server;
                if (http_server) {
                    m_state->http_thread = std::thread(
                        [state = m_state, server = http_server]() {
                            try {
                                server->start([state](unsigned short port) {
                                    notify_status_from_state(
                                        state,
                                        BridgeStatus::SERVER_STARTED,
                                        "http:" + std::to_string(port));
                                });
                            } catch (const std::exception& ex) {
                                finalize_failed_start_from_state(
                                    state,
                                    "http",
                                    ex.what());
                            } catch (...) {
                                finalize_failed_start_from_state(
                                    state,
                                    "http",
                                    "unknown HTTP server start failure");
                            }
                        });
                }
                if (config->enable_file_signal) {
                    m_state->file_thread = std::thread(
                        [state = m_state, config]() {
                            file_loop(state, config);
                        });
                }
            }
        }

        /// \brief Stops enabled transports and joins their threads.
        void shutdown() override {
            shutdown_impl();
        }

        /// \brief Returns the currently bound HTTP port.
        /// \return Non-zero bound port when the HTTP server has started.
        unsigned short bound_http_port() const noexcept {
            std::shared_ptr<HttpServer> server;
            {
                std::lock_guard<std::mutex> lock(m_state->mutex);
                server = m_state->http_server;
            }
            return server ? server->bound_port() : 0;
        }

    private:
        mutable std::mutex m_config_mutex;
        std::shared_ptr<RuntimeState> m_state;
        std::shared_ptr<BotBinaryBridgeConfig> m_config;

        std::shared_ptr<BotBinaryBridgeConfig> get_config_or_throw() const {
            std::lock_guard<std::mutex> lock(m_config_mutex);
            if (!m_config) {
                throw std::invalid_argument("BotBinary bridge is not configured.");
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
            notify_status_from_state(
                m_state,
                status,
                std::move(connection_id),
                std::move(message));
        }

        static void notify_status_from_state(
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
                CallbackScope scope(state);
                callback(BridgeStatusUpdate(
                    status,
                    std::move(connection_id),
                    std::move(message)));
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
                CallbackScope scope(state);
                callback(report);
            }
        }

        static std::int64_t unix_time_ms_now() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
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

        static SimpleWeb::CaseInsensitiveMultimap json_headers() {
            SimpleWeb::CaseInsensitiveMultimap headers;
            headers.emplace("Content-Type", "application/json");
            headers.emplace("Connection", "close");
            return headers;
        }

        static void write_json(
                const std::shared_ptr<HttpServer::Response>& response,
                SimpleWeb::StatusCode status,
                nlohmann::json body) {
            response->close_connection_after_response = true;
            response->write(status, body.dump(-1), json_headers());
        }

        static bool begin_http_request(
                const std::shared_ptr<RuntimeState>& state) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->stopping || state->stop_requested) {
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

        void configure_routes(
                const std::shared_ptr<HttpServer>& server,
                const std::shared_ptr<BotBinaryBridgeConfig>& config) {
            server->resource[regex_path("/health")]["GET"] =
                [](
                    std::shared_ptr<HttpServer::Response> response,
                    std::shared_ptr<HttpServer::Request>) {
                    write_json(
                        response,
                        SimpleWeb::StatusCode::success_ok,
                        nlohmann::json{{"ok", true}});
                };

            server->resource[regex_path(config->http_path)]["GET"] =
                [state = m_state, config](
                    std::shared_ptr<HttpServer::Response> response,
                    std::shared_ptr<HttpServer::Request> request) {
                    handle_http_request(state, config, response, request);
                };
        }

        static nlohmann::json parsed_payload_json(
                const BotBinaryParsedCommand& command,
                std::string source) {
            return nlohmann::json{
                {"source", std::move(source)},
                {"symbol", command.symbol},
                {"order_type", to_str(command.order_type)},
                {"amount", command.amount_value},
                {"expiry_kind", command.expiry_kind == BotBinaryExpiryKind::DURATION
                    ? "duration"
                    : "endtime"},
                {"expiry_value", command.expiry_value},
                {"expiry_unit", bot_binary_time_unit_token(command.expiry_unit)},
                {"transport_suffix", command.transport_suffix}
            };
        }

        static BridgeSignalReport make_signal_report(
                const BotBinaryBridgeConfig& config,
                BridgeSignalReportStatus status,
                std::string reason_code,
                std::string message,
                std::string raw_value,
                nlohmann::json parsed_payload = nlohmann::json::object(),
                std::string dedupe_key = {},
                std::shared_ptr<const TradeSignal> candidate_signal = {},
                nlohmann::json context = nlohmann::json::object()) {
            BridgeSignalReport report;
            report.bridge_id = config.bridge_id;
            report.bridge_type = config.bridge_type();
            report.status = status;
            report.reason_code = std::move(reason_code);
            report.message = std::move(message);
            report.raw_payload = nlohmann::json{{"value", std::move(raw_value)}};
            report.parsed_payload = std::move(parsed_payload);
            report.dedupe_key = std::move(dedupe_key);
            report.candidate_signal = std::move(candidate_signal);
            report.context = std::move(context);
            report.received_time_ms = unix_time_ms_now();
            if (report.parsed_payload.is_object()) {
                report.symbol = report.parsed_payload.value("symbol", std::string());
                report.signal_name = config.signal_name;
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

        static std::string dedupe_key_for_command(
                const BotBinaryParsedCommand& command,
                const std::string& raw_value,
                const std::string& source) {
            (void)source;
            if (!command.transport_suffix.empty()) {
                return "suffix:" + command.transport_suffix;
            }
            return "raw:" + raw_value;
        }

        static bool remember_key(
                const std::shared_ptr<RuntimeState>& state,
                const std::string& key,
                const std::size_t cache_size) {
            if (key.empty()) {
                return true;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->dedupe_keys.find(key) != state->dedupe_keys.end()) {
                return false;
            }
            state->dedupe_keys.insert(key);
            state->dedupe_order.push_back(key);
            while (state->dedupe_order.size() > cache_size) {
                state->dedupe_keys.erase(state->dedupe_order.front());
                state->dedupe_order.pop_front();
            }
            return true;
        }

        static void forget_key(
                const std::shared_ptr<RuntimeState>& state,
                const std::string& key) {
            if (key.empty()) {
                return;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            state->dedupe_keys.erase(key);
            state->dedupe_order.erase(
                std::remove(
                    state->dedupe_order.begin(),
                    state->dedupe_order.end(),
                    key),
                state->dedupe_order.end());
        }

        static bool remember_file_seen(
                const std::shared_ptr<RuntimeState>& state,
                const std::string& key,
                const std::size_t cache_size) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->seen_file_keys.find(key) != state->seen_file_keys.end()) {
                return false;
            }
            state->seen_file_keys.insert(key);
            state->seen_file_order.push_back(key);
            while (state->seen_file_order.size() > cache_size) {
                state->seen_file_keys.erase(state->seen_file_order.front());
                state->seen_file_order.pop_front();
            }
            return true;
        }

        static void apply_symbol_map(
                TradeSignal& signal,
                const BotBinaryBridgeConfig& config) {
            const auto it = config.symbol_map.find(signal.symbol);
            if (it != config.symbol_map.end()) {
                signal.symbol = optionx::bridges::detail::trim_ascii_copy(it->second);
            }
        }

        static std::shared_ptr<const TradeSignal> clone_signal(const TradeSignal& signal) {
            return std::shared_ptr<const TradeSignal>(signal.clone());
        }

        static nlohmann::json accepted_response(const TradeSignal& signal) {
            return nlohmann::json{
                {"ok", true},
                {"accepted", true},
                {"signal_id", signal.signal_id},
                {"symbol", signal.symbol},
                {"signal_name", signal.signal_name}
            };
        }

        static nlohmann::json dispatch_command(
                const std::shared_ptr<RuntimeState>& state,
                const BotBinaryBridgeConfig& config,
                const BotBinaryParsedCommand& parsed,
                const std::string& raw_value,
                const std::string& source) {
            auto signal = bot_binary_to_trade_signal(parsed, config.signal_name);
            signal.bridge_id = config.bridge_id;
            apply_symbol_map(signal, config);

            const auto parsed_payload = parsed_payload_json(parsed, source);
            const auto dedupe_key = dedupe_key_for_command(parsed, raw_value, source);
            try {
                optionx::bridges::detail::validate_executable_trade_signal(
                    signal,
                    "BotBinary signal");
            } catch (const std::exception& ex) {
                notify_signal_report(
                    state,
                    make_signal_report(
                        config,
                        BridgeSignalReportStatus::INVALID,
                        "invalid_trade_signal",
                        ex.what(),
                        raw_value,
                        parsed_payload,
                        dedupe_key,
                        clone_signal(signal)));
                return nlohmann::json{
                    {"ok", false},
                    {"accepted", false},
                    {"reason", "invalid_trade_signal"},
                    {"message", ex.what()}
                };
            }
            if (!remember_key(state, dedupe_key, config.dedupe_cache_size)) {
                notify_signal_report(
                    state,
                    make_signal_report(
                        config,
                        BridgeSignalReportStatus::DUPLICATE,
                        "duplicate",
                        "BotBinary command duplicated a recently handled signal.",
                        raw_value,
                        parsed_payload,
                        dedupe_key,
                        clone_signal(signal)));
                return nlohmann::json{
                    {"ok", true},
                    {"accepted", false},
                    {"duplicate", true},
                    {"reason", "duplicate"},
                    {"dedupe_key", dedupe_key}
                };
            }

            signal_id_allocator_t allocator;
            trade_signal_callback_t callback;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                allocator = state->signal_id_allocator;
                callback = state->trade_signal_callback;
            }

            if (!allocator) {
                forget_key(state, dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "missing_signal_id_allocator",
                        "BotBinary bridge signal ID allocator is not configured.",
                        raw_value,
                        parsed_payload,
                        dedupe_key,
                        clone_signal(signal)));
                return nlohmann::json{
                    {"ok", false},
                    {"accepted", false},
                    {"reason", "missing_signal_id_allocator"}
                };
            }

            if (!callback) {
                forget_key(state, dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "missing_trade_signal_callback",
                        "BotBinary bridge trade signal callback is not configured.",
                        raw_value,
                        parsed_payload,
                        dedupe_key,
                        clone_signal(signal)));
                return nlohmann::json{
                    {"ok", false},
                    {"accepted", false},
                    {"reason", "missing_trade_signal_callback"}
                };
            }

            try {
                CallbackScope scope(state);
                signal.signal_id = allocator();
            } catch (const std::exception& ex) {
                forget_key(state, dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "signal_id_allocator_failed",
                        ex.what(),
                        raw_value,
                        parsed_payload,
                        dedupe_key,
                        clone_signal(signal)));
                return nlohmann::json{
                    {"ok", false},
                    {"accepted", false},
                    {"reason", "signal_id_allocator_failed"},
                    {"message", ex.what()}
                };
            } catch (...) {
                forget_key(state, dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "signal_id_allocator_failed",
                        "BotBinary bridge signal ID allocator failed.",
                        raw_value,
                        parsed_payload,
                        dedupe_key,
                        clone_signal(signal)));
                return nlohmann::json{
                    {"ok", false},
                    {"accepted", false},
                    {"reason", "signal_id_allocator_failed"}
                };
            }
            if (signal.signal_id == 0) {
                forget_key(state, dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "signal_id_allocation_failed",
                        "BotBinary bridge could not allocate signal ID.",
                        raw_value,
                        parsed_payload,
                        dedupe_key,
                        clone_signal(signal)));
                return nlohmann::json{
                    {"ok", false},
                    {"accepted", false},
                    {"reason", "signal_id_allocation_failed"}
                };
            }

            auto candidate = clone_signal(signal);
            try {
                CallbackScope scope(state);
                callback(signal.clone());
            } catch (const std::exception& ex) {
                forget_key(state, dedupe_key);
                notify_signal_report(
                    state,
                    make_signal_report(
                        config,
                        BridgeSignalReportStatus::INTAKE_ERROR,
                        "trade_signal_callback_failed",
                        ex.what(),
                        raw_value,
                        parsed_payload,
                        dedupe_key,
                        candidate));
                return nlohmann::json{
                    {"ok", false},
                    {"accepted", false},
                    {"reason", "trade_signal_callback_failed"},
                    {"message", ex.what()}
                };
            }

            return accepted_response(signal);
        }

        static nlohmann::json dispatch_raw_value(
                const std::shared_ptr<RuntimeState>& state,
                const BotBinaryBridgeConfig& config,
                const std::string& raw_value,
                const std::string& source,
                const bool file_signal) {
            try {
                const auto parsed = file_signal
                    ? parse_bot_binary_file_signal_name(raw_value)
                    : parse_bot_binary_request_value(raw_value);
                return dispatch_command(state, config, parsed, raw_value, source);
            } catch (const std::exception& ex) {
                notify_signal_report(
                    state,
                    make_signal_report(
                        config,
                        BridgeSignalReportStatus::INVALID,
                        "invalid_bot_binary_command",
                        ex.what(),
                        raw_value,
                        nlohmann::json::object(),
                        {},
                        {},
                        nlohmann::json{{"source", source}}));
                return nlohmann::json{
                    {"ok", false},
                    {"accepted", false},
                    {"reason", "invalid_bot_binary_command"},
                    {"message", ex.what()}
                };
            }
        }

        static void handle_http_request(
                const std::shared_ptr<RuntimeState>& state,
                const std::shared_ptr<BotBinaryBridgeConfig>& config,
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
                    });
                return;
            }
            struct HttpRequestGuard {
                std::shared_ptr<RuntimeState> state;

                ~HttpRequestGuard() {
                    end_http_request(state);
                }
            } guard{state};

            const auto query = request->query_string;
            if (query.size() > config->request_query_limit) {
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INVALID,
                        "request_query_too_large",
                        "BotBinary HTTP request query is too large.",
                        query,
                        nlohmann::json::object(),
                        {},
                        {},
                        nlohmann::json{{"query_size", query.size()}}));
                write_json(
                    response,
                    SimpleWeb::StatusCode::client_error_bad_request,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "request_query_too_large"}
                    });
                return;
            }

            std::string raw_value;
            try {
                raw_value = detail::extract_request_query_value(query);
            } catch (const std::exception& ex) {
                notify_signal_report(
                    state,
                    make_signal_report(
                        *config,
                        BridgeSignalReportStatus::INVALID,
                        "invalid_bot_binary_query",
                        ex.what(),
                        query));
                write_json(
                    response,
                    SimpleWeb::StatusCode::client_error_bad_request,
                    nlohmann::json{
                        {"ok", false},
                        {"accepted", false},
                        {"reason", "invalid_bot_binary_query"},
                        {"message", ex.what()}
                    });
                return;
            }

            const auto body =
                dispatch_raw_value(state, *config, raw_value, "http", false);
            const auto status = body.value("ok", false)
                ? SimpleWeb::StatusCode::success_ok
                : SimpleWeb::StatusCode::client_error_bad_request;
            write_json(response, status, body);
        }

        static bool has_txt_extension(const std::filesystem::path& path) {
            auto extension = path.extension().u8string();
            std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                [](const unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
            return extension == ".txt";
        }

        static void remove_file_if_requested(
                const std::filesystem::path& path,
                const bool enabled) {
            if (!enabled) {
                return;
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        static void poll_file_signals(
                const std::shared_ptr<RuntimeState>& state,
                const BotBinaryBridgeConfig& config) {
            const auto root = std::filesystem::u8path(config.file_signal_dir);
            std::error_code ec;
            if (!std::filesystem::exists(root, ec)) {
                return;
            }

            for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
                if (ec) {
                    notify_status_from_state(
                        state,
                        BridgeStatus::CONNECTION_ERROR,
                        "file",
                        ec.message());
                    return;
                }
                if (!entry.is_regular_file(ec) || ec || !has_txt_extension(entry.path())) {
                    ec.clear();
                    continue;
                }

                const auto file_name = entry.path().filename().u8string();
                if (!remember_file_seen(state, file_name, config.dedupe_cache_size)) {
                    continue;
                }

                const auto result =
                    dispatch_raw_value(state, config, file_name, "file", true);
                const bool accepted = result.value("accepted", false);
                remove_file_if_requested(
                    entry.path(),
                    accepted ? config.delete_processed_files : config.delete_invalid_files);
            }
        }

        static void file_loop(
                const std::shared_ptr<RuntimeState>& state,
                const std::shared_ptr<BotBinaryBridgeConfig>& config) {
            notify_status_from_state(
                state,
                BridgeStatus::SERVER_STARTED,
                "file:" + config->file_signal_dir);

            while (true) {
                {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    if (state->stop_requested) {
                        break;
                    }
                }

                try {
                    poll_file_signals(state, *config);
                } catch (const std::exception& ex) {
                    notify_status_from_state(
                        state,
                        BridgeStatus::CONNECTION_ERROR,
                        "file",
                        ex.what());
                }

                std::unique_lock<std::mutex> lock(state->mutex);
                state->cv.wait_for(
                    lock,
                    std::chrono::milliseconds(config->poll_interval_ms),
                    [state]() { return state->stop_requested; });
                if (state->stop_requested) {
                    break;
                }
            }
        }

        bool is_runtime_thread_locked() const {
            const auto current_id = std::this_thread::get_id();
            return (m_state->http_thread.joinable() &&
                    m_state->http_thread.get_id() == current_id) ||
                   (m_state->file_thread.joinable() &&
                    m_state->file_thread.get_id() == current_id);
        }

        static bool is_inside_callback(const RuntimeState* state) {
            return state && s_callback_state == state && s_callback_depth > 0;
        }

        static void finalize_runtime_stop(
                const std::shared_ptr<RuntimeState>& state,
                std::shared_ptr<HttpServer> http_server,
                std::thread http_thread,
                std::thread file_thread,
                const bool should_notify) {
            try {
                if (http_server) {
                    http_server->stop();
                }
                if (http_thread.joinable()) {
                    if (http_thread.get_id() == std::this_thread::get_id()) {
                        http_thread.detach();
                    } else {
                        http_thread.join();
                    }
                }
                if (file_thread.joinable()) {
                    if (file_thread.get_id() == std::this_thread::get_id()) {
                        file_thread.detach();
                    } else {
                        file_thread.join();
                    }
                }
            } catch (...) {
                if (http_thread.joinable()) {
                    http_thread.detach();
                }
                if (file_thread.joinable()) {
                    file_thread.detach();
                }
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->stopping = false;
                state->stop_requested = false;
                state->pending_callback_shutdown = false;
            }
            state->cv.notify_all();
            if (should_notify) {
                notify_status_from_state(state, BridgeStatus::SERVER_STOPPED);
            }
        }

        static void finalize_runtime_stop_async(
                std::shared_ptr<RuntimeState> state,
                std::shared_ptr<HttpServer> http_server,
                std::thread http_thread,
                std::thread file_thread,
                const bool should_notify) {
            std::thread(
                [state = std::move(state),
                 http_server = std::move(http_server),
                 http_thread = std::move(http_thread),
                 file_thread = std::move(file_thread),
                 should_notify]() mutable {
                    finalize_runtime_stop(
                        state,
                        std::move(http_server),
                        std::move(http_thread),
                        std::move(file_thread),
                        should_notify);
                }).detach();
        }

        static void finalize_failed_start_from_state(
                const std::shared_ptr<RuntimeState>& state,
                std::string connection_id,
                std::string message) {
            std::shared_ptr<HttpServer> http_server;
            std::thread http_thread;
            std::thread file_thread;
            bool should_finalize = false;

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                should_finalize =
                    state->running ||
                    static_cast<bool>(state->http_server) ||
                    state->http_thread.joinable() ||
                    state->file_thread.joinable();
                if (should_finalize && !state->stopping) {
                    state->stopping = true;
                    state->running = false;
                    state->stop_requested = true;
                    http_server = std::move(state->http_server);
                    if (state->http_thread.joinable()) {
                        http_thread = std::move(state->http_thread);
                    }
                    if (state->file_thread.joinable()) {
                        file_thread = std::move(state->file_thread);
                    }
                } else {
                    should_finalize = false;
                }
            }
            state->cv.notify_all();

            try {
                notify_status_from_state(
                    state,
                    BridgeStatus::SERVER_START_FAILED,
                    std::move(connection_id),
                    std::move(message));
            } catch (...) {
                // Startup failure cleanup must still run even if a status hook throws.
            }

            if (should_finalize) {
                finalize_runtime_stop_async(
                    state,
                    std::move(http_server),
                    std::move(http_thread),
                    std::move(file_thread),
                    true);
            }
        }

        static void drain_pending_callback_shutdown(
                const std::shared_ptr<RuntimeState>& state) {
            std::shared_ptr<HttpServer> http_server;
            std::thread http_thread;
            std::thread file_thread;
            bool should_finalize = false;

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                should_finalize =
                    state->pending_callback_shutdown &&
                    state->active_http_requests == 0 &&
                    state->stopping;
                if (should_finalize) {
                    state->pending_callback_shutdown = false;
                    http_server = std::move(state->http_server);
                    if (state->http_thread.joinable()) {
                        http_thread = std::move(state->http_thread);
                    }
                    if (state->file_thread.joinable()) {
                        file_thread = std::move(state->file_thread);
                    }
                }
            }

            if (should_finalize) {
                finalize_runtime_stop_async(
                    state,
                    std::move(http_server),
                    std::move(http_thread),
                    std::move(file_thread),
                    true);
            }
        }

        void shutdown_impl() {
            std::shared_ptr<HttpServer> http_server;
            std::thread http_thread;
            std::thread file_thread;
            bool should_notify = false;
            bool async_finalize = false;

            {
                std::unique_lock<std::mutex> lock(m_state->mutex);
                if (m_state->stopping) {
                    if (!is_runtime_thread_locked() && !is_inside_callback(m_state.get())) {
                        m_state->cv.wait(lock, [state = m_state]() { return !state->stopping; });
                    }
                    return;
                }

                should_notify =
                    m_state->running ||
                    static_cast<bool>(m_state->http_server) ||
                    m_state->http_thread.joinable() ||
                    m_state->file_thread.joinable();
                if (!should_notify) {
                    return;
                }

                if (is_inside_callback(m_state.get())) {
                    m_state->stopping = true;
                    m_state->running = false;
                    m_state->stop_requested = true;
                    m_state->pending_callback_shutdown = true;
                    lock.unlock();
                    m_state->cv.notify_all();
                    drain_pending_callback_shutdown(m_state);
                    return;
                }

                async_finalize = is_runtime_thread_locked();
                m_state->stopping = true;
                m_state->running = false;
                m_state->stop_requested = true;
                m_state->pending_callback_shutdown = false;
                http_server = std::move(m_state->http_server);
                if (m_state->http_thread.joinable()) {
                    http_thread = std::move(m_state->http_thread);
                }
                if (m_state->file_thread.joinable()) {
                    file_thread = std::move(m_state->file_thread);
                }
            }
            m_state->cv.notify_all();

            auto finalize =
                [state = m_state,
                 http_server = std::move(http_server),
                 http_thread = std::move(http_thread),
                 file_thread = std::move(file_thread),
                 should_notify]() mutable {
                    if (http_server) {
                        http_server->stop();
                    }
                    if (http_thread.joinable()) {
                        if (http_thread.get_id() == std::this_thread::get_id()) {
                            http_thread.detach();
                        } else {
                            http_thread.join();
                        }
                    }
                    if (file_thread.joinable()) {
                        if (file_thread.get_id() == std::this_thread::get_id()) {
                            file_thread.detach();
                        } else {
                            file_thread.join();
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->stopping = false;
                        state->stop_requested = false;
                    }
                    state->cv.notify_all();
                    if (should_notify) {
                        notify_status_from_state(state, BridgeStatus::SERVER_STOPPED);
                    }
                };

            if (async_finalize) {
                std::thread(std::move(finalize)).detach();
            } else {
                finalize();
            }
        }
    };

} // namespace optionx::bridges::bot_binary

#endif // OPTIONX_HEADER_BRIDGES_BOT_BINARY_BOT_BINARY_BRIDGE_HPP_INCLUDED
