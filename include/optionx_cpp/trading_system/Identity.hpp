#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_IDENTITY_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_IDENTITY_HPP_INCLUDED

/// \file Identity.hpp
/// \brief Defines stable identities and cardinality policies for trading-system contracts.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace optionx::trading_system {

using IntentId = std::uint64_t;
using SeriesId = std::uint64_t;
using ExecutionGroupId = std::uint64_t;
using SettlementId = std::uint64_t;

enum class IntentStatus {
    PROPOSED,
    REJECTED,
    ADMITTED,
    SUBMITTING,
    EXECUTION_UNKNOWN,
    EXECUTED,
    SETTLED,
    CANCELLED
};

enum class ExecutionCardinality {
    ZERO_OR_ONE,
    EXACTLY_ONE,
    MULTIPLE,
    SPLIT_GROUP
};

inline const char* to_string(IntentStatus value) noexcept {
    switch (value) {
        case IntentStatus::PROPOSED: return "PROPOSED";
        case IntentStatus::REJECTED: return "REJECTED";
        case IntentStatus::ADMITTED: return "ADMITTED";
        case IntentStatus::SUBMITTING: return "SUBMITTING";
        case IntentStatus::EXECUTION_UNKNOWN: return "EXECUTION_UNKNOWN";
        case IntentStatus::EXECUTED: return "EXECUTED";
        case IntentStatus::SETTLED: return "SETTLED";
        case IntentStatus::CANCELLED: return "CANCELLED";
    }
    return "UNKNOWN";
}

inline const char* to_string(ExecutionCardinality value) noexcept {
    switch (value) {
        case ExecutionCardinality::ZERO_OR_ONE: return "ZERO_OR_ONE";
        case ExecutionCardinality::EXACTLY_ONE: return "EXACTLY_ONE";
        case ExecutionCardinality::MULTIPLE: return "MULTIPLE";
        case ExecutionCardinality::SPLIT_GROUP: return "SPLIT_GROUP";
    }
    return "UNKNOWN";
}

inline IntentStatus intent_status_from_string(const std::string& value) {
    if (value == "PROPOSED") return IntentStatus::PROPOSED;
    if (value == "REJECTED") return IntentStatus::REJECTED;
    if (value == "ADMITTED") return IntentStatus::ADMITTED;
    if (value == "SUBMITTING") return IntentStatus::SUBMITTING;
    if (value == "EXECUTION_UNKNOWN") return IntentStatus::EXECUTION_UNKNOWN;
    if (value == "EXECUTED") return IntentStatus::EXECUTED;
    if (value == "SETTLED") return IntentStatus::SETTLED;
    if (value == "CANCELLED") return IntentStatus::CANCELLED;
    throw std::invalid_argument("Unknown trading-system intent status: " + value);
}

inline ExecutionCardinality execution_cardinality_from_string(const std::string& value) {
    if (value == "ZERO_OR_ONE") return ExecutionCardinality::ZERO_OR_ONE;
    if (value == "EXACTLY_ONE") return ExecutionCardinality::EXACTLY_ONE;
    if (value == "MULTIPLE") return ExecutionCardinality::MULTIPLE;
    if (value == "SPLIT_GROUP") return ExecutionCardinality::SPLIT_GROUP;
    throw std::invalid_argument("Unknown trading-system execution cardinality: " + value);
}

inline void to_json(nlohmann::json& json, IntentStatus value) {
    json = to_string(value);
}

inline void from_json(const nlohmann::json& json, IntentStatus& value) {
    value = intent_status_from_string(json.get<std::string>());
}

inline void to_json(nlohmann::json& json, ExecutionCardinality value) {
    json = to_string(value);
}

inline void from_json(const nlohmann::json& json, ExecutionCardinality& value) {
    value = execution_cardinality_from_string(json.get<std::string>());
}

/// \brief Identifies a physical execution group without owning execution state.
struct ExecutionGroupIdentity {
    ExecutionGroupId group_id = 0;
    std::uint32_t expected_members = 0;

    /// \brief Checks whether this identity is compatible with a cardinality policy.
    bool valid_for(ExecutionCardinality cardinality) const noexcept {
        switch (cardinality) {
            case ExecutionCardinality::ZERO_OR_ONE:
                return group_id == 0 && expected_members <= 1;
            case ExecutionCardinality::EXACTLY_ONE:
                return expected_members == 1;
            case ExecutionCardinality::MULTIPLE:
                return group_id != 0 && expected_members >= 2;
            case ExecutionCardinality::SPLIT_GROUP:
                return group_id != 0 && expected_members >= 2;
        }
        return false;
    }
};

inline void to_json(nlohmann::json& json, const ExecutionGroupIdentity& value) {
    json = nlohmann::json{
        {"group_id", value.group_id},
        {"expected_members", value.expected_members}
    };
}

inline void from_json(const nlohmann::json& json, ExecutionGroupIdentity& value) {
    value.group_id = json.value("group_id", ExecutionGroupId{0});
    value.expected_members = json.value("expected_members", std::uint32_t{0});
}

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_IDENTITY_HPP_INCLUDED
