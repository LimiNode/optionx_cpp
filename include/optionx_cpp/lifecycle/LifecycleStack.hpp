#pragma once
#ifndef OPTIONX_HEADER_LIFECYCLE_LIFECYCLE_STACK_HPP_INCLUDED
#define OPTIONX_HEADER_LIFECYCLE_LIFECYCLE_STACK_HPP_INCLUDED

/// \file LifecycleStack.hpp
/// \brief Defines optional ordered processing and staged shutdown for modules.

namespace optionx::lifecycle {

    /// \class LifecycleStack
    /// \brief Drives non-owning lifecycle modules in dependency order.
    /// \details Register lower-level dependencies first and dependents last.
    ///          process() runs forward. shutdown() stops one module at a time in
    ///          reverse order, while lower-level dependencies keep processing.
    ///          All methods must be called from the same owner loop.
    class LifecycleStack final : public ILifecycleModule {
    public:
        LifecycleStack() = default;

        LifecycleStack(const LifecycleStack&) = delete;
        LifecycleStack& operator=(const LifecycleStack&) = delete;
        LifecycleStack(LifecycleStack&&) = delete;
        LifecycleStack& operator=(LifecycleStack&&) = delete;

        /// \brief Registers a non-owning module reference.
        /// \param module Module that must outlive this stack and its shutdown.
        /// \return False for duplicate/self registration or after shutdown starts.
        bool add_module(ILifecycleModule& module) {
            if (m_shutdown_requested || &module == this) return false;
            if (std::find(m_modules.begin(), m_modules.end(), &module) !=
                m_modules.end()) {
                return false;
            }
            m_modules.push_back(&module);
            return true;
        }

        /// \brief Returns the number of registered modules.
        [[nodiscard]] std::size_t size() const noexcept {
            return m_modules.size();
        }

        /// \brief Returns true when no modules are registered.
        [[nodiscard]] bool empty() const noexcept {
            return m_modules.empty();
        }

        /// \brief Returns true after staged shutdown has been requested.
        [[nodiscard]] bool is_shutdown_requested() const noexcept {
            return m_shutdown_requested;
        }

        /// \brief Processes modules forward and advances staged shutdown.
        void process() override {
            if (m_stopped) return;

            const auto process_count = m_shutdown_requested
                ? m_shutdown_cursor
                : m_modules.size();
            for (std::size_t index = 0; index < process_count; ++index) {
                auto* module = m_modules[index];
                if (module && !module->is_stopped()) {
                    module->process();
                }
            }

            if (m_shutdown_requested) advance_shutdown();
        }

        /// \brief Starts staged reverse-order shutdown.
        void shutdown() noexcept override {
            if (m_stopped || m_shutdown_requested) return;
            m_shutdown_requested = true;
            m_shutdown_cursor = m_modules.size();
            advance_shutdown();
        }

        /// \brief Returns true after every registered module stopped.
        [[nodiscard]] bool is_stopped() const noexcept override {
            return m_stopped;
        }

    private:
        void advance_shutdown() noexcept {
            while (m_shutdown_cursor != 0) {
                auto* module = m_modules[m_shutdown_cursor - 1];
                if (!module || module->is_stopped()) {
                    --m_shutdown_cursor;
                    m_current_shutdown_requested = false;
                    continue;
                }

                if (!m_current_shutdown_requested) {
                    m_current_shutdown_requested = true;
                    module->shutdown();
                }
                if (!module->is_stopped()) return;

                --m_shutdown_cursor;
                m_current_shutdown_requested = false;
            }
            m_stopped = true;
        }

        std::vector<ILifecycleModule*> m_modules;
        std::size_t m_shutdown_cursor = 0;
        bool m_shutdown_requested = false;
        bool m_current_shutdown_requested = false;
        bool m_stopped = false;
    };

} // namespace optionx::lifecycle

#endif // OPTIONX_HEADER_LIFECYCLE_LIFECYCLE_STACK_HPP_INCLUDED
