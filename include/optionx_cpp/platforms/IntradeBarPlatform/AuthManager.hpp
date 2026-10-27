#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_AUTH_MANAGER_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_AUTH_MANAGER_HPP_INCLUDED

/// \file AuthManager.hpp
/// \brief Handles authentication management for the Intrade Bar platform.

namespace optionx::platforms::intrade_bar {

    /// \brief Versioned data stored for a modern Intrade session cache.
    struct CachedAuthSession {
        std::string user_id;
        std::string cookies;
    };

    /// \brief Serializes the modern session identity together with its cookies.
    /// \details Older builds stored only the cookie header. Keeping the user ID
    /// in the same envelope is required when the broker returns PHPSESSID-only
    /// cookies, because legacy endpoints still require the numeric user ID.
    inline std::string serialize_cached_auth_session(
            const std::string& user_id,
            const std::string& cookies) {
        return nlohmann::json{
            {"version", 1},
            {"user_id", user_id},
            {"cookies", cookies}
        }.dump();
    }

    /// \brief Parses a versioned session envelope.
    /// \return Parsed session data, or std::nullopt for the legacy plain-cookie format.
    inline std::optional<CachedAuthSession> parse_cached_auth_session(
            const std::string& value) {
        try {
            const auto envelope = nlohmann::json::parse(value);
            if (!envelope.is_object() ||
                envelope.value("version", 0) != 1 ||
                !envelope.contains("cookies") ||
                !envelope.at("cookies").is_string()) {
                return std::nullopt;
            }

            return CachedAuthSession{
                envelope.value("user_id", std::string()),
                envelope.at("cookies").get<std::string>()};
        } catch (const nlohmann::json::exception&) {
            return std::nullopt;
        }
    }

    /// \class AuthManager
    /// \brief Manages the authentication process for the Intrade Bar platform.
    /// \details Handles user authentication, session management, and account information updates.
    class AuthManager final : public components::BaseComponent {
    public:

        /// \brief Constructs the authentication manager.
        /// \param platform Reference to the trading platform.
        /// \param request_manager Reference to the request manager for making HTTP requests.
        /// \param account_info Shared pointer to the account information data.
        explicit AuthManager(
                BaseTradingPlatform& platform,
                RequestManager& request_manager,
                std::shared_ptr<BaseAccountInfoData> account_info)
                : BaseComponent(platform.event_bus()),
                  m_request_manager(request_manager),
                  m_account_info(std::move(account_info))  {
            subscribe<events::AuthDataEvent>();
            subscribe<events::ConnectRequestEvent>();
            subscribe<events::RestartAuthEvent>();
            subscribe<events::DisconnectRequestEvent>();
            platform.register_component(this);
        }

        /// \brief Default destructor.
        virtual ~AuthManager() = default;

        /// \brief Processes incoming events and dispatches them to the appropriate handlers.
        /// \param event The received event.
        void on_event(const utils::Event* const event) override;

        /// \brief Processes internal authentication tasks.
        void process() override;

        /// \brief Shuts down authentication-related operations.
        void shutdown() override;

    private:
        RequestManager&    m_request_manager; ///< Reference to the request manager.
        utils::TaskManager m_task_manager;    ///< Task manager for handling async tasks.
        std::shared_ptr<BaseAccountInfoData> m_account_info; ///< Shared pointer to account information.
        std::unique_ptr<AuthData> m_temp_auth_data;        ///< Temporary authentication data storage.
        std::shared_ptr<AuthData> m_auth_data; ///< Currently active authentication data.
        std::uint64_t m_auth_generation = 0; ///< Monotonic auth flow generation for stale callback filtering.
        std::uint64_t m_pending_connect_generation = 0; ///< Generation of the active public connect callback.
        connection_callback_t m_pending_connect_callback; ///< Active public connect callback, if any.

        /// \brief Internal continuation between auth stages.
        /// \details Stage callbacks propagate profile/switch state inside one
        /// auth flow. Terminal public connect completion must go through
        /// complete_auth_generation().
        using auth_stage_callback_t = connection_callback_t;

        /// \brief Starts a new authentication generation and invalidates older callbacks.
        /// \param reason Human-readable trigger for diagnostics.
        /// \param callback Public connect callback to complete or cancel.
        /// \return Generation token to capture in async callbacks.
        std::uint64_t begin_auth_generation(
            const char* reason,
            connection_callback_t callback = nullptr);

        /// \brief Invalidates active authentication callbacks.
        /// \param reason Human-readable trigger for diagnostics.
        void invalidate_auth_generation(const char* reason);

        /// \brief Completes the active public connect callback if it belongs to a generation.
        /// \param generation Generation token captured for the auth flow.
        /// \param result Terminal connection result.
        /// \param reason Human-readable completion context for diagnostics.
        /// \return True if the auth flow is still current and terminal handling may continue.
        bool complete_auth_generation(
            std::uint64_t generation,
            ConnectionResult result,
            const char* reason);

        /// \brief Cancels the active public connect callback, if any.
        /// \param reason Human-readable cancellation reason.
        void cancel_pending_connect_callback(const char* reason);

        /// \brief Checks whether a callback still belongs to the active auth flow.
        /// \param generation Generation token captured when the request was sent.
        /// \param reason Human-readable callback context for diagnostics.
        /// \return True if the callback may continue the auth flow.
        bool is_auth_generation_current(
            std::uint64_t generation,
            const char* reason) const;

        /// \brief Stores authentication credentials after a successful login.
        /// \param user_id The user ID obtained after login.
        /// \param user_hash The user authentication hash.
        /// \param cookies Optional complete cookie jar returned by the broker.
        void set_auth_credentials(
                const std::string& user_id,
                const std::string& user_hash,
                const std::string& cookies = {});

