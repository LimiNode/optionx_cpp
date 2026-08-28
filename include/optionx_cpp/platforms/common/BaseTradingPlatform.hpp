#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_COMMON_BASE_TRADING_PLATFORM_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_COMMON_BASE_TRADING_PLATFORM_HPP_INCLUDED

/// \file BaseTradingPlatform.hpp
/// \brief Base class for interacting with trading platforms.

namespace optionx::platforms {

    /// \class BaseTradingPlatform
    /// \brief Base endpoint facade for trading platforms, account data, lifecycle, and connection state.
    class BaseTradingPlatform : public BaseEndpoint, public BaseTradingApi {
    public:
        BaseTradingPlatform(std::shared_ptr<BaseAccountInfoData> account_info)
            : m_account_info(std::move(account_info)),
              m_account_provider(m_account_info),
              m_event_bus(), m_task_manager(),
              m_account_info_handler(m_event_bus),
              m_trading_condition_handler(m_event_bus) {
        }

        virtual ~BaseTradingPlatform() noexcept {
            shutdown();
        }

        /// \brief Returns a reference to the account info callback.
        virtual account_info_callback_t& on_account_info() {
            return m_account_info_handler.on_account_info();
        }

        /// \brief Returns a reference to the trading-condition callback.
        virtual trading_condition_callback_t& on_trading_condition() {
            return m_trading_condition_handler.on_trading_condition();
        }

        /// \brief Configures the endpoint with a supported platform configuration DTO.
        /// \param config Endpoint configuration object.
        /// \return True if the configuration was accepted; false otherwise.
        bool configure(std::unique_ptr<IEndpointConfig> config) override {
            if (!config) return false;

            auto* auth_data = dynamic_cast<IAuthData*>(config.get());
            if (!auth_data) return false;

            config.release();
            return configure_auth(std::unique_ptr<IAuthData>(auth_data));
        }

        /// \brief Sets the authorization data for the platform.
        /// \param auth_data Authorization data.
        /// \return True if the authorization data was set successfully; false otherwise.
        virtual bool configure_auth(std::unique_ptr<IAuthData> auth_data) {
            if (!auth_data) return false;
            m_event_bus.notify_async(std::make_unique<events::AuthDataEvent>(std::move(auth_data)));
            return true;
        }

        /// \brief Requests the list of available trading symbols.
        /// \param callback Callback function to receive the symbol list.
        virtual bool fetch_symbol_list(std::function<void(const std::vector<SymbolInfo>&)> callback) { return false; };

        /// \brief Initiates a connection to the trading platform.
        /// \param callback Callback to handle connection result with error code.
        void connect(connection_callback_t callback) override {
            m_event_bus.notify_async(std::make_unique<events::ConnectRequestEvent>(std::move(callback)));
        }

        /// \brief Disconnects from the trading platform.
        /// \param callback Callback to handle completion of the disconnection process.
        void disconnect(connection_callback_t callback) override {
            m_event_bus.notify_async(std::make_unique<events::DisconnectRequestEvent>(std::move(callback)));
        }

        /// \brief Checks if the platform is connected.
        /// \return True if connected, otherwise false.
        bool is_connected() const override {
            return m_account_provider.get_info<bool>(AccountInfoType::CONNECTION_STATUS);
        }

        /// \brief Retrieves account information based on a detailed request.
        /// \tparam T The expected type of the returned value.
        /// \param request The request containing specific parameters for data retrieval.
        /// \return The requested account information of type `T`.
        template<class T>
        T get_info(const AccountInfoRequest& request) const {
            return m_account_provider.get_info<T>(request);
        }

        /// \brief Retrieves account information by `AccountInfoType`.
        /// \tparam T The expected type of the returned value.
        /// \param type The specific type of account information.
        /// \param timestamp Optional timestamp for retrieving historical values (default: 0).
        /// \return The requested account information of type `T`.
        template<class T>
        T get_info(AccountInfoType type, int64_t timestamp = 0) const {
            return m_account_provider.get_info<T>(type, timestamp);
        }

        /// \brief Retrieves account information by account type.
        /// \tparam T The expected type of the returned value.
        /// \param account The `AccountType` (e.g., Demo, Real).
        /// \param timestamp Optional timestamp for retrieving historical values (default: 0).
        /// \return The requested account information of type `T`
        template<class T>
        T get_info(AccountType account, int64_t timestamp = 0) const {
            return m_account_provider.get_info<T>(account, timestamp);
        }

        /// \brief Retrieves account information by currency type.
        /// \tparam T The expected type of the returned value.
        /// \param currency The `CurrencyType` (e.g., USD, EUR).
        /// \param timestamp Optional timestamp for retrieving historical values (default: 0).
        /// \return The requested account information of type `T`.
        template<class T>
        T get_info(CurrencyType currency, int64_t timestamp = 0) const {
            return m_account_provider.get_info<T>(currency, timestamp);
        }

