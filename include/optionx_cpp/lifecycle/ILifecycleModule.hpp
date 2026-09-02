#pragma once
#ifndef OPTIONX_HEADER_LIFECYCLE_ILIFECYCLE_MODULE_HPP_INCLUDED
#define OPTIONX_HEADER_LIFECYCLE_ILIFECYCLE_MODULE_HPP_INCLUDED

/// \file ILifecycleModule.hpp
/// \brief Declares the common process and graceful-shutdown module contract.

namespace optionx::lifecycle {

    /// \class ILifecycleModule
    /// \brief Common interface for modules driven by an application owner loop.
    /// \details shutdown() requests an idempotent stop. A module may need later
    ///          process() calls before is_stopped() becomes true.
    class ILifecycleModule {
    public:
        virtual ~ILifecycleModule() noexcept = default;

        /// \brief Advances normal work or an in-progress graceful shutdown.
        virtual void process() = 0;

        /// \brief Requests an idempotent graceful shutdown.
        virtual void shutdown() noexcept = 0;

        /// \brief Returns true after all module-owned work and cleanup finished.
        [[nodiscard]] virtual bool is_stopped() const noexcept = 0;
    };

} // namespace optionx::lifecycle

#endif // OPTIONX_HEADER_LIFECYCLE_ILIFECYCLE_MODULE_HPP_INCLUDED
