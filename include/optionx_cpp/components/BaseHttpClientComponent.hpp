#pragma once
#ifndef OPTIONX_HEADER_COMPONENTS_BASE_HTTP_CLIENT_COMPONENT_HPP_INCLUDED
#define OPTIONX_HEADER_COMPONENTS_BASE_HTTP_CLIENT_COMPONENT_HPP_INCLUDED

/// \file BaseHttpClientComponent.hpp
/// \brief Provides the BaseHttpClientComponent class for handling HTTP requests and processing trade events.

#ifndef OPTIONX_TIMESTAMP_MS
#define OPTIONX_TIMESTAMP_MS time_shield::timestamp_ms()
#endif

namespace optionx::components {

    /// \class BaseHttpClientComponent
    /// \brief Handles HTTP requests and processes trade request events.
    class BaseHttpClientComponent : public components::BaseComponent {
    public:

        /// \brief Constructor initializing the HTTP client component with an event bus.
        /// \param bus Reference to the event bus for event handling.
        /// \param account_info Shared pointer to account information data.
        explicit BaseHttpClientComponent(utils::EventBus& bus)
            : BaseComponent(bus) {
        }

        /// \brief Default virtual destructor.
        virtual ~BaseHttpClientComponent() noexcept override {
            deinitialize_rate_limits();
            m_client.cancel_requests();
            m_http_tasks.clear();
        }

        /// \brief Handles an event notification received as a raw pointer.
        /// \param event The event received, passed as a raw pointer.
        void on_event(const utils::Event* const event) override {}

        /// \brief Processes queued requests (to be implemented by derived classes).
        void process() override final {
            process_http_responses();
        }

        void shutdown() override final {
            deinitialize_rate_limits();
            m_client.cancel_requests();
        }

        kurlyk::HttpClient& get_http_client() {
            return m_client;
        }

        void set_max_pending_requests(size_t max) {
            kurlyk::set_max_pending_requests(max);
        }

        /// \brief Gets the rate limit for the specified type.
        /// \param rate_limit_id The type of rate limit to retrieve.
        /// \return The rate limit value for the specified type.
        template<class T>
        uint32_t get_rate_limit(T rate_limit_id) const {
            auto it = m_rate_limits.find(static_cast<uint32_t>(rate_limit_id));
            if (it == m_rate_limits.end()) return 0;
            return it->second ? static_cast<uint32_t>(it->second->id()) : 0;
        }

        /// \brief Adds a new HTTP request task to the list.
        /// \param future The future object representing the pending HTTP response.
        /// \param callback The callback function to handle the response.
        void add_http_request_task(
                std::future<kurlyk::HttpResponsePtr> future,
                std::function<void(kurlyk::HttpResponsePtr)> callback) {
            m_http_tasks.push_back({std::move(future), std::move(callback)});
        }

    protected:
        kurlyk::HttpClient m_client; ///< The HTTP client for making requests.
        std::unordered_map<uint32_t, kurlyk::HttpRateLimitHandlePtr> m_rate_limits; ///< Rate limit handles by ID.

        /// \class HttpRequestTask
        /// \brief Represents a single HTTP request task with a future and a callback.
        struct HttpRequestTask {
            std::future<kurlyk::HttpResponsePtr>            future;     ///< Future holding the HTTP response.
            std::function<void(kurlyk::HttpResponsePtr)>    callback;   ///< Callback for processing the response.

            /// \brief Checks if the response is ready.
            /// \return True if the future is ready, false otherwise.
            bool ready() const {
                return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            }
        };

        std::list<HttpRequestTask> m_http_tasks;      ///< List of pending HTTP request tasks.

        /// \brief Deinitializes all rate limits by resetting them.
        void deinitialize_rate_limits() {
            for (auto& item : m_rate_limits) {
                if (item.second) {
                    kurlyk::remove_limit(item.second);
                    item.second.reset();
                }
            }
            m_rate_limits.clear();
        }

        /// \brief Creates a rate limit based on Requests Per Minute (RPM).
        /// \param
        /// \param requests_per_minute Maximum number of requests allowed per minute.
        template<class T>
        void set_rate_limit_rpm(T rate_limit_id, uint32_t requests_per_minute) {
            const auto id = static_cast<uint32_t>(rate_limit_id);
            auto handle = kurlyk::create_rate_limit_rpm(requests_per_minute);
            replace_rate_limit(id, std::move(handle));
        }

        /// \brief Creates a rate limit based on Requests Per Second (RPS).
        /// \param
        /// \param requests_per_second Maximum number of requests allowed per second.
        template<class T>
        void set_rate_limit_rps(T rate_limit_id, uint32_t requests_per_second) {
            const auto id = static_cast<uint32_t>(rate_limit_id);
            auto handle = kurlyk::create_rate_limit_rps(requests_per_second);
            replace_rate_limit(id, std::move(handle));
        }

        /// \brief Creates a rate limit with an explicit period.
        /// \param rate_limit_id The logical rate-limit type.
        /// \param requests_per_period Maximum requests accepted in the period.
        /// \param period_ms Period length in milliseconds.
        /// \param sequential Whether a request holds the limit until completion.
        template<class T>
        void set_rate_limit(
                T rate_limit_id,
                uint32_t requests_per_period,
                uint32_t period_ms,
                bool sequential = false) {
            const auto id = static_cast<uint32_t>(rate_limit_id);
            auto handle = kurlyk::create_rate_limit(
                requests_per_period,
                period_ms,
                sequential);
            replace_rate_limit(id, std::move(handle));
        }

    private:

        void replace_rate_limit(uint32_t id, kurlyk::HttpRateLimitHandlePtr handle) {
            auto it = m_rate_limits.find(id);
            if (it != m_rate_limits.end()) {
                if (it->second) kurlyk::remove_limit(it->second);
                it->second = std::move(handle);
                return;
            }

            m_rate_limits.emplace(id, std::move(handle));
        }

        /// \brief Processes all pending HTTP responses.
        void process_http_responses() {
            auto it = m_http_tasks.begin();
            while (it != m_http_tasks.end()) {
                if (it->ready()) {
                    try {
                        // Retrieve the response and call the callback.
                        auto response = it->future.get();
                        if (it->callback) it->callback(std::move(response));
                    } catch (const std::exception& ex) {
                        LOGIT_ERROR(ex);
                        try {
                            auto response = std::make_unique<kurlyk::HttpResponse>();
                            response->ready = true;
                            response->error_code = kurlyk::utils::make_error_code(
                                kurlyk::utils::ClientError::AbortedDuringDestruction);
                            response->error_message = ex.what();
                            if (it->callback) it->callback(std::move(response));
                        } catch(...) {
                            LOGIT_ERROR("Unknown error processing HTTP response");
                        }
                    }

                    it = m_http_tasks.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }; // BaseHttpClientComponent

}

#endif // OPTIONX_HEADER_COMPONENTS_BASE_HTTP_CLIENT_COMPONENT_HPP_INCLUDED
