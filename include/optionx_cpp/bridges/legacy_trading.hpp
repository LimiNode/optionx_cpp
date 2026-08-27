#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_LEGACY_TRADING_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_LEGACY_TRADING_HPP_INCLUDED

/// \file legacy_trading.hpp
/// \brief Includes legacy trading bridge headers.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <logit_cpp/logit.hpp>
#include <nlohmann/json.hpp>
#include <time_shield.hpp>

#include "BaseBridge.hpp"
#include <optionx_cpp/utils/tasks.hpp>

#if defined(_WIN32)
#include <SimpleNamedPipe/NamedPipeServer.hpp>
#endif

#include "legacy_trading/LegacyTradingBridgeConfig.hpp"
#include "legacy_trading/detail/LegacyTradingProtocol.hpp"
#include "legacy_trading/LegacyTradingBridge.hpp"

#endif // OPTIONX_HEADER_BRIDGES_LEGACY_TRADING_HPP_INCLUDED
