#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_REQUEST_MANAGER_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_REQUEST_MANAGER_HPP_INCLUDED

/// \file RequestManager.hpp
/// \brief Handles HTTP requests for user authentication, trade execution, balance updates, and market data retrieval.

#include <algorithm>
#include <iterator>

#include "ApiResponses.hpp"
#include "BarHistoryUtils.hpp"

namespace optionx::platforms::intrade_bar {

    /// \class RequestManager
    /// \brief Manages HTTP requests for the Intrade Bar platform.
    ///
    /// The `RequestManager` class is responsible for handling HTTP-based communication
    /// with the trading platform, including authentication, balance retrieval, price updates,
    /// and trade execution.
    class RequestManager final : public components::BaseComponent {
    public:

        /// \brief Constructs the request manager.
        /// \param platform Reference to the trading platform.
        /// \param client Reference to the HTTP client component.
        explicit RequestManager(
                BaseTradingPlatform& platform,
                HttpClientComponent& client)
                : BaseComponent(platform.event_bus()),
                  m_client(client),
                  m_binance_client("https://api.binance.com") {
            subscribe<events::AuthDataEvent>();

            m_api_headers = {
                {"Accept", "*/*"},
                {"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"},
                {"X-Requested-With", "XMLHttpRequest"}
            };

            const kurlyk::Headers headers = {
                {"Connection", "keep-alive"},
                {"Cache-Control", "no-cache"},
                {"pragma", "no-cache"},
                {"sec-ch-ua-mobile", "?0"},
                {"sec-ch-ua-platform", "Windows"},
                {"sec-fetch-dest", "document"},
                {"sec-fetch-mode", "navigate"},
                {"sec-fetch-site", "same-origin"},
                {"sec-fetch-user", "?1"},
                {"sec-gpc", "1"}
            };
            get_http_client().set_headers(headers);
            platform.register_component(this);
        }

        /// \brief Default destructor.
        virtual ~RequestManager() = default;

        /// \brief Processes incoming events and dispatches them to the appropriate handlers.
        /// \param event The received event.
        void on_event(const utils::Event* const event) override {
            if (const auto* msg = dynamic_cast<const events::AuthDataEvent*>(event)) {
                handle_event(*msg);
            }
        }

        /// \brief Sets authentication credentials for HTTP requests.
        /// \param user_id The user ID.
        /// \param user_hash The authentication hash.
        void set_auth_credentials(
                const std::string& user_id,
                const std::string& user_hash) {
            m_user_id = user_id;
            m_user_hash = user_hash;
            m_cookies = "user_id=" + user_id + "; user_hash=" + user_hash;
        }

        /// \brief Cancels all ongoing HTTP requests.
        void cancel_requests() {
            get_http_client().cancel_requests();
        }
        
        /// \brief Performs a sweep over known domain variants to find a working endpoint.
        /// \param callback Callback to receive the first working domain, if any.
        void request_find_working_domain(
            std::function<void(
                bool success,
                std::string &host)> find_callback);

        /// \brief Typed variant of request_find_working_domain.
        void request_find_working_domain_result(
            std::function<void(DomainSelectionResult)> find_callback);
                
        /// \brief Checks if the currently set host in the HTTP client is available.
        /// \param check_callback Callback that receives the result: true if available, false otherwise.
        void request_check_current_host_available(
            std::function<void(bool success)> check_callback);

        /// \brief Typed variant of request_check_current_host_available.
        void request_check_current_host_available_result(
            std::function<void(HostAvailabilityResult)> check_callback);

        /// \brief Requests user profile information.
        /// \param profile_callback Callback function to receive profile details.
        void request_profile(
            std::function<void(
                bool success,
                CurrencyType currency,
                AccountType account)> profile_callback);

        /// \brief Typed variant of request_profile.
        void request_profile_result(
            std::function<void(ProfileInfoResult)> profile_callback);

        /// \brief Requests the main page to extract authentication parameters.
        /// \param auth_data Shared pointer to authentication data.
        /// \param callback Callback function to receive authentication parameters.
        void request_main_page(
            std::shared_ptr<AuthData> auth_data,
            std::function<void(
                bool success,
                const std::string& req_id,
                const std::string& req_value,
                const std::string& cookies,
                const std::string& reason)> callback);

        /// \brief Typed variant of request_main_page.
        void request_main_page_result(
            std::shared_ptr<AuthData> auth_data,
            std::function<void(MainPageChallengeResult)> callback);

        /// \brief Requests active trades from the authenticated main page.
        /// \param callback Callback function to receive active trades.
        void request_active_trades_snapshot(
            std::function<void(
                bool success,
                std::vector<ActiveTradeInfo> trades)> callback);

        /// \brief Typed variant of request_active_trades_snapshot.
        void request_active_trades_snapshot_result(
            std::function<void(ActiveTradesSnapshotResult)> callback);

        /// \brief Sends login credentials for authentication.
        /// \param req_id The request ID obtained from the main page.
        /// \param req_value The request value obtained from the main page.
        /// \param cookies Cookies extracted from the main page.
        /// \param auth_data Shared pointer to authentication data.
        /// \param result_callback Callback function to receive login result.
        void request_login(
            const std::string& req_id,
            const std::string& req_value,
            const std::string& cookies,
            std::shared_ptr<AuthData> auth_data,
            std::function<void(
                bool success,
                const std::string& user_id,
                const std::string& user_hash,
                const std::string& cookies,
                const std::string& reason)> result_callback);

        /// \brief Typed variant of request_login.
        void request_login_result(
            const std::string& req_id,
            const std::string& req_value,
            const std::string& cookies,
            std::shared_ptr<AuthData> auth_data,
            std::function<void(LoginCredentialsResult)> result_callback);

        /// \brief Performs authentication using user credentials.
        /// \param user_id The user ID.
        /// \param user_hash The authentication hash.
        /// \param cookies Cookies extracted from previous authentication steps.
        /// \param auth_data Shared pointer to authentication data.
        /// \param result_callback Callback function to receive authentication result.
        void request_auth(
            const std::string& user_id,
            const std::string& user_hash,
            const std::string& cookies,
            std::shared_ptr<AuthData> auth_data,
            std::function<void(
                bool success,
                const std::string& reason)> result_callback);

        /// \brief Typed variant of request_auth.
        void request_auth_result(
            const std::string& user_id,
            const std::string& user_hash,
            const std::string& cookies,
            std::shared_ptr<AuthData> auth_data,
            std::function<void(AuthCheckResult)> result_callback);

        /// \brief Requests the user's account balance.
        /// \param balance_callback Callback function to receive the balance data.
        void request_balance(
            std::function<void(
                bool success,
                double balance,
                CurrencyType currency)> balance_callback);

        /// \brief Typed variant of request_balance.
        void request_balance_result(
            std::function<void(BalanceInfoResult)> balance_callback);

        /// \brief Switches between real and demo account types.
        /// \param switch_callback Callback function to receive switch result.
        void request_switch_account_type(
            std::function<void(bool success)> switch_callback);

        /// \brief Typed variant of request_switch_account_type.
        void request_switch_account_type_result(
            std::function<void(SettingsSwitchResult)> switch_callback);

        /// \brief Switches the account's currency.
        /// \param switch_callback Callback function to receive switch result.
        void request_switch_currency(
            std::function<void(bool success)> switch_callback);

        /// \brief Typed variant of request_switch_currency.
        void request_switch_currency_result(
            std::function<void(SettingsSwitchResult)> switch_callback);

        /// \brief Requests the latest prices through the legacy per-tick DTO API.
        /// \param price_callback Callback function to receive `SingleTick` values.
        /// \note New internal consumers should use `request_price_batches()`.
        void request_price(
            std::function<void(
                bool success,
                std::vector<SingleTick> ticks)> price_callback);

        /// \brief Requests the latest prices grouped by source metadata.
        /// \param price_callback Callback function to receive source tick batches.
        void request_price_batches(
            std::function<void(
                bool success,
                std::vector<events::TickUpdateBatch> batches)> price_callback);

        /// \brief Typed variant of request_price.
        void request_price_result(
            std::function<void(PriceSnapshotResult)> price_callback);

        /// \brief Requests historical bars from the broker market-data backend.
        /// \param request Symbol, timeframe, range, and preferred price source.
        /// \param callback Callback receiving a typed bar history result.
        void request_bar_history_result(
            const BarHistoryRequest& request,
            std::function<void(BarHistoryApiResult)> callback);

        /// \brief Requests closed trade history export.
        /// \param request History range and timestamp field.
        /// \param account_type Current broker account type selected during authentication.
        /// \param callback Callback receiving parsed closed trades.
        void request_trade_history(
            const TradeHistoryRequest& request,
            AccountType account_type,
            std::function<void(
                bool success,
                long status_code,
                std::vector<TradeRecord> records)> callback);

