#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_HPP_INCLUDED

/// \file market_data.hpp
/// \brief Includes public market-data provider contracts.
/// \note Headers under market_data subdirectories are internal components and
/// intended to be included through this umbrella header.

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lifecycle.hpp"
#include "data.hpp"
#include "market_data/enums.hpp"
#include "market_data/MarketDataSubscription.hpp"
#include "market_data/MarketDataBatch.hpp"
#include "market_data/BaseMarketDataProvider.hpp"
#include "market_data/MarketDataContinuityService.hpp"
#include "market_data/IMarketDataSubscriber.hpp"
#include "market_data/MarketDataHub.hpp"
#include "market_data/MarketDataRouter.hpp"
#include "market_data/MarketDataSubscriberBase.hpp"

#endif // OPTIONX_HEADER_MARKET_DATA_HPP_INCLUDED
