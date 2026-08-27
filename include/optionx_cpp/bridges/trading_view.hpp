#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TRADING_VIEW_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TRADING_VIEW_HPP_INCLUDED

/// \file trading_view.hpp
/// \brief Includes TradingView bridge headers.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

#include "BaseBridge.hpp"
#include "utils/unicode_case.hpp"

#include <server_http.hpp>

#include "detail/BridgeTradeSignalValidation.hpp"
#include "trading_view/TradingViewExtensionBridgeConfig.hpp"
#include "trading_view/detail/TradingViewExtensionProtocol.hpp"
#include "trading_view/TradingViewExtensionBridge.hpp"

#endif // OPTIONX_HEADER_BRIDGES_TRADING_VIEW_HPP_INCLUDED