        /// \brief Stores a modern trading-SPA session after email/password login.
        /// \param user_id Optional broker user ID returned by the login JSON.
        /// \param cookies Session cookies returned by the trading backend.
        void set_auth_session(
                const std::string& user_id,
                const std::string& cookies);

        /// \brief Stores the numeric broker user id in shared account info.
        /// \param user_id Broker user id string from cookies/token auth.
        void set_account_user_id(const std::string& user_id);
        
        /// \brief Handles optional domain discovery and proceeds with authentication 
        /// \param callback Callback to be invoked with the result of the authentication process.
        /// \param auth_func Function to perform the actual authentication (email/password or token).
        void handle_auto_domain_and_auth(
                std::uint64_t generation,
                connection_callback_t callback,
                std::function<void(std::uint64_t, connection_callback_t)> auth_func);

        /// \brief Initiates email/password validation for authentication.
        /// \param generation Auth flow generation token.
        /// \param connect_callback The callback function to notify the result.
        void validate_email_pass(
            std::uint64_t generation,
            connection_callback_t connect_callback);

        /// \brief Initiates user token validation for authentication.
        /// \param generation Auth flow generation token.
        /// \param connect_callback The callback function to notify the result.
        void validate_user_token(
            std::uint64_t generation,
            connection_callback_t connect_callback);

        /// \brief Completes the authentication process after login.
        /// \param generation Auth flow generation token.
        /// \param connect_callback The callback function to notify the result.
        void finalize_authentication(
            std::uint64_t generation,
            connection_callback_t connect_callback);

        /// \brief Starts the authentication process.
        /// \param generation Auth flow generation token.
        /// \param stage_callback Internal stage continuation callback.
        void start_authentication(
            std::uint64_t generation,
            auth_stage_callback_t stage_callback);

        /// \brief Handles authentication failure.
        /// \param generation Auth flow generation token.
        /// \param reason The reason for the failure.
        /// \param callback The callback function to notify the result.
        void handle_auth_failure(
            std::uint64_t generation,
            const std::string& reason,
            connection_callback_t callback);

        /// \brief Executes the authentication process, handling necessary steps.
        /// \param generation Auth flow generation token.
        /// \param callback The callback function to notify the result.
        void execute_authentication_flow(
            std::uint64_t generation,
            connection_callback_t callback);

        /// \brief Handles an account type switch (e.g., Demo to Real).
        /// \param generation Auth flow generation token.
        /// \param account_type The new account type.
        /// \param currency The currency type.
        /// \param stage_callback Internal stage continuation callback.
        void handle_account_type_switch(
            std::uint64_t generation,
            AccountType account_type,
            CurrencyType currency,
            auth_stage_callback_t stage_callback);

        /// \brief Attempts an account type switch and retries while broker reports active trades.
        /// \param generation Auth flow generation token.
        /// \param currency Current currency from the profile response.
        /// \param stage_callback Internal stage continuation callback.
        /// \param started_ms First attempt timestamp.
        /// \param attempt Attempt number.
        void handle_account_type_switch_attempt(
            std::uint64_t generation,
            CurrencyType currency,
            auth_stage_callback_t stage_callback,
            int64_t started_ms,
            int attempt);

        /// \brief Handles currency switch during authentication.
        /// \param generation Auth flow generation token.
        /// \param currency The new currency type.
        /// \param stage_callback Internal stage continuation callback.
        void handle_currency_switch(
            std::uint64_t generation,
            CurrencyType currency,
            auth_stage_callback_t stage_callback);

        /// \brief Attempts a currency switch and retries while broker reports active trades.
        /// \param generation Auth flow generation token.
        /// \param stage_callback Internal stage continuation callback.
        /// \param started_ms First attempt timestamp.
        /// \param attempt Attempt number.
        void handle_currency_switch_attempt(
            std::uint64_t generation,
            auth_stage_callback_t stage_callback,
            int64_t started_ms,
            int attempt);

        /// \brief Schedules retry for a broker settings switch after inspecting active trades.
        /// \param generation Auth flow generation token.
        /// \param operation_name Human-readable operation name.
        /// \param started_ms First attempt timestamp.
        /// \param attempt Failed attempt number.
        /// \param stage_callback Internal stage continuation callback.
        /// \param retry Retry function.
        void schedule_settings_switch_retry(
            std::uint64_t generation,
            std::string operation_name,
            int64_t started_ms,
            int attempt,
            auth_stage_callback_t stage_callback,
            std::function<void()> retry);

        /// \brief Performs a multi-step authentication flow, making HTTP requests in sequence.
        /// \details Steps:
        /// 1. Requests the main page to obtain necessary authentication parameters.
        /// 2. Sends login credentials to obtain the user ID and hash.
        /// 3. Uses the obtained credentials to authenticate.
        /// If any step fails, the process stops and the callback is invoked with an error message.
        /// \param generation Auth flow generation token.
        /// \param result_callback The callback function to be called upon success or failure.
        void perform_auth_flow(
            std::uint64_t generation,
            std::function<void(
                bool success,
                const std::string& reason)> result_callback);

        /// \brief Handles an authentication event.
        /// \param event The received authentication data event.
        void handle_event(const events::AuthDataEvent& event);

        /// \brief Handles a connection request event.
        /// \param event The received connection request event.
        void handle_event(const events::ConnectRequestEvent& event);

        /// \brief Handles an authentication restart request event.
        /// \param event The received restart authentication request event.
        void handle_event(const events::RestartAuthEvent& event);

        /// \brief Handles a disconnection request event.
        /// \param event The received disconnection request event.
        void handle_event(const events::DisconnectRequestEvent& event);

        /// \brief Retrieves the account information object.
        /// \return Shared pointer to `AccountInfoData`.
        std::shared_ptr<AccountInfoData> get_account_info();
    };

    inline void AuthManager::on_event(const utils::Event* const event) {
        if (const auto* msg = dynamic_cast<const events::AuthDataEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::ConnectRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::DisconnectRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::RestartAuthEvent*>(event)) {
            handle_event(*msg);
        }
    };

