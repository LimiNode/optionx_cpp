#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_BOT_BINARY_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_BOT_BINARY_HPP_INCLUDED

/// \file bot_binary.hpp
/// \brief Includes BotBinary/BinaryBot compatibility bridge and helpers.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <server_http.hpp>

#include <optionx_cpp/data/trading.hpp>
#include <optionx_cpp/utils/metatrader_paths.hpp>

#include "BaseBridge.hpp"
#include "detail/BridgeTradeSignalValidation.hpp"
#include "bot_binary/BotBinaryBridgeConfig.hpp"
#include "bot_binary/detail/BotBinaryProtocol.hpp"
#include "bot_binary/BotBinaryBridge.hpp"

#endif // OPTIONX_HEADER_BRIDGES_BOT_BINARY_HPP_INCLUDED
