#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_PROTOCOL_V1_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_PROTOCOL_V1_HPP_INCLUDED

/// \file protocol_v1.hpp
/// \brief Includes Bridge Protocol v1 HTTP/WebSocket server headers.

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <server_http.hpp>
#include <server_ws.hpp>
#if defined(_WIN32)
#include <SimpleNamedPipe/NamedPipeServer.hpp>
#endif

#include <optionx_cpp/data/bridge.hpp>
#include <optionx_cpp/data/trading.hpp>
#include <optionx_cpp/utils/tasks.hpp>

#include "BaseBridge.hpp"
#include "metatrader_file.hpp"
#include "protocol_v1/BridgeProtocolNamedPipeConfig.hpp"
#include "protocol_v1/BridgeProtocolServerConfig.hpp"
#include "protocol_v1/detail/BridgeProtocolCanonicalization.hpp"
#include "protocol_v1/detail/BridgeProtocolTradeCommand.hpp"
#include "protocol_v1/detail/BridgeProtocolServerUtils.hpp"
#include "protocol_v1/BridgeProtocolNamedPipeBridge.hpp"
#include "protocol_v1/BridgeProtocolServerBridge.hpp"

#endif // OPTIONX_HEADER_BRIDGES_PROTOCOL_V1_HPP_INCLUDED
