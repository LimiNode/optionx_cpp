#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_HPP_INCLUDED

/// \file platforms.hpp
/// \brief Includes platform-related headers.
/// \note Headers under platforms subdirectories are internal components and are
/// intended to be included through this umbrella header.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config.hpp"
#include "utils.hpp"
#include "lifecycle.hpp"
#include "data.hpp"
#include "market_data.hpp"
#include "storages.hpp"
#include "components.hpp"
#include "platforms/common/ApiResult.hpp"
#include "platforms/common/BaseTradingApi.hpp"
#include "platforms/common/BaseTradingPlatform.hpp"
#include "platforms/IntradeBarPlatform.hpp"
//#include "platforms/TradeUpPlatform.hpp"

#endif // OPTIONX_HEADER_PLATFORMS_HPP_INCLUDED
