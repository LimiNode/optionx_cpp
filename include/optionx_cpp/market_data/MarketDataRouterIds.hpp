#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_IDS_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_IDS_HPP_INCLUDED

/// \file MarketDataRouterIds.hpp
/// \brief Defines the strong identifiers used by MarketDataRouter.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace optionx::market_data {

    namespace detail {
        class MarketDataRouterState;
    } // namespace detail

    /// \class RoutedSubscriptionId
    /// \brief Strong Router-local identifier of a logical market-data subscription.
    class RoutedSubscriptionId {
    public:
        /// \brief Constructs an invalid identifier.
        constexpr RoutedSubscriptionId() noexcept = default;

        /// \brief Returns true when the Router assigned an identifier.
        [[nodiscard]] constexpr bool valid() const noexcept {
            return m_value != 0;
        }

        /// \brief Allows identifiers to be checked directly in conditions.
        constexpr explicit operator bool() const noexcept {
            return valid();
        }

        /// \brief Returns the numeric value for logging and diagnostics.
        [[nodiscard]] constexpr std::uint64_t value() const noexcept {
            return m_value;
        }

        friend constexpr bool operator==(
                RoutedSubscriptionId lhs,
                RoutedSubscriptionId rhs) noexcept {
            return lhs.m_value == rhs.m_value;
        }

        friend constexpr bool operator!=(
                RoutedSubscriptionId lhs,
                RoutedSubscriptionId rhs) noexcept {
            return !(lhs == rhs);
        }

    private:
        std::uint64_t m_value = 0;

        explicit constexpr RoutedSubscriptionId(std::uint64_t value) noexcept
                : m_value(value) {}

        friend class detail::MarketDataRouterState;
    };

    /// \struct RoutedSubscriptionIdHash
    /// \brief Hashes a routed subscription identifier for unordered containers.
    struct RoutedSubscriptionIdHash {
        [[nodiscard]] std::size_t operator()(RoutedSubscriptionId id) const noexcept {
            return std::hash<std::uint64_t>{}(id.value());
        }
    };

    /// \class MarketDataProviderId
    /// \brief Stable application-assigned identifier of a registered provider.
    class MarketDataProviderId {
    public:
        /// \brief Constructs an invalid provider identifier.
        constexpr MarketDataProviderId() noexcept = default;

        /// \brief Constructs a stable provider identifier from application data.
        explicit constexpr MarketDataProviderId(std::uint64_t value) noexcept
                : m_value(value) {}

        /// \brief Returns true when the identifier can select a provider.
        [[nodiscard]] constexpr bool valid() const noexcept {
            return m_value != 0;
        }

        /// \brief Allows identifiers to be checked directly in conditions.
        constexpr explicit operator bool() const noexcept {
            return valid();
        }

        /// \brief Returns the application-assigned numeric value.
        [[nodiscard]] constexpr std::uint64_t value() const noexcept {
            return m_value;
        }

        friend constexpr bool operator==(
                MarketDataProviderId lhs,
                MarketDataProviderId rhs) noexcept {
            return lhs.m_value == rhs.m_value;
        }

        friend constexpr bool operator!=(
                MarketDataProviderId lhs,
                MarketDataProviderId rhs) noexcept {
            return !(lhs == rhs);
        }

    private:
        std::uint64_t m_value = 0;
    };

    /// \struct MarketDataProviderIdHash
    /// \brief Hashes an application-assigned provider identifier.
    struct MarketDataProviderIdHash {
        [[nodiscard]] std::size_t operator()(MarketDataProviderId id) const noexcept {
            return std::hash<std::uint64_t>{}(id.value());
        }
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_IDS_HPP_INCLUDED
