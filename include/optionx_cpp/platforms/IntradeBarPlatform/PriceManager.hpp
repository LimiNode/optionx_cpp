#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_PRICE_MANAGER_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_PRICE_MANAGER_HPP_INCLUDED

/// \file PriceManager.hpp
/// \brief Defines the PriceManager class responsible for handling price updates and related events.

namespace optionx::platforms::intrade_bar {

    /// \class PriceManager
    /// \brief Manages price updates and price-related events.
    ///
    /// The `PriceManager` class is responsible for fetching, updating, and processing tick data
    /// from the trading platform to keep price information up-to-date. It subscribes to events
    /// related to connection status and account information updates.
    class PriceManager final : public components::BaseComponent {
    public:

        /// \brief Constructs the price manager.
        /// \param platform Reference to the trading platform.
        /// \param request_manager Reference to the request manager for making HTTP requests.
        explicit PriceManager(
                BaseTradingPlatform& platform,
                RequestManager& request_manager)
                : BaseComponent(platform.event_bus()), m_request_manager(request_manager) {
            subscribe<events::ConnectRequestEvent>();
            subscribe<events::DisconnectRequestEvent>();
            subscribe<events::AccountInfoUpdateEvent>();
            platform.register_component(this);
        }

        /// \brief Default destructor.
        virtual ~PriceManager() = default;

        /// \brief Processes incoming events and dispatches them to the appropriate handlers.
        /// \param event The received event.
        void on_event(const utils::Event* const event) override;

        /// \brief Periodically processes price updates and account-related tasks.
        void process() override;

        /// \brief Shuts down all active price-related operations.
        void shutdown() override;

    private:
        RequestManager&    m_request_manager; ///< Reference to the request manager.
        utils::TaskManager m_task_manager;    ///< Task manager for handling asynchronous tasks.
        std::unordered_map<std::string, Tick> m_ticks; ///< Latest tick payload by symbol.
        bool m_has_price_update = false; ///< Flag indicating whether a price update is in progress.

        /// \brief Initiates the process of retrieving price updates.
        void handle_price_update();

        /// \brief Handles an event triggered when a connection request is received.
        /// \param event The connection request event.
        void handle_event(const events::ConnectRequestEvent& event);

        /// \brief Handles an event triggered when a disconnection request is received.
        /// \param event The disconnection request event.
        void handle_event(const events::DisconnectRequestEvent& event);

        /// \brief Handles an account information update event.
        /// \param event The account info update event.
        void handle_event(const events::AccountInfoUpdateEvent& event);

        /// \brief Processes a periodic price update task.
        /// \param task The scheduled task that triggered this update.
        void handle_price_update(std::shared_ptr<utils::Task> task);
    };

    inline void PriceManager::on_event(const utils::Event* const event) {
        if (const auto* msg = dynamic_cast<const events::ConnectRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::DisconnectRequestEvent*>(event)) {
            handle_event(*msg);
        } else
        if (const auto* msg = dynamic_cast<const events::AccountInfoUpdateEvent*>(event)) {
            handle_event(*msg);
        }
    };

    inline void PriceManager::handle_event(const events::ConnectRequestEvent& event) {
        LOGIT_0TRACE();
        m_task_manager.shutdown();
        m_ticks.clear();
    }

    inline void PriceManager::handle_event(const events::DisconnectRequestEvent& event) {
        LOGIT_0TRACE();
        m_task_manager.shutdown();
        m_ticks.clear();
    }

    inline void PriceManager::handle_event(const events::AccountInfoUpdateEvent& event) {
        using Status = events::AccountInfoUpdateEvent::Status;
        if (event.status == Status::CONNECTED) {
            LOGIT_0TRACE();
            m_task_manager.add_periodic_task(
                    "event(AccountInfoUpdateEvent)-1sec",
                    time_shield::MS_PER_SEC, 
                    [this](
                        std::shared_ptr<utils::Task> task){
                if (task->is_shutdown()) {
                    m_ticks.clear();
                    return;
                }
                handle_price_update(std::move(task));
            });
        } else
        if (event.status == Status::DISCONNECTED) {
            LOGIT_0TRACE();
            m_task_manager.shutdown();
            m_ticks.clear();
        }
    }

    inline void PriceManager::process() {
        m_task_manager.process();
    }

    inline void PriceManager::shutdown() {
        m_task_manager.shutdown();
    }

    inline void PriceManager::handle_price_update(std::shared_ptr<utils::Task> task) {
        if (m_has_price_update) return;
        m_has_price_update = true;
        LOGIT_DEBUG("Intrade Bar price: requesting price snapshot.");
        m_request_manager.request_price_batches([this, task](
                bool success,
                std::vector<events::TickUpdateBatch> batches) {
            m_has_price_update = false;
            if (task->is_shutdown()) {
                m_ticks.clear();
                return;
            }
            if (!success) {
                LOGIT_WARN("Intrade Bar price: snapshot request failed, slowing polling.");
                task->set_period(time_shield::MS_PER_5_MIN);
                return;
            }

            task->set_period(time_shield::MS_PER_SEC);
            LOGIT_DEBUG("Intrade Bar price: snapshot received. batches=", batches.size());

            for (auto& batch : batches) {
                for (auto& tick : batch.items) {
                    auto it = m_ticks.find(batch.symbol);
                    if (it == m_ticks.end()) {
                        tick.set_flag(TickUpdateFlags::ASK_UPDATED);
                        tick.set_flag(TickUpdateFlags::BID_UPDATED);
                        m_ticks[batch.symbol] = tick;
                        continue;
                    }

                    if (!utils::compare_with_precision(
                            it->second.ask,
                            tick.ask,
                            batch.price_digits)) {
                        tick.set_flag(TickUpdateFlags::ASK_UPDATED);
                    }
                    if (!utils::compare_with_precision(
                            it->second.bid,
                            tick.bid,
                            batch.price_digits)) {
                        tick.set_flag(TickUpdateFlags::BID_UPDATED);
                    }
                    it->second = tick;
                }
            }

            notify(events::PriceUpdateEvent(std::move(batches), MarketDataUpdateSource::POLLING));
        });
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_PRICE_MANAGER_HPP_INCLUDED