        /// \brief Typed variant of request_trade_history.
        void request_trade_history_result(
            const TradeHistoryRequest& request,
            AccountType account_type,
            std::function<void(TradeHistoryApiResult)> callback);
        /// \brief Requests the trade check result.
        /// \param deal_id The deal ID to check.
        /// \param retry_attempts Number of retries if the response is empty.
        /// \param callback_check Callback function to receive trade status.
        void request_trade_check(
            int64_t deal_id,
            int retry_attempts,
            std::function<void(
                bool success,
                long status_code,
                double price,
                double profit)> callback_check);

        /// \brief Typed variant of request_trade_check.
        void request_trade_check_result(
            int64_t deal_id,
            int retry_attempts,
            std::function<void(TradeCheckResult)> callback_check);

        /// \brief Sends a trade execution request.
        /// \param request Shared pointer to the trade request.
        /// \param result Shared pointer to the trade result.
        /// \param request_callback Callback function to receive execution result.
        void request_execute_trade(
            std::shared_ptr<TradeRequest> request,
            std::function<void(
                bool success,
                long status_code,
                int64_t option_id,
                int64_t open_date,
                double open_price,
                const std::string& error_desc)> result_callback);

        /// \brief Typed variant of request_execute_trade.
        void request_execute_trade_result(
            std::shared_ptr<TradeRequest> request,
            std::function<void(TradeOpenResult)> result_callback);

    private:
        HttpClientComponent& m_client;      ///< Reference to the HTTP client component.
        kurlyk::HttpClient m_binance_client; ///< Separate backend for BTCUSDT history.
        kurlyk::Headers   m_api_headers; ///< Default API headers.
        std::string       m_user_id;     ///< User ID for authentication.
        std::string       m_user_hash;   ///< User authentication hash.
        std::string       m_cookies;     ///< Session cookies.
        int m_domain_index_min = 0;      ///< Minimum domain index to scan (0 = intrade.bar).
        int m_domain_index_max = 0;      ///< Maximum domain index to scan (e.g., intrade1000.bar).
        bool m_domain_include_primary = true; ///< Whether to include https://intrade.bar in domain discovery.
        TradeHistorySource m_trade_history_source = TradeHistorySource::CSV; ///< Closed trade history source mode.

        /// \brief Returns a reference to the HTTP client.
        /// \return Reference to the `kurlyk::HttpClient` instance.
        kurlyk::HttpClient& get_http_client() {
            return m_client.get_http_client();
        }

        /// \brief Gets the rate limit for the specified type.
        /// \param rate_limit_id The type of rate limit to retrieve.
        /// \return The rate limit value for the specified type.
        template<class T>
        uint32_t get_rate_limit(T rate_limit_id) const {
            return m_client.get_rate_limit<T>(rate_limit_id);
        }

        /// \brief Adds a new HTTP request task to the list.
        /// \param future The future object representing the pending HTTP response.
        /// \param callback The callback function to handle the response.
        void add_http_request_task(
                std::future<kurlyk::HttpResponsePtr> future,
                std::function<void(kurlyk::HttpResponsePtr)> callback) {
            m_client.add_http_request_task(std::move(future), std::move(callback));
        }

        /// \brief Handles authentication event updates.
        /// \param event The received authentication data event.
        void handle_event(const events::AuthDataEvent& event) {
            if (auto msg = std::dynamic_pointer_cast<AuthData>(event.auth_data)) {
                LOGIT_TRACE0();
                const std::string referer(msg->host + "/");
                auto& client = get_http_client();
                if (!msg->auto_find_domain) {
                    client.set_host(msg->host);
                    client.set_origin(msg->host);
                } else {
                    m_domain_include_primary = msg->domain_index_min >= 0;
                    m_domain_index_min = std::abs(msg->domain_index_min);
                    m_domain_index_max = std::abs(msg->domain_index_max);
                    if (m_domain_index_min > m_domain_index_max) {
                        std::swap(m_domain_index_min, m_domain_index_max);
                    }
                }
                client.set_user_agent(msg->user_agent);
                client.set_accept_language(msg->accept_language);
                client.set_proxy_server(msg->proxy_server);
                client.set_proxy_auth(msg->proxy_auth);
                client.set_proxy_type(msg->proxy_type);
                m_binance_client.set_user_agent(msg->user_agent);
                m_binance_client.set_accept_language(msg->accept_language);
                m_binance_client.set_proxy_server(msg->proxy_server);
                m_binance_client.set_proxy_auth(msg->proxy_auth);
                m_binance_client.set_proxy_type(msg->proxy_type);
                m_trade_history_source = msg->trade_history_source;
                LOGIT_INFO(
                    "Intrade Bar HTTP client configured. host=",
                    referer,
                    ", proxy_enabled=",
                    (!msg->proxy_server.empty()));
                client.set_referer(referer);
                client.set_retry_attempts(10, time_shield::MS_PER_SEC);
                client.set_timeout(30);
                client.set_connect_timeout(15);
                m_binance_client.set_retry_attempts(10, time_shield::MS_PER_SEC);
                m_binance_client.set_timeout(30);
                m_binance_client.set_connect_timeout(15);
            }
        }

        void start_authentication(
            std::shared_ptr<AuthData> auth_data,
            connection_callback_t connect_callback);

        void handle_account_type_switch(
            std::shared_ptr<AuthData> auth_data,
            AccountType account_type,
            CurrencyType currency,
            connection_callback_t connect_callback);

        void handle_currency_switch(
            std::shared_ptr<AuthData> auth_data,
            CurrencyType currency,
            connection_callback_t connect_callback);

        /// \brief Sends account settings switch request and preserves failure diagnostics.
        void request_settings_switch_result(
            const std::string& operation_name,
            const std::string& endpoint,
            std::function<void(SettingsSwitchResult)> switch_callback);

        void finalize_authentication(
            std::shared_ptr<AuthData> auth_data,
            connection_callback_t connect_callback);

        void request_trade_history_csv(
            const TradeHistoryRequest& request,
            AccountType account_type,
            std::function<void(TradeHistoryApiResult)> callback);

        void request_trade_history_html(
            const TradeHistoryRequest& request,
            AccountType account_type,
            std::function<void(TradeHistoryApiResult)> callback);
    };
	
    // ------------------------------------------------------------------------