    inline void AuthManager::process() {
        m_task_manager.process();
    }

    inline void AuthManager::shutdown() {
        invalidate_auth_generation("shutdown");
        m_task_manager.shutdown();
    }

    inline std::uint64_t AuthManager::begin_auth_generation(
            const char* reason,
            connection_callback_t callback) {
        cancel_pending_connect_callback(reason);
        const std::uint64_t generation = ++m_auth_generation;
        if (callback) {
            m_pending_connect_generation = generation;
            m_pending_connect_callback = std::move(callback);
        }
        LOGIT_DEBUG(
            "Intrade Bar auth: generation started. reason=",
            reason,
            ", generation=",
            generation);
        return generation;
    }

    inline void AuthManager::invalidate_auth_generation(const char* reason) {
        const std::uint64_t generation = ++m_auth_generation;
        cancel_pending_connect_callback(reason);
        LOGIT_DEBUG(
            "Intrade Bar auth: generation invalidated. reason=",
            reason,
            ", generation=",
            generation);
    }

    inline bool AuthManager::complete_auth_generation(
            std::uint64_t generation,
            ConnectionResult result,
            const char* reason) {
        if (!is_auth_generation_current(generation, reason)) {
            return false;
        }

        connection_callback_t callback;
        if (m_pending_connect_generation == generation) {
            callback = std::move(m_pending_connect_callback);
            m_pending_connect_generation = 0;
        }

        ++m_auth_generation;
        LOGIT_DEBUG(
            "Intrade Bar auth: generation completed. reason=",
            reason,
            ", completed_generation=",
            generation,
            ", current_generation=",
            m_auth_generation);

        if (callback) {
            callback(std::move(result));
        }
        return true;
    }

    inline void AuthManager::cancel_pending_connect_callback(const char* reason) {
        if (!m_pending_connect_callback) {
            m_pending_connect_generation = 0;
            return;
        }

        auto callback = std::move(m_pending_connect_callback);
        m_pending_connect_generation = 0;
        const std::string message =
            std::string("Authentication cancelled: ") + reason;
        LOGIT_DEBUG("Intrade Bar auth: cancelling pending connect. reason=", reason);
        callback(ConnectionResult(
            false,
            message,
            m_auth_data ? m_auth_data->clone_unique() : nullptr));
    }

    inline bool AuthManager::is_auth_generation_current(
            std::uint64_t generation,
            const char* reason) const {
        const bool current = generation == m_auth_generation;
        if (!current) {
            LOGIT_DEBUG(
                "Intrade Bar auth: stale callback ignored. reason=",
                reason,
                ", callback_generation=",
                generation,
                ", current_generation=",
                m_auth_generation);
        }
        return current;
    }

    inline void AuthManager::set_auth_credentials(
            const std::string& user_id,
            const std::string& user_hash,
            const std::string& cookies) {
        const std::string session_cookies = cookies.empty()
            ? "user_id=" + user_id + "; user_hash=" + user_hash
            : cookies;
        storage::ServiceSessionDB::get_instance().set_session_value(
            to_str(PlatformType::INTRADE_BAR),
            m_auth_data->email,
            serialize_cached_auth_session(user_id, session_cookies));
        m_request_manager.set_auth_credentials(user_id, user_hash, session_cookies);
        set_account_user_id(user_id);
    }

    inline void AuthManager::set_auth_session(
            const std::string& user_id,
            const std::string& cookies) {
        storage::ServiceSessionDB::get_instance().set_session_value(
            to_str(PlatformType::INTRADE_BAR),
            m_auth_data->email,
            serialize_cached_auth_session(user_id, cookies));
        m_request_manager.set_auth_session(user_id, cookies);
        if (!user_id.empty()) {
            set_account_user_id(user_id);
        }
    }

    inline void AuthManager::set_account_user_id(const std::string& user_id) {
        auto account_info = get_account_info();
        if (auto parsed = utils::parse_i64_strict(user_id)) {
            account_info->user_id = *parsed;
        } else {
            account_info->user_id = 0;
            LOGIT_WARN(
                "Intrade Bar auth: failed to parse broker user_id. user_id=",
                user_id);
        }
    }

    inline void AuthManager::handle_event(const events::AuthDataEvent& event) {
        if (auto auth_data = std::dynamic_pointer_cast<AuthData>(event.auth_data)) {
            invalidate_auth_generation("auth-data");
            LOGIT_TRACE0();
            auto [success, message] = auth_data->validate();
            if (success) {
                m_temp_auth_data = std::make_unique<AuthData>(*auth_data.get());
            } else {
                m_temp_auth_data.reset();
            }
            auth_data->dispatch_callbacks(success, message);
        }
    }

