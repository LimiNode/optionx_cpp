#pragma once
#ifndef OPTIONX_HEADER_DATA_EVENTS_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_EVENTS_HPP_INCLUDED

/// \file events.hpp
/// \brief Aggregated include for all core event types used across the OptionX system.
///
/// This header provides a single entry point to include all event definitions.
/// Include this file instead of individual event headers when working with multiple event types.


#include "events/AuthDataEvent.hpp"
#include "events/RestartAuthEvent.hpp"
#include "events/WebSocketAuthDataEvent.hpp"
#include "events/WebSocketAuthAppliedEvent.hpp"
#include "events/WebSocketRequestEvent.hpp"
#include "events/WebSocketResultEvent.hpp"
#include "events/AutoDomainSelectedEvent.hpp"
#include "events/AccountInfoUpdateEvent.hpp"
#include "events/TradingConditionUpdateEvent.hpp"
#include "events/BalanceRequestEvent.hpp"
#include "events/PriceUpdateEvent.hpp"
#include "events/ConnectRequestEvent.hpp"
#include "events/DisconnectRequestEvent.hpp"
#include "events/TradeRequestEvent.hpp"
#include "events/TradeTransactionEvent.hpp"
#include "events/TradeStatusEvent.hpp"
#include "events/OpenTradesEvent.hpp"
#include "events/OpenTradesSnapshotEvent.hpp"
#include "events/OpenTradesSnapshotAvailabilityEvent.hpp"
#include "events/OpenTradesSnapshotRefreshRequestEvent.hpp"

#endif // OPTIONX_HEADER_DATA_EVENTS_HPP_INCLUDED
