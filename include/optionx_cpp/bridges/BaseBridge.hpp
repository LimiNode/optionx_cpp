#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_BASE_BRIDGE_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_BASE_BRIDGE_HPP_INCLUDED

/// \file BaseBridge.hpp
/// \brief Declares the BaseBridge abstract class, serving as a foundation for bridge implementations.

#include <optionx_cpp/data/account.hpp>
#include <optionx_cpp/data/bridge.hpp>

#include <functional>
#include <memory>

namespace optionx::bridges {

    /// \class BaseBridge
    /// \brief Abstract base class for bridges facilitating communication between the application and external platforms.
    class BaseBridge {
    public:
        using trade_signal_callback_t = std::function<void(std::unique_ptr<TradeSignal>)>;
        using signal_report_callback_t = std::function<void(const BridgeSignalReport&)>;
        using signal_id_allocator_t = std::function<SignalId()>;

        /// \brief Virtual destructor for safe polymorphic destruction.
        virtual ~BaseBridge() = default;

        /// \brief Configures the bridge with the provided configuration data.
        /// \param config Unique pointer to the bridge configuration data.
        /// \return True if the configuration was set successfully; false otherwise.
        virtual bool configure(std::unique_ptr<IBridgeConfig> config) {
            if (!config) return false;
            // Process the configuration data as needed.
            return true;
        }

        /// \brief Retrieves a reference to the bridge status update callback function.
        /// \return Reference to the status update callback function.
        virtual bridge_status_callback_t& on_status_update() {
            static bridge_status_callback_t null_callback;
            return null_callback;
        }

        /// \brief Retrieves a reference to the trade signal callback function.
        /// \return Reference to the trade signal callback function.
        virtual trade_signal_callback_t& on_trade_signal() {
            static trade_signal_callback_t null_callback;
            return null_callback;
        }

        /// \brief Retrieves the bridge signal diagnostic callback function.
        /// \return Reference to the signal report callback function.
        virtual signal_report_callback_t& on_signal_report() {
            static signal_report_callback_t null_callback;
            return null_callback;
        }

        /// \brief Returns allocator used to initialize empty TradeSignal::signal_id values.
        /// \return Reference to the signal ID allocator function.
        virtual signal_id_allocator_t& on_signal_id() {
            static signal_id_allocator_t null_allocator;
            return null_allocator;
        }

        /// \brief Updates the account information with the provided data.
        /// \param info Structure containing account information updates.
        virtual void update_account_info(const AccountInfoUpdate& info) = 0;

        /// \brief Updates the bridge with a trade result produced by the trading pipeline.
        /// \param request Original trade request.
        /// \param result Current trade result snapshot.
        virtual void update_trade_result(
                const TradeRequest& request,
                const TradeResult& result) {
            (void)request;
            (void)result;
        }

        /// \brief Initiates the bridge's main operations.
        virtual void run() = 0;

        /// \brief Shuts down the bridge, terminating its operations.
        virtual void shutdown() = 0;
    };

} // namespace optionx::bridges

#endif // OPTIONX_HEADER_BRIDGES_BASE_BRIDGE_HPP_INCLUDED