    inline void AuthManager::handle_event(const events::ConnectRequestEvent& event) {
        LOGIT_TRACE0();
        auto callback = event.callback;
        const std::uint64_t generation =
            begin_auth_generation("connect-request", callback);
        m_task_manager.add_single_task(
                "event(ConnectRequestEvent)-single-1",
                [this, callback, generation](
                    std::shared_ptr<utils::Task> task) {
            LOGIT_TRACE0();
            m_request_manager.cancel_requests();

            if (task->is_shutdown()) {
                LOGIT_TRACE0();

                using Status = events::AccountInfoUpdateEvent::Status;
                auto account_info = get_account_info();
                if (account_info->connect) {
                    account_info->connect = false;
                    notify(events::AccountInfoUpdateEvent(
                        account_info,
                        Status::DISCONNECTED,
                        "Shutdown."));
                }
                return;
            }
            if (!is_auth_generation_current(generation, "connect-request-stage-1")) {
                return;
            }

            // Обработаем подключение после получения результата всех HTTP запросов
            LOGIT_TRACE0();
            m_task_manager.add_single_task(
                    "event(ConnectRequestEvent)-single-2",
                    [this, callback, generation](
                        std::shared_ptr<utils::Task> task) {
                LOGIT_TRACE0();
                using Status = events::AccountInfoUpdateEvent::Status;
                if (task->is_shutdown()) {
                    LOGIT_TRACE0();

                    auto account_info = get_account_info();
                    if (account_info->connect) {
                        account_info->connect = false;
                        notify(events::AccountInfoUpdateEvent(
                            account_info,
                            Status::DISCONNECTED,
                            "Shutdown."));
                    }
                    return;
                }
                if (!is_auth_generation_current(generation, "connect-request-stage-2")) {
                    return;
                }

                LOGIT_TRACE0();
                auto account_info = get_account_info();

                if (account_info->connect) {
                    LOGIT_TRACE0();
                    account_info->connect = false;
                    notify(events::AccountInfoUpdateEvent(
                        account_info,
                        Status::DISCONNECTED,
                        "Reconnecting."));
                }

                // Отменяем все запросы
                LOGIT_TRACE0();
                m_request_manager.cancel_requests();

                LOGIT_TRACE0();
                if (!m_temp_auth_data) {
                    const std::string error_text("Authentication data is missing.");
                    handle_auth_failure(generation, error_text, std::move(callback));
                    return;
                }

                m_auth_data = std::make_shared<AuthData>(*m_temp_auth_data.get());
                account_info->account_type = m_auth_data->account_type;
                account_info->currency     = m_auth_data->currency;
                account_info->order_interval_ms = m_auth_data->order_interval_ms;
                account_info->user_id = 0;
                notify(events::AccountInfoUpdateEvent(
                    account_info,
                    Status::CONNECTING));

                switch (m_auth_data->auth_method) {
                case AuthMethod::NONE: {
                        const std::string error_text("Authentication method is not specified.");
                        LOGIT_ERROR(error_text);
                        if (complete_auth_generation(
                                generation,
                                ConnectionResult(
                                    false,
                                    error_text,
                                    m_auth_data->clone_unique()),
                                "auth-method-none")) {
                            notify(events::AccountInfoUpdateEvent(
                                account_info,
                                Status::FAILED_TO_CONNECT,
                                error_text));
                        }
                    } break;
                case AuthMethod::EMAIL_PASSWORD:
                    handle_auto_domain_and_auth(generation, callback, [this](auto gen, auto cb) {
                        // Process authentication via email and password
                        validate_email_pass(gen, std::move(cb));
                    });
                    break;
                case AuthMethod::USER_TOKEN:
                    handle_auto_domain_and_auth(generation, callback, [this](auto gen, auto cb) {
                        // Validate user authentication token
                        validate_user_token(gen, std::move(cb));
                    });
                    break;
                default: {
                        const std::string error_text("Unsupported authentication method.");
                        handle_auth_failure(generation, error_text, std::move(callback));
                    } break;
                };
            });
        });
    }

    inline void AuthManager::handle_event(const events::DisconnectRequestEvent& event) {
        LOGIT_TRACE0();
        invalidate_auth_generation("disconnect-request");
        auto callback = event.callback;
        m_task_manager.add_single_task(
                "event(DisconnectRequestEvent)-single-1",
                [this, callback](
                    std::shared_ptr<utils::Task> task) {
            LOGIT_INFO("Cancelling all pending requests before disconnecting.");
            m_request_manager.cancel_requests();

            if (task->is_shutdown()) {
                using Status = events::AccountInfoUpdateEvent::Status;
                auto account_info = get_account_info();
                if (account_info->connect) {
                    account_info->connect = false;
                    notify(events::AccountInfoUpdateEvent(
                        account_info,
                        Status::DISCONNECTED,
                        "Shutdown."));
                }
                return;
            }

            // Обработаем отключение после получения результата всех HTTP запросов
            m_task_manager.add_single_task(
                    "event(DisconnectRequestEvent)-single-2",
                    [this, callback](
                        std::shared_ptr<utils::Task> task) {
                auto account_info = get_account_info();
                if (account_info->connect) {
                    using Status = events::AccountInfoUpdateEvent::Status;
                    account_info->connect = false;
                    LOGIT_INFO("Disconnected from the platform.");
                    const std::string event_text("Successfully disconnected.");
                    callback({true, event_text, m_auth_data ? m_auth_data->clone_unique() : nullptr});
                    notify(events::AccountInfoUpdateEvent(
                        get_account_info(),
                        Status::DISCONNECTED,
                        event_text));
                } else {
                    LOGIT_WARN("Disconnect request received, but the account was already disconnected.");
                    const std::string error_text("Already disconnected.");
                    callback({false, error_text, m_auth_data ? m_auth_data->clone_unique() : nullptr});
                }
            });
        });
    }

    inline void AuthManager::handle_event(const events::RestartAuthEvent& event) {
        const std::uint64_t generation = begin_auth_generation("restart-auth");
        start_authentication(generation, [this, generation](const ConnectionResult& result) {
            if (!is_auth_generation_current(generation, "restart-auth-profile")) {
                return;
            }
            if (!result.success) {
                auto account_info = get_account_info();
                if (account_info->connect) {
                    using Status = events::AccountInfoUpdateEvent::Status;
                    account_info->connect = false;
                    notify(events::AccountInfoUpdateEvent(
                        get_account_info(),
                        Status::DISCONNECTED,
                        result.reason));
                }
                return;
            }

            finalize_authentication(
                generation,
                [this](const ConnectionResult& result) {
            });
        });
    }
    
