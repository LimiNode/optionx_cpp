#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_SUBSCRIPTION_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_SUBSCRIPTION_HPP_INCLUDED

/// \file MarketDataRouterSubscription.hpp
/// \brief Defines the move-only RAII handle returned by MarketDataRouter.

#include <memory>
#include <mutex>
#include <utility>

#include "BaseMarketDataProvider.hpp"
#include "MarketDataRouterIds.hpp"

namespace optionx::market_data {

    class MarketDataRouter;

    namespace detail {
        class MarketDataRouterState;
        struct MarketDataRouterSubscriptionControl;
    } // namespace detail

    /// \class MarketDataRouterSubscription
    /// \brief Move-only RAII owner of one subscription created by MarketDataRouter.
    /// \details Destroying or resetting the handle requests unsubscription. The
    ///          handle is created before an asynchronous provider necessarily
    ///          accepts the subscription, so valid() can be true while pending()
    ///          is also true. provider_subscription() becomes valid after the
    ///          provider reports SUBSCRIBED.
    class MarketDataRouterSubscription {
    public:
        /// \brief Default-constructs an empty handle.
        MarketDataRouterSubscription() = default;

        /// \brief Copy construction is disabled because ownership is unique.
        MarketDataRouterSubscription(const MarketDataRouterSubscription&) = delete;
        /// \brief Copy assignment is disabled because ownership is unique.
        MarketDataRouterSubscription& operator=(const MarketDataRouterSubscription&) = delete;

        /// \brief Transfers subscription ownership.
        MarketDataRouterSubscription(MarketDataRouterSubscription&& other) noexcept
                : m_control(std::move(other.m_control)) {}

        /// \brief Releases the current subscription and transfers ownership.
        MarketDataRouterSubscription& operator=(MarketDataRouterSubscription&& other) noexcept;

        /// \brief Requests unsubscription for an owned subscription.
        ~MarketDataRouterSubscription() {
            reset();
        }

        /// \brief Returns the stable router-local subscription ID.
        [[nodiscard]] RoutedSubscriptionId router_id() const noexcept;

        /// \brief Returns the provider-assigned subscription descriptor, if accepted.
        [[nodiscard]] MarketDataSubscriptionHandle provider_subscription() const;

        /// \brief Returns the stable registered provider ID used to create this route.
        /// \details Direct provider-reference subscriptions return an invalid ID.
        [[nodiscard]] MarketDataProviderId registered_provider_id() const;

        /// \brief Returns true while this object owns a pending or active route.
        [[nodiscard]] bool valid() const;

        /// \brief Returns true after the provider accepted the subscription.
        [[nodiscard]] bool active() const;

        /// \brief Returns true while provider acceptance is still pending.
        [[nodiscard]] bool pending() const {
            return valid() && !active();
        }

        /// \brief Allows handles to be used in boolean contexts.
        explicit operator bool() const {
            return valid();
        }

        /// \brief Explicitly releases the route and requests provider unsubscription.
        /// \param callback Optional callback receiving the provider unsubscribe result.
        /// \return True when an unsubscribe or pending cancellation was accepted.
        /// \details The logical route is released even when physical provider cleanup
        ///          fails. MarketDataRouter retains failed cleanup ownership for retry.
        bool unsubscribe(BaseMarketDataProvider::subscription_callback_t callback = {});

        /// \brief Releases the route without an unsubscribe completion callback.
        void reset() noexcept;

    private:
        std::shared_ptr<detail::MarketDataRouterSubscriptionControl> m_control;

        explicit MarketDataRouterSubscription(
                std::shared_ptr<detail::MarketDataRouterSubscriptionControl> control)
                : m_control(std::move(control)) {}

        friend class MarketDataRouter;
        friend class detail::MarketDataRouterState;
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_ROUTER_SUBSCRIPTION_HPP_INCLUDED
