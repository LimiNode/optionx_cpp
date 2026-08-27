#pragma once
#ifndef OPTIONX_HEADER_DATA_TRADING_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TRADING_HPP_INCLUDED

/// \file trading.hpp
/// \brief Includes core trading-related components.
///
/// This header file provides access to essential trading-related classes 
/// and enumerations, including trade request and result structures, 
/// trade signals, and decision-making parameters.
/// \note Headers from the trading/ directory are internal components and are
/// intended to be included through this umbrella header.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <optionx_cpp/utils/enum_utils.hpp>
#include <optionx_cpp/utils/string_utils.hpp>

#include "trading/types.hpp"
#include "trading/enums.hpp"
#include "trading/SpreadPack.hpp"
#include "trading/TradeResult.hpp"
#include "trading/TradeResultQuery.hpp"
#include "trading/TradeRecordTimeRange.hpp"
#include "trading/TradeTimeZone.hpp"
#include "trading/TradeHistoryRequest.hpp"
#include "trading/TradeRequest.hpp"
#include "trading/IMoneyManagementParams.hpp"
#include "trading/ITradeDecisionParams.hpp"
#include "trading/TradeSignal.hpp"
#include "trading/SignalRecord.hpp"
#include "trading/TradeRecord.hpp"
#include "trading/TradeRecordFilter.hpp"
#include "trading/TradeRecordQuery.hpp"
#include "trading/TradeHistoryResult.hpp"
#include "trading/trade_state_traits.hpp"
#include "trading/TradeStats.hpp"

#endif // OPTIONX_HEADER_DATA_TRADING_HPP_INCLUDED