    inline void AuthManager::handle_auto_domain_and_auth(
            std::uint64_t generation,
            connection_callback_t callback,
            std::function<void(std::uint64_t, connection_callback_t)> auth_func) {

        if (m_auth_data->auto_find_domain) {
            LOGIT_INFO(
                "Intrade Bar auth: auto domain discovery enabled. range=",
                m_auth_data->domain_index_min,
                "-",
                m_auth_data->domain_index_max);
            m_request_manager.request_find_working_domain(
                [this, callback, auth_func, generation](bool success, std::string& host) {
                    if (!is_auth_generation_current(generation, "auto-domain")) {
                        return;
                    }
                    using Status = events::AccountInfoUpdateEvent::Status;
                    if (!success) {
                        const std::string error_text = "No working domain found.";
                        if (complete_auth_generation(
                                generation,
                                ConnectionResult(
                                    false,
                                    error_text,
                                    m_auth_data->clone_unique()),
                                "auto-domain-failed")) {
                            notify(events::AccountInfoUpdateEvent(
                                get_account_info(),
                                Status::FAILED_TO_CONNECT,
                                error_text));
                        }
                        return;
                    }

                    notify(events::AutoDomainSelectedEvent(
                        success,
                        host));
                    LOGIT_INFO("Intrade Bar auth: selected host=", host);
                            
                    auth_func(generation, std::move(callback));
                });
        } else {
            LOGIT_INFO("Intrade Bar auth: using configured host.");
            auth_func(generation, std::move(callback));
        }
    }

    inline void AuthManager::validate_email_pass(
            std::uint64_t generation,
            connection_callback_t callback) {
        if (!is_auth_generation_current(generation, "validate-email-pass")) {
            return;
        }
        // Validate email and password
        if (m_auth_data->email.empty() ||
            m_auth_data->password.empty()) {
            LOGIT_ERROR_IF(m_auth_data->email.empty(), "Validation failed: Email is missing.");
            LOGIT_ERROR_IF(m_auth_data->password.empty(), "Validation failed: Password is missing.");

            const std::string error_text("Login failed: Please provide both email and password.");
            handle_auth_failure(generation, error_text, std::move(callback));
            return;
        }

        // Attempt to retrieve session from storage
        auto session = storage::ServiceSessionDB::get_instance().get_session_value(
            to_str(PlatformType::INTRADE_BAR),
            m_auth_data->email);

        if (!session) {
            LOGIT_INFO("Intrade Bar auth: saved session not found, starting fresh login flow.");
            LOGIT_TRACE("Session not found for configured Intrade Bar account.");
            execute_authentication_flow(generation, std::move(callback));
            return;
        }

        LOGIT_INFO("Intrade Bar auth: saved session found, validating it.");
        LOGIT_TRACE("Session retrieved for configured Intrade Bar account.");

        // Set cookies from the session. Current values use a versioned
        // envelope so PHPSESSID-only sessions retain the JSON user ID;
        // older installations continue to provide a plain cookie string.
        const auto cached_session = parse_cached_auth_session(*session);
        std::string cookies = cached_session
            ? cached_session->cookies
            : *session;
        const std::string cached_user_id = cached_session
            ? cached_session->user_id
            : std::string();
        auto parsed_data = parse_cookies(cookies);

        if (parsed_data) {
            const auto& [user_id, user_hash] = *parsed_data;
            LOGIT_PRINT_TRACE("Cookies parsed successfully for user_id: ", user_id);
            m_request_manager.set_auth_credentials(user_id, user_hash, cookies);
            set_account_user_id(user_id);

            // Start authentication flow
            start_authentication(
                    generation,
                    [this, callback, generation](const ConnectionResult &result) {
                if (!is_auth_generation_current(generation, "cached-session-profile")) {
                    return;
                }
                if (!result.success) {
                    LOGIT_PRINT_ERROR("Authentication failed: ", result.reason);
                    execute_authentication_flow(generation, std::move(callback));
                    return;
                }

                    finalize_authentication(generation, std::move(callback));
                });
        } else if (has_cookie(cookies, "PHPSESSID")) {
            // Modern trading login sessions contain PHPSESSID rather than the
            // retired user_id/user_hash cookie pair.
            LOGIT_INFO("Intrade Bar auth: saved trading session found, validating it.");
            const auto user_id = !cached_user_id.empty()
                ? cached_user_id
                : parse_cookie_value(cookies, "user_id").value_or(std::string());
            m_request_manager.set_auth_session(user_id, cookies);
            set_account_user_id(user_id);
            start_authentication(
                    generation,
                    [this, callback, generation](const ConnectionResult& result) {
                if (!is_auth_generation_current(generation, "cached-trading-session-profile")) {
                    return;
                }
                if (!result.success) {
                    LOGIT_PRINT_ERROR("Trading session authentication failed: ", result.reason);
                    execute_authentication_flow(generation, std::move(callback));
                    return;
                }

                finalize_authentication(generation, std::move(callback));
            });
        } else {
            LOGIT_PRINT_ERROR("Failed to parse cookies: ", utils::redact_secret_value(cookies));
            execute_authentication_flow(generation, std::move(callback));
        }
    }

    inline void AuthManager::validate_user_token(
            std::uint64_t generation,
            connection_callback_t callback) {
        if (!is_auth_generation_current(generation, "validate-user-token")) {
            return;
        }
        // Validate user_id and token
        if (m_auth_data->user_id.empty() || m_auth_data->token.empty()) {
            LOGIT_ERROR_IF(m_auth_data->user_id.empty(), "Validation failed: User ID is missing.");
            LOGIT_ERROR_IF(m_auth_data->token.empty(), "Validation failed: Token is missing.");

            const std::string error_text("Validation failed: User ID or token is missing.");
            handle_auth_failure(generation, error_text, std::move(callback));
            return;
        }

        LOGIT_TRACE("Validation passed for user_id: ", m_auth_data->user_id);

        // Set user ID, token, and cookies
        m_request_manager.set_auth_credentials(m_auth_data->user_id, m_auth_data->token);
        set_account_user_id(m_auth_data->user_id);
        start_authentication(generation, [this, callback, generation](const ConnectionResult& result) {
            if (!is_auth_generation_current(generation, "user-token-profile")) {
                return;
            }
            if (!result.success) {
                const std::string error_text("Login failed: Please provide both User ID and token.");
                handle_auth_failure(generation, error_text, std::move(callback));
                return;
            }

            finalize_authentication(generation, std::move(callback));
        });
    }