        /// \brief Starts the platform's event loop and component lifecycle.
        /// \details Adds initialization and periodic update tasks.
        ///          If start_worker_thread is true (default), TaskManager launches its own worker thread.
        ///          Otherwise, the caller must periodically call process() manually.
        ///          Repeated calls before shutdown are ignored to avoid duplicate component initialization
        ///          and duplicate processing loops.
        /// \param start_worker_thread  Whether to use an internal background thread for updates.
        void run(bool start_worker_thread  = true) override {
            std::lock_guard<std::mutex> lock(m_lifecycle_mutex);

            if (m_stopping.load(std::memory_order_acquire) ||
                m_stopped .load(std::memory_order_acquire)) {
                LOGIT_WARN("run() after shutdown()");
                return;
            }

            if (m_running.load(std::memory_order_acquire)) {
                LOGIT_WARN("run() called while platform is already running.");
                return;
            }

            bool initialize_scheduled = false;
            bool loop_scheduled = false;

            try {
                initialize_scheduled = m_task_manager.add_single_task("initialize", [this](
                        std::shared_ptr<utils::Task> task){
                    if (task->is_shutdown()) return;
                    LOGIT_TRACE0();
                    for (auto* component : m_components) component->initialize();
                    on_once();
                });

                loop_scheduled = m_task_manager.add_periodic_task("loop", 1, [this](
                        std::shared_ptr<utils::Task> task){
                    m_event_bus.process();
                    if (task->is_shutdown()) {
                        LOGIT_TRACE0();
                        return;
                    }
                    for (auto* component : m_components) component->process();
                    on_loop();
                });

                if (!initialize_scheduled || !loop_scheduled) {
                    LOGIT_WARN("run() failed to schedule lifecycle tasks.");
                    if (initialize_scheduled || loop_scheduled) {
                        m_task_manager.shutdown();
                    }
                    m_running.store(false, std::memory_order_release);
                    return;
                }
            
                if (start_worker_thread) {
                    m_task_manager.run();
                }

                m_running.store(true, std::memory_order_release);
            } catch (...) {
                if (initialize_scheduled || loop_scheduled) {
                    m_task_manager.shutdown();
                }
                m_running.store(false, std::memory_order_release);
                throw;
            }
        };
        
        /// \brief Manually processes pending tasks and events.
        /// \details Should be called periodically if internal threading is disabled (run(false)).
        void process() override {
            m_task_manager.process();
        }

        /// \brief Queues work for serialized execution in the platform owner loop.
        /// \details This is the cross-thread ingress point for components such as
        ///          bots that must not call platform-owned provider APIs directly.
        ///          Accepted work runs from TaskManager processing and can be
        ///          cancelled by platform shutdown before execution.
        /// \param callback Work that must execute in the platform owner loop.
        /// \return True when the task was accepted for owner-loop processing.
        bool post_task(std::function<void()> callback) {
            if (!callback) return false;

            std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
            if (m_stopping.load(std::memory_order_acquire) ||
                m_stopped.load(std::memory_order_acquire)) {
                return false;
            }

            return m_task_manager.add_single_task(
                "external",
                [callback = std::move(callback)](
                        std::shared_ptr<utils::Task> task) mutable {
                    if (task->is_shutdown()) return;
                    callback();
                });
        }

        /// \brief Shuts down the platform, stopping the event loop and tasks.
        /// \details Always calls shutdown() on TaskManager, regardless of internal thread usage.
        void shutdown() noexcept override {
            {
                std::lock_guard<std::mutex> lock(m_lifecycle_mutex);

                if (m_stopped.load(std::memory_order_acquire)) return;

                if (m_stopping.exchange(true, std::memory_order_acq_rel)) return;
            }
            
            m_task_manager.shutdown();
            
            for (auto* component : m_components) {
                if (component) {
                    component->shutdown();
                }
            }
            on_shutdown();

            m_event_bus.drain();

            std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
            m_running.store(false, std::memory_order_release);
            m_stopped.store(true, std::memory_order_release);
        };

        /// \brief Returns a reference to the event bus.
        utils::EventBus& event_bus() { return m_event_bus; }

        /// \brief Registers a component for platform lifecycle processing.
        /// \param component The component to be registered.
        void register_component(components::BaseComponent* component) {
            m_components.push_back(component);
        }

        /// \brief Retrieves the API type associated with this authorization data.
        /// \return The `PlatformType` associated with this authentication data.
        virtual PlatformType platform_type() const = 0;

    protected:
        std::shared_ptr<BaseAccountInfoData> m_account_info;
        components::AccountInfoProvider         m_account_provider;
        utils::EventBus                      m_event_bus;
        utils::TaskManager                   m_task_manager;
        components::BaseAccountInfoHandler        m_account_info_handler;
        components::BaseTradingConditionHandler    m_trading_condition_handler;
        std::vector<components::BaseComponent*>    m_components;
        std::mutex                           m_lifecycle_mutex;
        std::atomic<bool>                    m_running{false};
        std::atomic<bool>                    m_stopping{false};
        std::atomic<bool>                    m_stopped{false};

        virtual void on_once() {};

        virtual void on_loop() {};

        virtual void on_shutdown() {};
    };

}; // namespace optionx

#endif // OPTIONX_HEADER_PLATFORMS_COMMON_BASE_TRADING_PLATFORM_HPP_INCLUDED
