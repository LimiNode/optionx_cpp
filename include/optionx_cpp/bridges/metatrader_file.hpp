#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_METATRADER_FILE_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_METATRADER_FILE_HPP_INCLUDED

/// \file metatrader_file.hpp
/// \brief Includes MetaTrader file-transport bridge headers.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <optionx_cpp/data/bridge.hpp>
#include <optionx_cpp/data/trading.hpp>
#include <optionx_cpp/utils/Base36.hpp>
#include <optionx_cpp/utils/metatrader_paths.hpp>
#include <optionx_cpp/utils/tasks.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "BaseBridge.hpp"
#include "metatrader_file/detail/MetaTraderFilePathUtils.hpp"
#include "metatrader_file/detail/MetaTraderFileOperationKey.hpp"
#include "metatrader_file/MetaTraderFileBridgeConfig.hpp"
#include "metatrader_file/detail/MetaTraderFileProtocol.hpp"
#include "metatrader_file/detail/MetaTraderFileIdempotencyStore.hpp"
#include "protocol_v1/detail/BridgeProtocolCanonicalization.hpp"
#include "protocol_v1/detail/BridgeProtocolTradeCommand.hpp"
#include "metatrader_file/detail/MetaTraderFileBridgeUtils.hpp"
#include "metatrader_file/MetaTraderFileCommandWriter.hpp"
#include "metatrader_file/MetaTraderFileBridge.hpp"

#endif // OPTIONX_HEADER_BRIDGES_METATRADER_FILE_HPP_INCLUDED