    inline void AuthManager::handle_auth_failure(
            std::uint64_t generation,
            const std::string& reason,
            connection_callback_t callback) {
        if (!is_auth_generation_current(generation, "auth-failure")) {
            return;
        }
        using Status = events::AccountInfoUpdateEvent::Status;
        LOGIT_PRINTF_ERROR("Authentication failed: ", reason);
        if (complete_auth_generation(
                generation,
                ConnectionResult(
                    false,
                    reason,
                    m_auth_data ? m_auth_data->clone_unique() : nullptr),
                "auth-failure")) {
            notify(events::AccountInfoUpdateEvent(
                get_account_info(),
                Status::FAILED_TO_CONNECT,
                reason));
        }
    }

    inline void AuthManager::execute_authentication_flow(
            std::uint64_t generation,
            connection_callback_t callback) {
        perform_auth_flow(generation, [this, callback, generation](
                bool success,
                const std::string& reason) {
            if (!is_auth_generation_current(generation, "fresh-auth-flow")) {
                return;
            }
            if (!success) {
                handle_auth_failure(generation, reason, std::move(callback));
                return;
            }

            start_authentication(generation, [this, callback, generation](const ConnectionResult& result) {
                if (!is_auth_generation_current(generation, "fresh-auth-profile")) {
                    return;
                }
                if (!result.success) {
                    handle_auth_failure(generation, result.reason, std::move(callback));
                    return;
                }

                finalize_authentication(generation, std::move(callback));
            });
        });
    }

    inline void AuthManager::perform_auth_flow(
            std::uint64_t generation,
            std::function<void(
                bool success,
                const std::string& reason)> result_callback) {
        LOGIT_SCOPE_INFO("intradebar.auth.fresh_login_flow");
        LOGIT_TRACE0();

        // Step 3: Handle authentication response
        auto handle_auth_response = [this, result_callback, generation](
                    bool success,
                    const std::string& user_id,
                    const std::string& user_hash,
                    const std::string& cookies,
                    const std::string& reason) {
            if (!is_auth_generation_current(generation, "auth-endpoint")) {
                return;
            }
            if (!success) {
                LOGIT_WARN(
                    "Intrade Bar auth: authentication flow failed. reason=",
                    reason);
                result_callback(false, reason);
                return;
            }

            // Store either the legacy user_id/user_hash pair or the current
            // trading-SPA session cookie, depending on the login response.
            LOGIT_INFO("Intrade Bar auth: fresh credentials accepted, storing session.");
            if (user_hash.empty()) {
                set_auth_session(user_id, cookies);
            } else {
                LOGIT_PRINT_TRACE("Authentication successful. User ID: ", user_id);
                set_auth_credentials(user_id, user_hash, cookies);
            }
            result_callback(true, std::string());
        };

        // Step 2: Handle login response
        auto handle_login_response = [this, handle_auth_response, generation](
                bool success,
                const std::string& user_id,
                const std::string& user_hash,
                const std::string& cookies,
                const std::string& reason) {
            if (!is_auth_generation_current(generation, "login-endpoint")) {
                return;
            }
            if (!success) {
                LOGIT_WARN("Intrade Bar auth: login step failed.");
                handle_auth_response(false, std::string(), std::string(), std::string(), reason);
                return;
            }

            if (user_hash.empty()) {
                handle_auth_response(true, user_id, user_hash, cookies, std::string());
                return;
            }

            // Proceed to authentication
            LOGIT_INFO("Intrade Bar auth: login step succeeded, checking auth endpoint.");
            m_request_manager.request_auth(user_id, user_hash, cookies, m_auth_data,
                [handle_auth_response, user_id, user_hash, cookies](
                        bool success,
                        const std::string& reason) {
                    handle_auth_response(success, user_id, user_hash, cookies, reason);
                });
        };

        // Step 1: Handle main page response
        auto handle_main_page_response = [this, handle_login_response, generation](
                    bool success,
                    const MainPageChallenge& challenge,
                    const std::string& reason) {
            if (!is_auth_generation_current(generation, "main-page-challenge")) {
                return;
            }
            if (!success) {
                LOGIT_WARN("Intrade Bar auth: main page challenge step failed.");
                handle_login_response(false, std::string(), std::string(), std::string(), reason);
                return;
            }

            // Proceed to login
            LOGIT_INFO(
                "Intrade Bar auth: login page recognized as ",
                login_page_flow_to_string(challenge.flow),
                ", sending login.");
            m_request_manager.request_login(challenge, m_auth_data,
                [handle_login_response](
                        bool success,
                        const std::string& user_id,
                        const std::string& user_hash,
                        const std::string& cookies,
                        const std::string& reason) {
                    handle_login_response(success, user_id, user_hash, cookies, reason);
                });
        };

        // Start the authentication process
        LOGIT_INFO("Intrade Bar auth: requesting main page challenge.");
        m_request_manager.request_main_page(m_auth_data, handle_main_page_response);
    }

    inline void AuthManager::start_authentication(
            std::uint64_t generation,
            auth_stage_callback_t stage_callback) {
        LOGIT_TRACE0();
        LOGIT_INFO("Intrade Bar auth: requesting profile.");
        m_request_manager.request_profile([this, stage_callback, generation](
                bool success,
                CurrencyType currency,
                AccountType account_type) {
            if (!is_auth_generation_current(generation, "profile")) {
                return;
            }
            if (!success) {
                const std::string error_text = "Failed to retrieve profile information.";
                LOGIT_ERROR(error_text);
                stage_callback({false, error_text, m_auth_data->clone_unique()});
                return;
            }
            handle_account_type_switch(generation, account_type, currency, std::move(stage_callback));
        });
    }

