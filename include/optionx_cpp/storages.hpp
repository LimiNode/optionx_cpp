#pragma once
#ifndef OPTIONX_HEADER_STORAGES_HPP_INCLUDED
#define OPTIONX_HEADER_STORAGES_HPP_INCLUDED

#include "crypto.hpp"
#include "utils.hpp"
#include "storages/common.hpp"
#if defined(small)
#pragma push_macro("small")
#undef small
#define OPTIONX_RESTORE_SMALL_MACRO
#endif
#include "storages/ServiceSessionDB.hpp"
#include "storages/TradeRecordDB.hpp"
#include "storages/SignalRecordDB.hpp"
#if defined(OPTIONX_RESTORE_SMALL_MACRO)
#pragma pop_macro("small")
#undef OPTIONX_RESTORE_SMALL_MACRO
#endif

#endif // OPTIONX_HEADER_STORAGES_HPP_INCLUDED