    inline void RequestManager::request_main_page(
            std::shared_ptr<AuthData> auth_data,
            std::function<void(
                bool success,
                const std::string& req_id,
                const std::string& req_value,
                const std::string& cookies,
                const std::string& reason)> result_callback) {
        LOGIT_TRACE0();

        // Validate email and password
        if (auth_data->email.empty() ||
            auth_data->password.empty()) {
            LOGIT_ERROR_IF(auth_data->email.empty(), "Email is missing.");
            LOGIT_ERROR_IF(auth_data->password.empty(), "Password is missing.");
            result_callback(false, std::string(), std::string(), std::string(), "Email or password is missing.");
            return;
        }

        // Prepare the HTTP GET request
        auto future = get_http_client().get(
            "/",
            kurlyk::QueryParams(),
            {
                {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9"},
                {"Content-Type", "application/x-www-form-urlencoded"},
                {"Upgrade-Insecure-Requests", "1"}
            },
            get_rate_limit(RateLimitType::ACCOUNT_INFO)
        );

        // Handle the HTTP response
        auto callback = [this, auth_data, result_callback](
                kurlyk::HttpResponsePtr response) {
            // Validate the response
            if (!validate_response(response, [&result_callback](const std::string& error_text){
                    result_callback(false, std::string(), std::string(), std::string(), error_text);
                })) {
                return;
            }

            // Parse the response content
            auto parsed_data = parse_main_page_response(response->content, response->headers);
            if (!parsed_data) {
#               ifdef OPTIONX_LOG_UNIQUE_FILE_INDEX
                const int log_index = OPTIONX_LOG_UNIQUE_FILE_INDEX;
                LOGIT_STREAM_ERROR_TO(log_index) << response->content;
                LOGIT_PRINT_ERROR(
                    "Failed to process server response; content log was written to file: ",
                    LOGIT_GET_LAST_FILE_NAME(log_index)
                );
#               else
                LOGIT_ERROR(
                     "Failed to process server response.",
                    (response ? response->status_code : -1)
                );
#               endif
                result_callback(false, std::string(), std::string(), std::string(), "Failed to process server response.");
                return;
            }
            // Extract parsed data
            const auto& [req_id, req_value, cookies] = *parsed_data;

            result_callback(true, req_id, req_value, cookies, "");
        };

        // Add the HTTP request task for asynchronous processing
        add_http_request_task(std::move(future), std::move(callback));
    }

    inline void RequestManager::request_active_trades_snapshot(
            std::function<void(
                bool success,
                std::vector<ActiveTradeInfo> trades)> callback) {
        LOGIT_TRACE0();

        auto future = get_http_client().get(
            "/",
            kurlyk::QueryParams(),
            {
                {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0"},
                {"Content-Type", "application/x-www-form-urlencoded"},
                {"Upgrade-Insecure-Requests", "1"},
                {"Cookie", m_cookies}
            },
            get_rate_limit(RateLimitType::ACCOUNT_INFO)
        );

        auto response_callback = [callback](kurlyk::HttpResponsePtr response) {
            if (!validate_response(response)) {
                callback(false, {});
                return;
            }

            try {
                callback(true, parse_active_trades_snapshot(response->content));
            } catch (const std::exception& ex) {
                LOGIT_ERROR("Failed to parse active trades snapshot: ", ex.what());
                callback(false, {});
            }
        };

        add_http_request_task(std::move(future), std::move(response_callback));
    }

    inline void RequestManager::request_login(
            const std::string& req_id,
            const std::string& req_value,
            const std::string& cookies,
            std::shared_ptr<AuthData> auth_data,
            std::function<void(
                bool success,
                const std::string& user_id,
                const std::string& user_hash,
                const std::string& cookies,
                const std::string& reason)> result_callback) {
        LOGIT_DEBUG(req_id, req_value, utils::redact_secret_value(cookies));

        // Prepare query parameters
        kurlyk::QueryParams query = {
            {"email", auth_data->email},
            {"password", auth_data->password},
        };
        if (!req_id.empty() && !req_value.empty()) {
            query.emplace(req_id, req_value);
        }

        // Prepare HTTP GET request
        auto future = get_http_client().post(
            "/login",
            kurlyk::QueryParams(),
            {
                {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9"},
                {"Content-Type", "application/x-www-form-urlencoded"},
                {"Upgrade-Insecure-Requests", "1"},
                {"Connection", "keep-alive"},
                {"Cookie", cookies}
            },
            kurlyk::utils::to_query_string(query),
            get_rate_limit(RateLimitType::ACCOUNT_INFO)
        );

        // Define callback for handling HTTP response
        auto callback = [this, cookies, auth_data, result_callback](
                kurlyk::HttpResponsePtr response) {
            if (!validate_response(response, [&result_callback](const std::string& error_text){
                    result_callback(false, std::string(), std::string(), std::string(), error_text);
                })) {
                return;
            }

            auto login_result = parse_login(response->content);
            if (!login_result) {
#               ifdef OPTIONX_LOG_UNIQUE_FILE_INDEX
                const int log_index = OPTIONX_LOG_UNIQUE_FILE_INDEX;
                LOGIT_STREAM_ERROR_TO(log_index) << response->content;
                LOGIT_PRINT_ERROR("Failed to parse login; content log was written to file: ", LOGIT_GET_LAST_FILE_NAME(log_index));
#               else
                LOGIT_PRINT_ERROR("Failed to parse login.");
#               endif
                result_callback(false, std::string(), std::string(), std::string(), "Failed to parse login.");
                return;
            }

            const auto [user_id, user_hash] = *login_result;
            result_callback(true, user_id, user_hash, cookies, std::string());
        };

        // Add the task to handle the HTTP request
        add_http_request_task(std::move(future), std::move(callback));
    }

    inline void RequestManager::request_auth(
            const std::string& user_id,
            const std::string& user_hash,
            const std::string& cookies,
            std::shared_ptr<AuthData> auth_data,
            std::function<void(
                bool success,
                const std::string& reason)> result_callback) {
        LOGIT_TRACE0();

        // Prepare query parameters
        kurlyk::QueryParams query = {
            {"id", user_id},
            {"hash", user_hash},
        };

        // Send POST request
        auto future = get_http_client().post(
            "/auth",
            query,
            {
                {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9"},
                {"Content-Type", "application/x-www-form-urlencoded"},
                {"Upgrade-Insecure-Requests", "1"},
                {"Cookie", cookies}
            },
            std::string(),
            get_rate_limit(RateLimitType::ACCOUNT_INFO)
        );

        // Define callback for handling HTTP response
        auto callback = [this, result_callback](
                kurlyk::HttpResponsePtr response) {
            if (!validate_response(response, [&result_callback](const std::string& error_text){
                    result_callback(false, error_text);
                })) {
                return;
            }

            result_callback(true, std::string());
        };

        add_http_request_task(std::move(future), std::move(callback));
    }

    inline void RequestManager::request_balance(
            std::function<void(
                bool success,
                double balance,
                CurrencyType currency)> balance_callback) {
        LOGIT_TRACE0();

        // Prepare query parameters
        kurlyk::QueryParams query = {
            {"user_id", m_user_id},
            {"user_hash", m_user_hash},
        };

        // Send POST request
        auto future = get_http_client().post(
            "/balance.php",
            kurlyk::QueryParams(),
            m_api_headers,
            kurlyk::utils::to_query_string(query),
            get_rate_limit(RateLimitType::BALANCE)
        );

        auto callback = [this, balance_callback](kurlyk::HttpResponsePtr response) {
            if (!validate_response(response)) {
                balance_callback(false, 0.0, CurrencyType::UNKNOWN);
                return;
            }

            auto balance_result = parse_balance(response->content);
            if (!balance_result) {
                LOGIT_ERROR("Failed to parse balance.");
                balance_callback(false, 0.0, CurrencyType::UNKNOWN);
                return;
            }

            const auto [balance, currency] = *balance_result;
            balance_callback(true, balance, currency);
        };

        add_http_request_task(std::move(future), std::move(callback));
    }

    inline void RequestManager::request_switch_account_type(
            std::function<void(bool success)> switch_callback) {
        request_switch_account_type_result(
            [switch_callback = std::move(switch_callback)](SettingsSwitchResult result) {
                if (switch_callback) switch_callback(static_cast<bool>(result));
            });
    }

    inline void RequestManager::request_switch_currency(
            std::function<void(bool success)> switch_callback) {
        request_switch_currency_result(
            [switch_callback = std::move(switch_callback)](SettingsSwitchResult result) {
                if (switch_callback) switch_callback(static_cast<bool>(result));
            });
    }

    inline void RequestManager::request_settings_switch_result(
            const std::string& operation_name,
            const std::string& endpoint,
            std::function<void(SettingsSwitchResult)> switch_callback) {
        LOGIT_TRACE0();
        if (!switch_callback) return;
    
        // Prepare query parameters
        kurlyk::QueryParams query = {
            {"user_id", m_user_id},
            {"user_hash", m_user_hash},
        };

        // Send POST request
        auto future = get_http_client().post(
            endpoint,
            kurlyk::QueryParams(),
            m_api_headers,
            kurlyk::utils::to_query_string(query),
            get_rate_limit(RateLimitType::ACCOUNT_SETTINGS)
        );

        auto callback = [operation_name, switch_callback = std::move(switch_callback)](
                kurlyk::HttpResponsePtr response) mutable {
            if (!validate_response(response)) {
                switch_callback(make_settings_switch_failure(
                    SettingsSwitchFailureReason::TRANSPORT_ERROR,
                    ::optionx::utils::describe_response_error(
                        response,
                        "Settings switch request failed."),
                    response ? response->status_code : -1));
                return;
            }

            auto result = parse_settings_switch_response(
                response->content,
                response->status_code,
                operation_name);
            if (!result && result.value.failure_reason == SettingsSwitchFailureReason::UNEXPECTED_RESPONSE) {
                LOGIT_PRINT_ERROR(
                    "Response validation failed: expected 'ok' or 'error', but received '",
                    response->content,
                    "'.");
            }
            switch_callback(std::move(result));
        };

        add_http_request_task(std::move(future), std::move(callback));
    }
    
    inline void RequestManager::request_find_working_domain(
            std::function<void(
                bool success,
                std::string& host)> find_callback) {
        LOGIT_TRACE0();
        
        const int min_index = m_domain_index_min;
        const int max_index = m_domain_index_max;
        const bool include_primary = m_domain_include_primary;

        struct DomainCheckState {
            std::vector<int> indices;
            std::size_t next_index = 0;
            int pending_requests = 0;
            bool completed = false;
            std::function<void(bool, std::string&)> on_complete;
        };

        auto state = std::make_shared<DomainCheckState>();
        state->on_complete = std::move(find_callback);
        if (include_primary) {
            state->indices.push_back(0);
        }
        for (int i = std::max(1, min_index); i <= max_index; ++i) {
            state->indices.push_back(i);
        }

        constexpr int domain_check_batch_size = 50;

        auto make_host = [](int index) {
            return (index == 0)
                ? "https://intrade.bar"
                : "https://intrade" + std::to_string(index) + ".bar";
        };

        auto restore_client_defaults = [this]() {
            auto& client = get_http_client();
            client.set_head_only(false);
            client.set_retry_attempts(10, time_shield::MS_PER_SEC);
            client.set_timeout(30);
            client.set_connect_timeout(15);
        };

        auto complete = [this, state, make_host, restore_client_defaults](
                bool success,
                int selected_index) {
            if (state->completed) return;
            state->completed = true;

            std::string selected_host;
            if (success) {
                selected_host = make_host(selected_index);
                get_http_client().set_host(selected_host);
                get_http_client().set_origin(selected_host);
            }

            restore_client_defaults();
            LOGIT_PRINT_INFO("Auto-selected domain:", selected_host, "; success:", success);
            state->on_complete(success, selected_host);
        };

        auto launch_batch = std::make_shared<std::function<void()>>();
        *launch_batch = [this, state, make_host, complete, launch_batch]() {
            if (state->completed) return;
            if (state->next_index >= state->indices.size()) {
                complete(false, 0);
                return;
            }

            auto& client = get_http_client();
            client.set_head_only(true);
            client.set_retry_attempts(3, time_shield::MS_PER_SEC);
            client.set_timeout(5);
            client.set_connect_timeout(5);

            state->pending_requests = 0;
            for (int sent = 0;
                 sent < domain_check_batch_size && state->next_index < state->indices.size();
                 ++sent, ++state->next_index) {
                const int index = state->indices[state->next_index];
                const std::string host = make_host(index);

                client.set_host(host);
                auto future = client.get("/", {}, {});

                auto callback = [state, complete, launch_batch, index](
                        kurlyk::HttpResponsePtr response) {
                    if (state->completed) return;

                    if (response && response->ready && response->status_code == 200) {
                        complete(true, index);
                        return;
                    }

                    if (!response || !response->ready) {
                        LOGIT_ERROR("Domain check: response not ready or null.");
                    }

                    --state->pending_requests;
                    if (state->pending_requests == 0) {
                        (*launch_batch)();
                    }
                };

                ++state->pending_requests;
                add_http_request_task(std::move(future), std::move(callback));
            }

            if (state->pending_requests == 0) {
                (*launch_batch)();
            }
        };

        if (state->indices.empty()) {
            complete(false, 0);
            return;
        }

        (*launch_batch)();
    }
    
    /// \brief Checks if the currently set host in the HTTP client is available.
    /// \param callback Callback that receives the result: true if available, false otherwise.
    inline void RequestManager::request_check_current_host_available(
            std::function<void(bool)> check_callback) {
        LOGIT_TRACE0();

        auto& client = get_http_client();
        client.set_head_only(true); // Use HEAD to avoid downloading body
        client.set_retry_attempts(3, time_shield::MS_PER_SEC);
        client.set_timeout(5);
        client.set_connect_timeout(5);

        auto future = client.get("/", {}, {});
        auto cb = [check_callback = std::move(check_callback)](kurlyk::HttpResponsePtr response) {
            if (!response || !response->ready) {
                LOGIT_ERROR("Host availability check: response not ready or null.");
                check_callback(false);
                return;
            }
            bool success = response->status_code == 200;
            LOGIT_PRINT_DEBUG("Current host ping check:",
                " success:", success,
                "; status:", response->status_code);
            check_callback(success);
        };

        add_http_request_task(std::move(future), std::move(cb));
        client.set_head_only(false); // Restore to default after request
        client.set_retry_attempts(10, time_shield::MS_PER_SEC);
        client.set_timeout(30);
        client.set_connect_timeout(15);
    }

    inline void RequestManager::request_profile(
            std::function<void(
                bool success,
                CurrencyType currency,
                AccountType account)> profile_callback) {
        LOGIT_TRACE0();

        // Send POST request
        auto future = get_http_client().get(
            "/profile",
            kurlyk::QueryParams(),
            {
                {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0"},
                {"Upgrade-Insecure-Requests", "1"},
                {"Cookie", m_cookies}
            },
            get_rate_limit(RateLimitType::ACCOUNT_INFO)
        );

        auto callback = [this, profile_callback](kurlyk::HttpResponsePtr response) {
            if (!validate_response(response)) {
                profile_callback(false, CurrencyType::UNKNOWN, AccountType::UNKNOWN);
                return;
            }

            auto [currency, account] = parse_profile_response(response->content);
            if (currency == CurrencyType::UNKNOWN || account == AccountType::UNKNOWN) {
                //LOGIT_PRINT_ERROR("Response validation failed: expected 'ok', but received '", response->content, "'.");
#               ifdef OPTIONX_LOG_UNIQUE_FILE_INDEX
                const int log_index = OPTIONX_LOG_UNIQUE_FILE_INDEX;
                LOGIT_STREAM_ERROR_TO(log_index) << response->content;
                LOGIT_PRINT_ERROR(
                    "Response validation failed: currency=",currency,";account=",account,"; Content log was written to file: ",
                    LOGIT_GET_LAST_FILE_NAME(log_index)
                );
#               else
                LOGIT_PRINT_ERROR("Response validation failed: currency=", currency, ";account=", account);
#               endif
                profile_callback(false, currency, account);
                return;
            }

            LOGIT_TRACE(currency, account);
            profile_callback(true, currency, account);
        };

        add_http_request_task(std::move(future), std::move(callback));
    }

    inline void RequestManager::request_price(
            std::function<void(
                bool success,
                std::vector<SingleTick> ticks)> price_callback) {
        request_price_batches(
            [price_callback = std::move(price_callback)](
                    bool success,
                    std::vector<events::TickUpdateBatch> batches) mutable {
                if (!success) {
                    price_callback(false, {});
                    return;
                }

                std::size_t tick_count = 0;
                for (const auto& batch : batches) {
                    tick_count += batch.items.size();
                }

                std::vector<SingleTick> ticks;
                ticks.reserve(tick_count);
                for (auto& batch : batches) {
                    for (auto& tick : batch.items) {
                        ticks.emplace_back(
                            std::move(tick),
                            batch.symbol,
                            batch.provider,
                            batch.price_digits,
                            batch.volume_digits);
                    }
                }
                price_callback(true, std::move(ticks));
            });
    }

    inline void RequestManager::request_price_batches(
            std::function<void(
                bool success,
                std::vector<events::TickUpdateBatch> batches)> price_callback) {
        // Отправка GET-запроса
        auto future = get_http_client().get(
            "/price_now",
            kurlyk::QueryParams(),
            {{"Accept", "*/*"}, {"Cookie", m_cookies}},
            get_rate_limit(RateLimitType::TICK_DATA)
        );

        // Коллбэк для обработки ответа
        auto callback = [this, price_callback](kurlyk::HttpResponsePtr response) {
            if (!validate_response(response)) {
                price_callback(false, {});
                return;
            }

            try {
                auto batches = parse_price_snapshot_response(
                    response->content,
                    static_cast<std::uint64_t>(OPTIONX_TIMESTAMP_MS));
                price_callback(true, std::move(batches));
            } catch (const std::exception& ex) {
                LOGIT_ERROR("Error parsing price response: ", ex.what());
                price_callback(false, {});
            }
        };

        // Добавление задачи HTTP-запроса
        add_http_request_task(std::move(future), std::move(callback));
    }

    namespace {
        int64_t select_trade_history_timestamp(
                const TradeRecord& record,
                TradeRecordTimeField field) {
            switch (field) {
            case TradeRecordTimeField::PLACE_DATE:
                return record.place_date;
            case TradeRecordTimeField::SEND_DATE:
                return record.send_date;
            case TradeRecordTimeField::OPEN_DATE:
                return record.open_date;
            case TradeRecordTimeField::CLOSE_DATE:
                return record.close_date;
            case TradeRecordTimeField::AUTO:
            default:
                if (record.place_date > 0) return record.place_date;
                if (record.send_date > 0) return record.send_date;
                if (record.open_date > 0) return record.open_date;
                if (record.close_date > 0) return record.close_date;
                return 0;
        }
    }

        std::vector<TradeRecord> filter_trade_history_range(
                std::vector<TradeRecord> records,
                const TradeHistoryRequest& request) {
            if (request.range_mode == TimeRangeMode::NONE) return records;

            records.erase(
                std::remove_if(
                    records.begin(),
                    records.end(),
                    [&request](const TradeRecord& record) {
                        const int64_t timestamp =
                            select_trade_history_timestamp(record, request.time_field);
                        if (timestamp <= 0) return true;
                        if (request.range_mode == TimeRangeMode::HALF_OPEN) {
                            return timestamp < request.start_ms ||
                                timestamp >= request.stop_ms;
                        }
                        return timestamp < request.start_ms ||
                            timestamp > request.stop_ms;
                    }),
                records.end());
            return records;
        }

        void append_unique_trade_history(
                std::vector<TradeRecord>& target,
                const std::vector<TradeRecord>& source) {
            for (const auto& record : source) {
                const bool duplicate = record.option_id > 0 &&
                    std::any_of(target.begin(), target.end(), [&record](const TradeRecord& existing) {
                        return existing.option_id == record.option_id;
                    });
                if (!duplicate) target.push_back(record);
            }
        }

        bool trade_history_page_reached_start(
                const std::vector<TradeRecord>& records,
                const TradeHistoryRequest& request) {
            if (request.range_mode == TimeRangeMode::NONE) return false;

            for (const auto& record : records) {
                const int64_t timestamp =
                    select_trade_history_timestamp(record, request.time_field);
                if (timestamp > 0 && timestamp < request.start_ms) {
                    return true;
                }
            }
            return false;
        }

        std::size_t count_substrings(
                const std::string& text,
                const std::string& needle) {
            if (needle.empty()) return 0;
            std::size_t count = 0;
            std::size_t pos = 0;
            while ((pos = text.find(needle, pos)) != std::string::npos) {
                ++count;
                pos += needle.size();
            }
            return count;
        }

        void apply_trade_history_request_metadata(
                std::vector<TradeRecord>& records,
                const TradeHistoryRequest& request) {
            if (request.comment.empty()) return;
            for (auto& record : records) {
                if (record.comment.empty()) record.comment = request.comment;
            }
        }

        long select_combined_trade_history_status(
                bool csv_success,
                long csv_status_code,
                bool html_success,
                long html_status_code) {
            if (!csv_success && csv_status_code >= 0) return csv_status_code;
            if (!html_success && html_status_code >= 0) return html_status_code;
            if (!csv_success) return csv_status_code;
            if (!html_success) return html_status_code;
            return csv_status_code >= 0 ? csv_status_code : html_status_code;
        }

        std::string trade_history_error_message(
                const char* source,
                const TradeHistoryApiResult& result) {
            std::string message = "Failed to retrieve ";
            message += source;
            message += " trade history.";
            if (!result.error_message.empty()) {
                message += " ";
                message += result.error_message;
            }
            return message;
        }

        std::string combined_trade_history_error_message(
                const TradeHistoryApiResult& csv_result,
                const TradeHistoryApiResult& html_result) {
            if (!csv_result && !html_result) {
                return trade_history_error_message("CSV", csv_result) +
                    " " +
                    trade_history_error_message("HTML", html_result);
            }
            if (!csv_result) {
                return trade_history_error_message("CSV", csv_result);
            }
            if (!html_result) {
                return trade_history_error_message("HTML", html_result);
            }
            return {};
        }
    }

    inline void RequestManager::request_trade_history(
            const TradeHistoryRequest& request,
            AccountType account_type,
            std::function<void(
                bool success,
                long status_code,
                std::vector<TradeRecord> records)> callback) {
        request_trade_history_result(
            request,
            account_type,
            [callback = std::move(callback)](TradeHistoryApiResult result) mutable {
                if (!callback) return;
                callback(
                    result.success,
                    result.status_code,
                    result.success ? std::move(result.value.records) : std::vector<TradeRecord>());
            });
    }

    inline void RequestManager::request_trade_history_result(
            const TradeHistoryRequest& request,
            AccountType account_type,
            std::function<void(TradeHistoryApiResult)> callback) {
        if (!request.has_valid_range() || account_type == AccountType::UNKNOWN) {
            if (callback) {
                callback(TradeHistoryApiResult::fail(
                    "Invalid trade history request range or account type.",
                    TradeHistoryApiResult::NO_RESPONSE_STATUS));
            }
            return;
        }

        LOGIT_INFO(
            "Intrade Bar trade history request. source=",
            trade_history_source_to_string(m_trade_history_source),
            ", account=",
            to_str(account_type));

        if (m_trade_history_source == TradeHistorySource::HTML) {
            request_trade_history_html(request, account_type, std::move(callback));
            return;
        }
        if (m_trade_history_source == TradeHistorySource::CSV) {
            request_trade_history_csv(request, account_type, std::move(callback));
            return;
        }

        request_trade_history_csv(
            request,
            account_type,
            [this, request, account_type, callback = std::move(callback)](
                    TradeHistoryApiResult csv_result) mutable {
                request_trade_history_html(
                    request,
                    account_type,
                    [callback = std::move(callback),
                     csv_result = std::move(csv_result)](
                            TradeHistoryApiResult html_result) mutable {
                        if (!callback) return;
                        if (csv_result && html_result) {
                            callback(
                                TradeHistoryApiResult::ok(
                                    TradeHistory{
                                        merge_trade_history_csv_with_html(
                                            std::move(csv_result.value.records),
                                            html_result.value.records)},
                                    select_combined_trade_history_status(
                                        csv_result.success,
                                        csv_result.status_code,
                                        html_result.success,
                                        html_result.status_code)));
                            return;
                        }
                        callback(
                            TradeHistoryApiResult::fail(
                                combined_trade_history_error_message(
                                    csv_result,
                                    html_result),
                            select_combined_trade_history_status(
                                csv_result.success,
                                csv_result.status_code,
                                html_result.success,
                                html_result.status_code)));
                    });
            });
    }

    inline void RequestManager::request_trade_history_csv(
            const TradeHistoryRequest& request,
            AccountType account_type,
            std::function<void(TradeHistoryApiResult)> callback) {
        constexpr int64_t broker_offset_sec = 3 * time_shield::SEC_PER_HOUR;
        // The broker CSV export endpoint requires finite dates; 2000-01-01 is
        // used as a practical lower bound for "all available" history.
        const int64_t start_ms = request.range_mode == TimeRangeMode::NONE ?
            time_shield::sec_to_ms(time_shield::to_timestamp(2000, 1, 1)) :
            request.start_ms;
        const int64_t stop_ms = request.range_mode == TimeRangeMode::NONE ?
            OPTIONX_TIMESTAMP_MS :
            request.stop_ms;
        kurlyk::QueryParams query = {
            {"name_method", "stat_export"},
            {"status_real", account_type == AccountType::REAL ? "1" : "0"},
            {"date1", time_shield::to_string(
                "%DD.%MM.%YYYY",
                time_shield::ms_to_sec(start_ms) + broker_offset_sec)},
            {"date2", time_shield::to_string(
                "%DD.%MM.%YYYY",
                time_shield::ms_to_sec(stop_ms) + broker_offset_sec)}
        };

        auto future = get_http_client().post(
            "/stat_trade_export.php",
            kurlyk::QueryParams(),
            {
                {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                {"Content-Type", "application/x-www-form-urlencoded"},
                {"Upgrade-Insecure-Requests", "1"},
                {"Cookie", m_cookies}
            },
            kurlyk::utils::to_query_string(query),
            get_rate_limit(RateLimitType::ACCOUNT_INFO)
        );

        auto response_callback = [request, account_type, callback = std::move(callback)](
                kurlyk::HttpResponsePtr response) {
            if (!validate_response(response)) {
                if (callback) {
                    callback(TradeHistoryApiResult::fail(
                        "CSV trade history HTTP response failed validation.",
                        response ? response->status_code : TradeHistoryApiResult::NO_RESPONSE_STATUS));
                }
                return;
            }

            try {
                auto records = filter_trade_history_range(
                    parse_trade_history_csv_export(response->content, account_type),
                    request);
                apply_trade_history_request_metadata(records, request);
                if (callback) {
                    callback(TradeHistoryApiResult::ok(
                        TradeHistory{std::move(records)},
                        response->status_code));
                }
            } catch (const std::exception& ex) {
                LOGIT_ERROR("Error parsing trade history CSV export: ", ex.what());
                if (callback) {
                    callback(TradeHistoryApiResult::fail(
                        std::string("Failed to parse CSV trade history: ") + ex.what(),
                        response ? response->status_code : TradeHistoryApiResult::NO_RESPONSE_STATUS));
                }
            }
        };

        add_http_request_task(std::move(future), std::move(response_callback));
    }

    inline void RequestManager::request_trade_history_html(
            const TradeHistoryRequest& request,
            AccountType account_type,
            std::function<void(TradeHistoryApiResult)> callback) {
        struct HtmlHistoryState {
            TradeHistoryRequest request;
            AccountType account_type = AccountType::UNKNOWN;
            std::vector<TradeRecord> records;
            std::vector<std::string> requested_last_values;
            std::function<void(TradeHistoryApiResult)> callback;
            int page_count = 0;
            bool completed = false;
        };

        auto state = std::make_shared<HtmlHistoryState>();
        state->request = request;
        state->account_type = account_type;
        state->callback = std::move(callback);

        constexpr int max_html_history_pages = 200;
        auto complete = std::make_shared<std::function<void(TradeHistoryApiResult)>>();
        *complete = [state](TradeHistoryApiResult result) {
            if (state->completed) return;
            state->completed = true;

            if (!result.success) {
                if (state->callback) state->callback(std::move(result));
                return;
            }

            auto records = filter_trade_history_range(
                std::move(state->records),
                state->request);
            apply_trade_history_request_metadata(records, state->request);
            if (state->callback) {
                state->callback(TradeHistoryApiResult::ok(
                    TradeHistory{std::move(records)},
                    result.status_code));
            }
        };

        auto load_more = std::make_shared<std::function<void(std::string)>>();
        auto process_page = std::make_shared<std::function<void(TradeHistoryHtmlPage, long)>>();

        *process_page = [state, complete, load_more](
                TradeHistoryHtmlPage page,
                long status_code) {
            ++state->page_count;
            const bool page_has_records = !page.records.empty();
            const bool reached_start = trade_history_page_reached_start(
                page.records,
                state->request);

            append_unique_trade_history(state->records, page.records);

            const bool next_repeats =
                !page.next_last.empty() &&
                std::find(
                    state->requested_last_values.begin(),
                    state->requested_last_values.end(),
                    page.next_last) != state->requested_last_values.end();

            if (page_has_records &&
                !reached_start &&
                !page.next_last.empty() &&
                !next_repeats &&
                state->page_count < max_html_history_pages) {
                (*load_more)(page.next_last);
                return;
            }

            (*complete)(TradeHistoryApiResult::ok(TradeHistory{}, status_code));
        };

        const std::weak_ptr<std::function<void(TradeHistoryHtmlPage, long)>> weak_process_page =
            process_page;

        *load_more = [this, state, weak_process_page, complete](std::string last) {
            if (last.empty() || state->completed) {
                (*complete)(TradeHistoryApiResult::ok(
                    TradeHistory{},
                    TradeHistoryApiResult::NO_HTTP_STATUS));
                return;
            }

            auto process_page_ref = weak_process_page.lock();
            if (!process_page_ref) {
                (*complete)(TradeHistoryApiResult::fail(
                    "HTML trade history parser state expired.",
                    TradeHistoryApiResult::NO_RESPONSE_STATUS));
                return;
            }

            state->requested_last_values.push_back(last);
            kurlyk::QueryParams query = {
                {"last", std::move(last)},
                {"user_id", m_user_id},
                {"user_hash", m_user_hash}
            };

            kurlyk::Headers headers = m_api_headers;
            headers.emplace("Cookie", m_cookies);

            auto future = get_http_client().post(
                "/trade_load_more2.php",
                kurlyk::QueryParams(),
                headers,
                kurlyk::utils::to_query_string(query),
                get_rate_limit(RateLimitType::ACCOUNT_INFO)
            );

            auto response_callback = [state,
                                      process_page_ref = std::move(process_page_ref),
                                      complete](
                    kurlyk::HttpResponsePtr response) mutable {
                if (!validate_response(response)) {
                    (*complete)(TradeHistoryApiResult::fail(
                        "HTML trade history load-more HTTP response failed validation.",
                        response ? response->status_code : TradeHistoryApiResult::NO_RESPONSE_STATUS));
                    return;
                }

                try {
                    auto page = parse_trade_history_html_page(
                        response->content,
                        state->account_type);
                    LOGIT_INFO(
                        "Intrade Bar trade history load-more parsed. bytes=",
                        response->content.size(),
                        ", records=",
                        page.records.size(),
                        ", next_last=",
                        page.next_last);
                    (*process_page_ref)(std::move(page), response->status_code);
                } catch (const std::exception& ex) {
                    LOGIT_ERROR("Error parsing trade history load-more HTML: ", ex.what());
                    (*complete)(TradeHistoryApiResult::fail(
                        std::string("Failed to parse HTML trade history load-more: ") + ex.what(),
                        response ? response->status_code : TradeHistoryApiResult::NO_RESPONSE_STATUS));
                }
            };

            add_http_request_task(std::move(future), std::move(response_callback));
        };

        auto future = get_http_client().get(
            "/",
            kurlyk::QueryParams(),
            {
                {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                {"Content-Type", "application/x-www-form-urlencoded"},
                {"Upgrade-Insecure-Requests", "1"},
                {"Cookie", m_cookies}
            },
            get_rate_limit(RateLimitType::ACCOUNT_INFO)
        );

        auto response_callback = [state, process_page, complete](
                kurlyk::HttpResponsePtr response) {
            if (!validate_response(response)) {
                (*complete)(TradeHistoryApiResult::fail(
                    "HTML trade history HTTP response failed validation.",
                    response ? response->status_code : TradeHistoryApiResult::NO_RESPONSE_STATUS));
                return;
            }

            try {
                auto page = parse_trade_history_html_page(
                    response->content,
                    state->account_type);
                LOGIT_INFO(
                    "Intrade Bar trade history HTML parsed. bytes=",
                    response->content.size(),
                    ", has_trade_close=",
                    (response->content.find("trade_close") != std::string::npos),
                    ", has_trade_history=",
                    (response->content.find("trade_history") != std::string::npos),
                    ", has_load_more=",
                    (response->content.find("trade_btn_load_more") != std::string::npos),
                    ", tr_count=",
                    count_substrings(response->content, "<tr"),
                    ", th_count=",
                    count_substrings(response->content, "<th"),
                    ", td_count=",
                    count_substrings(response->content, "<td"),
                    ", records=",
                    page.records.size(),
                    ", next_last=",
                    page.next_last);
                (*process_page)(std::move(page), response->status_code);
            } catch (const std::exception& ex) {
                LOGIT_ERROR("Error parsing trade history HTML: ", ex.what());
                (*complete)(TradeHistoryApiResult::fail(
                    std::string("Failed to parse HTML trade history: ") + ex.what(),
                    response ? response->status_code : TradeHistoryApiResult::NO_RESPONSE_STATUS));
            }
        };

        add_http_request_task(std::move(future), std::move(response_callback));
    }

    inline void RequestManager::request_trade_check(
            int64_t deal_id,
            int retry_attempts,
            std::function<void(
                bool success,
                long status_code,
                double price,
                double profit)> callback_check) {

        // Prepare query parameters
        kurlyk::QueryParams query = {
            {"user_id", m_user_id},
            {"user_hash", m_user_hash},
            {"trade_id", std::to_string(deal_id)}
        };

        // Send POST request
        auto future = get_http_client().post(
            "/trade_check2.php",
            kurlyk::QueryParams(), // No additional URL parameters
            m_api_headers,         // Headers
            kurlyk::utils::to_query_string(query),
            get_rate_limit(RateLimitType::TRADE_RESULT)
        );

        auto callback = [this, deal_id, callback_check, retry_attempts](kurlyk::HttpResponsePtr response) {
            if (!validate_response(response)) {
                callback_check(false, response ? response->status_code : -1, 0.0, 0.0);
                return;
            }

            const std::string& content = response->content;

            // Retry if the response is empty and retry attempts remain
            if (content.empty() && retry_attempts > 0) {
                LOGIT_0ERROR();
                request_trade_check(deal_id, retry_attempts - 1, callback_check);
                return;
            }

            try {
                // Check for error in the response
                if (content.find("error") != std::string::npos) {
                    // Retry if the response contains 'error' and retry attempts remain
                    if (retry_attempts > 0) {
                        LOGIT_0ERROR();
                        request_trade_check(deal_id, retry_attempts - 1, callback_check);
                        return;
                    }
#                   ifdef OPTIONX_LOG_UNIQUE_FILE_INDEX
                    const int log_index = OPTIONX_LOG_UNIQUE_FILE_INDEX;
                    LOGIT_STREAM_ERROR_TO(log_index) << content;
                    LOGIT_PRINT_ERROR(
                        "Trade check failed. Response contains 'error'. Content log was written to file: ",
                        LOGIT_GET_LAST_FILE_NAME(log_index)
                    );
#                   else
                    LOGIT_PRINT_ERROR("Trade check failed. Response contains 'error'.");
#                   endif
                    callback_check(false, response ? response->status_code : -1, 0.0, 0.0);
                    return;
                }

                // Parse the response for price and profit
                size_t separator_pos = content.find(';');
                if (separator_pos == std::string::npos) {
#                   ifdef OPTIONX_LOG_UNIQUE_FILE_INDEX
                    const int log_index = OPTIONX_LOG_UNIQUE_FILE_INDEX;
                    LOGIT_STREAM_ERROR_TO(log_index) << content;
                    LOGIT_PRINT_ERROR(
                        "Trade check failed. Invalid response format. Content log was written to file: ",
                        LOGIT_GET_LAST_FILE_NAME(log_index)
                    );
#                   else
                    LOGIT_PRINT_ERROR("Trade check failed. Invalid response format.");
#                   endif
                    callback_check(false, response ? response->status_code : -1, 0.0, 0.0);
                    return;
                }

                double price = std::stod(content.substr(0, separator_pos));
                double profit = std::stod(content.substr(separator_pos + 1));
                callback_check(true, response->status_code, price, profit);
            } catch (const std::exception& ex) {
                LOGIT_ERROR("Error parsing trade check response: ", ex.what());
                callback_check(false, response ? response->status_code : -1, 0.0, 0.0);
            }
        };

        // Add the HTTP request task with the response handler
        add_http_request_task(std::move(future), std::move(callback));
    }

    inline void RequestManager::request_execute_trade(
            std::shared_ptr<TradeRequest> request,
            std::function<void(
                bool success,
                long status_code,
                int64_t option_id,
                int64_t open_date,
                double open_price,
                const std::string& error_desc)> result_callback) {

        if (!request) {
            LOGIT_ERROR("TradeRequest is null.");
            result_callback(false, -1, 0, 0, 0, "TradeRequest is null.");
            return;
        }

        kurlyk::QueryParams query;

        // Populate query parameters
        query.emplace("user_id", m_user_id);
        query.emplace("user_hash", m_user_hash);
        query.emplace("option", request->symbol);
        query.emplace("investment", std::to_string(request->amount));

        if (request->option_type == OptionType::SPRINT) {
            query.emplace("time", std::to_string(request->duration / time_shield::SEC_PER_MIN));
            query.emplace("date", "0");
            query.emplace("trade_type", "sprint");
        } else
        if (request->option_type == OptionType::CLASSIC) {
            const int64_t zone_offset = 3 * time_shield::SEC_PER_HOUR;
            query.emplace("time", time_shield::to_string("%hh:%mm", request->expiry_time + zone_offset));
            query.emplace("date", time_shield::to_string("%DD-%MM-%YYYY", request->expiry_time + zone_offset));
            query.emplace("trade_type", "classic");
        }

        query.emplace("status", (request->order_type == OrderType::BUY) ? "1" : "2");

        // Send POST request
        auto future = get_http_client().post(
            "/ajax5_new.php",
            kurlyk::QueryParams(),  // No additional URL parameters
            m_api_headers,          // Headers
            kurlyk::utils::to_query_string(query),  // Body parameters
            get_rate_limit(RateLimitType::TRADE_EXECUTION)
        );

        // Define the response handler
        auto callback = [this, result_callback](kurlyk::HttpResponsePtr response) {
            if (!validate_response(response)) {
                LOGIT_ERROR("Invalid response received for trade open request.");
                result_callback(false, response ? response->status_code : -1,
                    0, 0, 0, "Trade open failed due to invalid response.");
                return;
            }
            parse_execute_trade(
                response->content,
                response->status_code,
                std::move(result_callback));
        };

        // Add the HTTP request task
        add_http_request_task(std::move(future), std::move(callback));
    }

    inline void RequestManager::request_find_working_domain_result(
            std::function<void(DomainSelectionResult)> find_callback) {
        request_find_working_domain(
            [find_callback = std::move(find_callback)](
                    bool success,
                    std::string& host) {
                if (!find_callback) return;
                if (!success) {
                    find_callback(DomainSelectionResult::fail("No working domain found."));
                    return;
                }
                find_callback(DomainSelectionResult::ok(DomainSelection{host}));
            });
    }

    inline void RequestManager::request_check_current_host_available_result(
            std::function<void(HostAvailabilityResult)> check_callback) {
        request_check_current_host_available(
            [check_callback = std::move(check_callback)](bool success) {
                if (!check_callback) return;
                if (!success) {
                    check_callback(HostAvailabilityResult::fail("Current host is unavailable."));
                    return;
                }
                check_callback(HostAvailabilityResult::ok(HostAvailability{true}));
            });
    }

    inline void RequestManager::request_profile_result(
            std::function<void(ProfileInfoResult)> profile_callback) {
        request_profile(
            [profile_callback = std::move(profile_callback)](
                    bool success,
                    CurrencyType currency,
                    AccountType account_type) {
                if (!profile_callback) return;
                if (!success) {
                    profile_callback(ProfileInfoResult::fail("Failed to retrieve profile information."));
                    return;
                }
                profile_callback(ProfileInfoResult::ok(ProfileInfo{currency, account_type}));
            });
    }

    inline void RequestManager::request_main_page_result(
            std::shared_ptr<AuthData> auth_data,
            std::function<void(MainPageChallengeResult)> callback) {
        request_main_page(
            std::move(auth_data),
            [callback = std::move(callback)](
                    bool success,
                    const std::string& req_id,
                    const std::string& req_value,
                    const std::string& cookies,
                    const std::string& reason) {
                if (!callback) return;
                if (!success) {
                    callback(MainPageChallengeResult::fail(reason));
                    return;
                }
                callback(MainPageChallengeResult::ok(MainPageChallenge{req_id, req_value, cookies}));
            });
    }

    inline void RequestManager::request_active_trades_snapshot_result(
            std::function<void(ActiveTradesSnapshotResult)> callback) {
        request_active_trades_snapshot(
            [callback = std::move(callback)](
                    bool success,
                    std::vector<ActiveTradeInfo> trades) {
                if (!callback) return;
                if (!success) {
                    callback(ActiveTradesSnapshotResult::fail("Failed to retrieve active trades snapshot."));
                    return;
                }
                callback(ActiveTradesSnapshotResult::ok(ActiveTradesSnapshot{std::move(trades)}));
            });
    }

    inline void RequestManager::request_login_result(
            const std::string& req_id,
            const std::string& req_value,
            const std::string& cookies,
            std::shared_ptr<AuthData> auth_data,
            std::function<void(LoginCredentialsResult)> result_callback) {
        request_login(
            req_id,
            req_value,
            cookies,
            std::move(auth_data),
            [result_callback = std::move(result_callback)](
                    bool success,
                    const std::string& user_id,
                    const std::string& user_hash,
                    const std::string& response_cookies,
                    const std::string& reason) {
                if (!result_callback) return;
                if (!success) {
                    result_callback(LoginCredentialsResult::fail(reason));
                    return;
                }
                result_callback(LoginCredentialsResult::ok(
                    LoginCredentials{user_id, user_hash, response_cookies}));
            });
    }

    inline void RequestManager::request_auth_result(
            const std::string& user_id,
            const std::string& user_hash,
            const std::string& cookies,
            std::shared_ptr<AuthData> auth_data,
            std::function<void(AuthCheckResult)> result_callback) {
        request_auth(
            user_id,
            user_hash,
            cookies,
            std::move(auth_data),
            [result_callback = std::move(result_callback)](
                    bool success,
                    const std::string& reason) {
                if (!result_callback) return;
                if (!success) {
                    result_callback(AuthCheckResult::fail(reason));
                    return;
                }
                result_callback(AuthCheckResult::ok(AuthCheck{}));
            });
    }

    inline void RequestManager::request_balance_result(
            std::function<void(BalanceInfoResult)> balance_callback) {
        request_balance(
            [balance_callback = std::move(balance_callback)](
                    bool success,
                    double balance,
                    CurrencyType currency) {
                if (!balance_callback) return;
                if (!success) {
                    balance_callback(BalanceInfoResult::fail("Failed to retrieve balance."));
                    return;
                }
                balance_callback(BalanceInfoResult::ok(BalanceInfo{balance, currency}));
            });
    }

    inline void RequestManager::request_switch_account_type_result(
            std::function<void(SettingsSwitchResult)> switch_callback) {
        request_settings_switch_result(
            "account type",
            "/user_real_trade.php",
            std::move(switch_callback));
    }

    inline void RequestManager::request_switch_currency_result(
            std::function<void(SettingsSwitchResult)> switch_callback) {
        request_settings_switch_result(
            "currency",
            "/user_currency_edit.php",
            std::move(switch_callback));
    }

    inline void RequestManager::request_price_result(
            std::function<void(PriceSnapshotResult)> price_callback) {
        request_price(
            [price_callback = std::move(price_callback)](
                    bool success,
                    std::vector<SingleTick> ticks) {
                if (!price_callback) return;
                if (!success) {
                    price_callback(PriceSnapshotResult::fail("Failed to retrieve price snapshot."));
                    return;
                }
                price_callback(PriceSnapshotResult::ok(PriceSnapshot{std::move(ticks)}));
            });
    }

    inline void RequestManager::request_bar_history_result(
            const BarHistoryRequest& request,
            std::function<void(BarHistoryApiResult)> callback) {
        if (!callback) return;

        if (request.symbol.empty()) {
            callback(BarHistoryApiResult::fail("Bar history symbol is required."));
            return;
        }
        if (request.timeframe <= 0) {
            callback(BarHistoryApiResult::fail("Bar history timeframe is invalid."));
            return;
        }
        if (request.from_ts <= 0 || request.to_ts < request.from_ts) {
            callback(BarHistoryApiResult::fail("Bar history request range is invalid."));
            return;
        }

        BarHistoryRequest effective_request = request;
        effective_request.symbol = normalize_symbol_name(request.symbol);

        const bool use_binance = uses_binance_bar_history(effective_request.symbol);
        const auto min_from_ts = minimum_bar_history_from_ts(effective_request.symbol);
        if (min_from_ts > 0 && effective_request.from_ts < min_from_ts) {
            effective_request.from_ts = min_from_ts;
        }

        const auto make_empty_sequence = [](const BarHistoryRequest& req, BarPriceSource source) {
            BarSequence sequence;
            sequence.symbol = normalize_symbol_name(req.symbol);
            sequence.provider = to_str(PlatformType::INTRADE_BAR);
            sequence.timeframe = req.timeframe;
            sequence.price_digits = price_digits_for_symbol(sequence.symbol);
            sequence.volume_digits = 0;
            sequence.price_source = source;
            return sequence;
        };

        if (effective_request.from_ts > effective_request.to_ts) {
            const auto source = use_binance ? BarPriceSource::LAST : effective_request.price_source;
            callback(BarHistoryApiResult::ok(
                make_empty_sequence(effective_request, source)));
            return;
        }

        std::string binance_interval;
        std::int64_t max_bars_per_request = FX_HISTORY_MAX_BARS_PER_REQUEST;
        if (use_binance) {
            if (effective_request.price_source == BarPriceSource::BID ||
                effective_request.price_source == BarPriceSource::ASK ||
                effective_request.price_source == BarPriceSource::UNKNOWN) {
                callback(BarHistoryApiResult::fail(
                    "BTCUSDT bar history is available only as LAST trade-price bars."));
                return;
            }

            binance_interval = binance_interval_from_timeframe(effective_request.timeframe);
            if (binance_interval.empty()) {
                callback(BarHistoryApiResult::fail(
                    "Unsupported Binance kline timeframe for BTCUSDT history."));
                return;
            }
            max_bars_per_request = BINANCE_KLINES_MAX_BARS_PER_REQUEST;
        } else {
            if (effective_request.price_source == BarPriceSource::UNKNOWN ||
                effective_request.price_source == BarPriceSource::LAST) {
                callback(BarHistoryApiResult::fail(
                    "Intrade FX bar history supports only BID, ASK, or MID price sources."));
                return;
            }
            if (effective_request.timeframe % time_shield::SEC_PER_MIN != 0) {
                callback(BarHistoryApiResult::fail(
                    "Intrade FX bar history supports minute-based timeframes only."));
                return;
            }
        }

        auto chunks = build_bar_history_chunks(
            effective_request.from_ts,
            effective_request.to_ts,
            effective_request.timeframe,
            max_bars_per_request);

        struct BarHistoryState {
            BarHistoryRequest request;
            std::vector<BarHistoryChunk> chunks;
            std::string binance_interval;
            BarSequence sequence;
            std::size_t next_index = 0;
            long last_status = BarHistoryApiResult::NO_HTTP_STATUS;
            bool use_binance = false;
            std::function<void(BarHistoryApiResult)> callback;
        };

        auto state = std::make_shared<BarHistoryState>();
        state->request = effective_request;
        state->chunks = std::move(chunks);
        state->binance_interval = std::move(binance_interval);
        state->use_binance = use_binance;
        state->sequence = make_empty_sequence(
            effective_request,
            use_binance ? BarPriceSource::LAST : effective_request.price_source);
        state->callback = std::move(callback);

        auto finish_success = [state]() {
            auto& bars = state->sequence.bars;
            std::sort(bars.begin(), bars.end(), [](const Bar& lhs, const Bar& rhs) {
                return lhs.time_ms < rhs.time_ms;
            });
            bars.erase(
                std::unique(bars.begin(), bars.end(), [](const Bar& lhs, const Bar& rhs) {
                    return lhs.time_ms == rhs.time_ms;
                }),
                bars.end());

            state->callback(BarHistoryApiResult::ok(
                std::move(state->sequence),
                state->last_status));
        };

        auto request_next = std::make_shared<std::function<void()>>();
        *request_next = [this, state, request_next, finish_success]() {
            if (state->next_index >= state->chunks.size()) {
                finish_success();
                return;
            }

            const auto chunk = state->chunks[state->next_index++];

            std::future<kurlyk::HttpResponsePtr> future;
            if (state->use_binance) {
                kurlyk::QueryParams query = {
                    {"symbol", state->request.symbol},
                    {"interval", state->binance_interval},
                    {"startTime", std::to_string(time_shield::sec_to_ms(chunk.from_ts))},
                    {"endTime", std::to_string(time_shield::sec_to_ms(chunk.to_ts))},
                    {"limit", std::to_string(BINANCE_KLINES_MAX_BARS_PER_REQUEST)}
                };
                future = m_binance_client.get(
                    "/api/v3/klines",
                    query,
                    {{"Accept", "application/json"}},
                    get_rate_limit(RateLimitType::BTC_BAR_HISTORY));
            } else {
                kurlyk::QueryParams query = {
                    {"symbol", fx_history_query_symbol(state->request.symbol)},
                    {"resolution", std::to_string(state->request.timeframe / time_shield::SEC_PER_MIN)},
                    {"from", std::to_string(chunk.from_ts)},
                    {"to", std::to_string(chunk.to_ts)}
                };
                future = get_http_client().get(
                    "/fxhis/",
                    query,
                    {{"Accept", "application/json"}, {"Cookie", m_cookies}},
                    get_rate_limit(RateLimitType::FX_BAR_HISTORY));
            }

            auto response_callback = [state, request_next](
                    kurlyk::HttpResponsePtr response) {
                if (!validate_response(response)) {
                    state->callback(BarHistoryApiResult::fail(
                        "Bar history HTTP response failed validation.",
                        response ? response->status_code : BarHistoryApiResult::NO_RESPONSE_STATUS));
                    return;
                }

                state->last_status = response->status_code;
                try {
                    auto chunk_sequence = state->use_binance
                        ? parse_binance_klines_bar_history(response->content, state->request)
                        : parse_fxhis_bar_history(response->content, state->request);
                    auto& target = state->sequence.bars;
                    target.insert(
                        target.end(),
                        std::make_move_iterator(chunk_sequence.bars.begin()),
                        std::make_move_iterator(chunk_sequence.bars.end()));
                } catch (const std::exception& ex) {
                    state->callback(BarHistoryApiResult::fail(
                        std::string("Failed to parse bar history: ") + ex.what(),
                        response->status_code));
                    return;
                }

                (*request_next)();
            };

            add_http_request_task(std::move(future), std::move(response_callback));
        };

        (*request_next)();
    }

    inline void RequestManager::request_trade_check_result(
            int64_t deal_id,
            int retry_attempts,
            std::function<void(TradeCheckResult)> callback_check) {
        request_trade_check(
            deal_id,
            retry_attempts,
            [callback_check = std::move(callback_check)](
                    bool success,
                    long status_code,
                    double price,
                    double profit) {
                if (!callback_check) return;
                if (!success) {
                    callback_check(TradeCheckResult::fail(
                        "Failed to retrieve trade result.",
                        status_code));
                    return;
                }
                callback_check(TradeCheckResult::ok(TradeCheckInfo{price, profit}, status_code));
            });
    }

    inline void RequestManager::request_execute_trade_result(
            std::shared_ptr<TradeRequest> request,
            std::function<void(TradeOpenResult)> result_callback) {
        request_execute_trade(
            std::move(request),
            [result_callback = std::move(result_callback)](
                    bool success,
                    long status_code,
                    int64_t option_id,
                    int64_t open_date,
                    double open_price,
                    const std::string& error_desc) {
                if (!result_callback) return;
                if (!success) {
                    result_callback(TradeOpenResult::fail(error_desc, status_code));
                    return;
                }
                result_callback(TradeOpenResult::ok(
                    TradeOpenInfo{option_id, open_date, open_price},
                    status_code));
            });
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_REQUEST_MANAGER_HPP_INCLUDED