    inline void AuthManager::handle_account_type_switch(
            std::uint64_t generation,
            AccountType account_type,
            CurrencyType currency,
            auth_stage_callback_t stage_callback) {
        LOGIT_TRACE0();
        if (!is_auth_generation_current(generation, "account-type-switch")) {
            return;
        }
        if (m_auth_data->account_type != account_type) {
            LOGIT_INFO(
                "Intrade Bar auth: switching account type from ",
                to_str(account_type),
                " to ",
                to_str(m_auth_data->account_type));
            handle_account_type_switch_attempt(
                generation,
                currency,
                std::move(stage_callback),
                OPTIONX_TIMESTAMP_MS,
                1);
        } else {
            handle_currency_switch(generation, currency, std::move(stage_callback));
        }
    }

    inline void AuthManager::handle_account_type_switch_attempt(
            std::uint64_t generation,
            CurrencyType currency,
            auth_stage_callback_t stage_callback,
            int64_t started_ms,
            int attempt) {
        LOGIT_INFO(
            "Intrade Bar auth: account type switch attempt=",
            attempt);
        m_request_manager.request_switch_account_type_result(
            [this, currency, stage_callback, started_ms, attempt, generation](SettingsSwitchResult result) {
                if (!is_auth_generation_current(generation, "account-type-switch-result")) {
                    return;
                }
                if (!result) {
                    if (!result.value.should_retry()) {
                        const std::string error_text = result.error_message.empty()
                            ? "Failed to switch account type."
                            : result.error_message;
                        LOGIT_ERROR(
                            "Intrade Bar auth: account type switch failed without retry. reason=",
                            error_text,
                            ", failure_reason=",
                            settings_switch_failure_reason_to_string(result.value.failure_reason),
                            ", status_code=",
                            result.status_code);
                        stage_callback({false, error_text, m_auth_data->clone_unique()});
                        return;
                    }

                    LOGIT_WARN(
                        "Intrade Bar auth: account type switch rejected by broker; scheduling retry. reason=",
                        result.error_message,
                        ", status_code=",
                        result.status_code);
                    schedule_settings_switch_retry(
                        generation,
                        "account type",
                        started_ms,
                        attempt,
                        stage_callback,
                        [this, currency, stage_callback, started_ms, attempt, generation]() {
                            handle_account_type_switch_attempt(
                                generation,
                                currency,
                                stage_callback,
                                started_ms,
                                attempt + 1);
                        });
                    return;
                }

                using Status = events::AccountInfoUpdateEvent::Status;
                auto account_info = get_account_info();
                // Intrade Bar "ok" is treated as broker acknowledgement of the switch.
                // We intentionally avoid an extra profile request here; smoke coverage
                // validates this broker contract, and balance/profile polling can detect
                // a future mismatch without adding latency to every successful switch.
                account_info->account_type = m_auth_data->account_type;
                notify(events::AccountInfoUpdateEvent(account_info, Status::ACCOUNT_TYPE_CHANGED));

                handle_currency_switch(generation, currency, std::move(stage_callback));
            });
    }

    inline void AuthManager::handle_currency_switch(
            std::uint64_t generation,
            CurrencyType currency,
            auth_stage_callback_t stage_callback) {
        LOGIT_TRACE0();
        if (!is_auth_generation_current(generation, "currency-switch")) {
            return;
        }
        if (m_auth_data->currency != currency) {
            LOGIT_INFO(
                "Intrade Bar auth: switching currency from ",
                to_str(currency),
                " to ",
                to_str(m_auth_data->currency));
            handle_currency_switch_attempt(
                generation,
                std::move(stage_callback),
                OPTIONX_TIMESTAMP_MS,
                1);
        } else {
            stage_callback({true, std::string(), m_auth_data->clone_unique()});
        }
    }

    inline void AuthManager::handle_currency_switch_attempt(
            std::uint64_t generation,
            auth_stage_callback_t stage_callback,
            int64_t started_ms,
            int attempt) {
        LOGIT_INFO(
            "Intrade Bar auth: currency switch attempt=",
            attempt);
        m_request_manager.request_switch_currency_result(
            [this, stage_callback, started_ms, attempt, generation](SettingsSwitchResult result) {
                if (!is_auth_generation_current(generation, "currency-switch-result")) {
                    return;
                }
                if (!result) {
                    if (!result.value.should_retry()) {
                        const std::string error_text = result.error_message.empty()
                            ? "Failed to switch currency."
                            : result.error_message;
                        LOGIT_ERROR(
                            "Intrade Bar auth: currency switch failed without retry. reason=",
                            error_text,
                            ", failure_reason=",
                            settings_switch_failure_reason_to_string(result.value.failure_reason),
                            ", status_code=",
                            result.status_code);
                        stage_callback({false, error_text, m_auth_data->clone_unique()});
                        return;
                    }

                    LOGIT_WARN(
                        "Intrade Bar auth: currency switch rejected by broker; scheduling retry. reason=",
                        result.error_message,
                        ", status_code=",
                        result.status_code);
                    schedule_settings_switch_retry(
                        generation,
                        "currency",
                        started_ms,
                        attempt,
                        stage_callback,
                        [this, stage_callback, started_ms, attempt, generation]() {
                            handle_currency_switch_attempt(
                                generation,
                                stage_callback,
                                started_ms,
                                attempt + 1);
                        });
                    return;
                }

                using Status = events::AccountInfoUpdateEvent::Status;
                auto account_info = get_account_info();
                // Intrade Bar "ok" is treated as broker acknowledgement of the switch.
                // We intentionally avoid an extra profile request here; smoke coverage
                // validates this broker contract, and balance/profile polling can detect
                // a future mismatch without adding latency to every successful switch.
                account_info->currency = m_auth_data->currency;
                notify(events::AccountInfoUpdateEvent(account_info, Status::CURRENCY_CHANGED));

                stage_callback({true, std::string(), m_auth_data->clone_unique()});
            });
    }

