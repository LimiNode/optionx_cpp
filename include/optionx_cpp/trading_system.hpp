#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_HPP_INCLUDED

/// \file trading_system.hpp
/// \brief Public aggregate for the complete trading-system domain.

#include <optionx_cpp/data/trading.hpp>
#include "platforms/common/BaseTradingApi.hpp"

#include "trading_system/Identity.hpp"
#include "trading_system/TradeSeries.hpp"
#include "trading_system/TradeIntent.hpp"
#include "trading_system/PolicyTypes.hpp"
#include "trading_system/DecisionContext.hpp"
#include "trading_system/Filters.hpp"
#include "trading_system/Sizing.hpp"
#include "trading_system/Settlement.hpp"
#include "trading_system/ExecutionGroupAggregator.hpp"
#include "trading_system/SeriesProgression.hpp"
#include "trading_system/RiskAdmission.hpp"
#if defined(small)
#pragma push_macro("small")
#undef small
#define OPTIONX_RESTORE_SMALL_MACRO
#endif
#include "trading_system/TradingSystemStorage.hpp"
#if defined(OPTIONX_RESTORE_SMALL_MACRO)
#pragma pop_macro("small")
#undef OPTIONX_RESTORE_SMALL_MACRO
#endif
#include "trading_system/ExecutionRecovery.hpp"
#include "trading_system/detail/PlatformDtoMapper.hpp"
#include "trading_system/detail/PlatformReceipts.hpp"
#include "trading_system/detail/PlatformObservationHandoff.hpp"

#endif // OPTIONX_HEADER_TRADING_SYSTEM_HPP_INCLUDED