    inline void AuthManager::schedule_settings_switch_retry(
            std::uint64_t generation,
            std::string operation_name,
            int64_t started_ms,
            int attempt,
            auth_stage_callback_t stage_callback,
            std::function<void()> retry) {
        if (!is_auth_generation_current(generation, "settings-switch-retry-schedule")) {
            return;
        }
        const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
        const int64_t timeout_ms = m_auth_data->settings_switch_retry_timeout_ms;
        if ((now_ms - started_ms) >= timeout_ms) {
            const std::string error_text =
                "Failed to switch " + operation_name + " before retry timeout.";
            LOGIT_ERROR(error_text);
            stage_callback({false, error_text, m_auth_data->clone_unique()});
            return;
        }

        m_request_manager.request_active_trades_snapshot_result(
            [this, operation_name = std::move(operation_name), started_ms, attempt, stage_callback, retry = std::move(retry), generation](
                    ActiveTradesSnapshotResult snapshot) mutable {
                if (!is_auth_generation_current(generation, "settings-switch-active-trades")) {
                    return;
                }
                const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
                const int64_t timeout_ms = m_auth_data->settings_switch_retry_timeout_ms;
                int64_t delay_ms = m_auth_data->settings_switch_retry_delay_ms;
                int64_t latest_close_ms = 0;
                std::size_t active_count = 0;

                if (snapshot) {
                    active_count = snapshot.value.trades.size();
                    for (const auto& trade : snapshot.value.trades) {
                        if (trade.close_time_ms > latest_close_ms) {
                            latest_close_ms = trade.close_time_ms;
                        }
                    }

                    if (latest_close_ms > now_ms) {
                        delay_ms = latest_close_ms - now_ms +
                            m_auth_data->settings_switch_active_trade_buffer_ms;
                    }

                    LOGIT_INFO(
                        "Intrade Bar auth: settings switch retry after broker rejection. operation=",
                        operation_name,
                        ", attempt=",
                        attempt,
                        ", active_trades=",
                        active_count,
                        ", latest_close_ms=",
                        latest_close_ms,
                        ", delay_ms=",
                        delay_ms);
                } else {
                    LOGIT_WARN(
                        "Intrade Bar auth: active trades snapshot failed; using fallback retry delay. operation=",
                        operation_name,
                        ", attempt=",
                        attempt,
                        ", delay_ms=",
                        delay_ms);
                }

                if (delay_ms < 1000) delay_ms = 1000;

                const int64_t remaining_ms = timeout_ms - (now_ms - started_ms);
                if (remaining_ms <= 0 || delay_ms > remaining_ms) {
                    const std::string error_text =
                        "Failed to switch " + operation_name + " before retry timeout.";
                    LOGIT_ERROR(error_text);
                    stage_callback({false, error_text, m_auth_data->clone_unique()});
                    return;
                }

                m_task_manager.add_delayed_task(
                    "settings-switch-retry",
                    delay_ms,
                    [this, retry = std::move(retry), generation](std::shared_ptr<utils::Task> task) mutable {
                        if (task->is_shutdown()) return;
                        if (!is_auth_generation_current(generation, "settings-switch-retry-task")) {
                            return;
                        }
                        retry();
                    });
            });
    }

    inline void AuthManager::finalize_authentication(
            std::uint64_t generation,
            connection_callback_t callback) {
        if (!is_auth_generation_current(generation, "finalize-authentication")) {
            return;
        }
        LOGIT_TRACE0();
        LOGIT_INFO("Intrade Bar auth: requesting final balance.");
        m_request_manager.request_balance([this, callback, generation](
                bool success,
                double balance,
                CurrencyType currency) {
            if (!is_auth_generation_current(generation, "final-balance")) {
                return;
            }
            using Status = events::AccountInfoUpdateEvent::Status;
            if (!success) {
                const std::string error_text("Failed to retrieve balance.");
                LOGIT_ERROR(error_text);
                if (!complete_auth_generation(
                        generation,
                        ConnectionResult(
                            false,
                            error_text,
                            m_auth_data->clone_unique()),
                        "final-balance-failed")) {
                    return;
                }

                auto account_info = get_account_info();
                if (account_info->connect) {
                    account_info->connect = false;
                    notify(events::AccountInfoUpdateEvent(account_info, Status::DISCONNECTED, error_text));
                } else {
                    notify(events::AccountInfoUpdateEvent(account_info, Status::FAILED_TO_CONNECT, error_text));
                }
                return;
            }

            LOGIT_TRACE("Authentication successful. Connection established.");
            if (!complete_auth_generation(
                    generation,
                    ConnectionResult(
                        true,
                        std::string(),
                        m_auth_data->clone_unique()),
                    "final-balance-success")) {
                return;
            }

            auto account_info = get_account_info();
            account_info->account_type = m_auth_data->account_type;
            account_info->currency = currency;
            const double account_balance = account_info->balance;
            account_info->balance = balance;

            if (!account_info->connect) {
                account_info->connect  = true;
                notify(events::AccountInfoUpdateEvent(account_info, Status::CONNECTED));
            }

            if (std::abs(account_balance - balance) >= 0.01) {
                notify(events::AccountInfoUpdateEvent(account_info, Status::BALANCE_UPDATED));
            }
        });
    }

    inline std::shared_ptr<AccountInfoData> AuthManager::get_account_info() {
        if (auto account_info = std::dynamic_pointer_cast<AccountInfoData>(m_account_info)) {
            return account_info;
        }
        LOGIT_FATAL("Failed to cast IAccountInfoData to AccountInfoData");
        throw std::runtime_error("Invalid account information type");
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_AUTH_MANAGER_HPP_INCLUDED
