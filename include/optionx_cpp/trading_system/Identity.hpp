#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_IDENTITY_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_IDENTITY_HPP_INCLUDED

/// \file Identity.hpp
/// \brief Defines stable identities and cardinality policies for trading-system contracts.

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

namespace detail {

template <class E, std::size_t N>
using EnumTable = std::array<std::pair<E, std::string_view>, N>;

inline constexpr EnumTable<IntentStatus, 8> intent_status_table{{
    {IntentStatus::PROPOSED, "PROPOSED"},
    {IntentStatus::REJECTED, "REJECTED"},
    {IntentStatus::ADMITTED, "ADMITTED"},
    {IntentStatus::SUBMITTING, "SUBMITTING"},
    {IntentStatus::EXECUTION_UNKNOWN, "EXECUTION_UNKNOWN"},
    {IntentStatus::EXECUTED, "EXECUTED"},
    {IntentStatus::SETTLED, "SETTLED"},
    {IntentStatus::CANCELLED, "CANCELLED"}
}};

inline constexpr EnumTable<ExecutionCardinality, 4> execution_cardinality_table{{
    {ExecutionCardinality::ZERO_OR_ONE, "ZERO_OR_ONE"},
    {ExecutionCardinality::EXACTLY_ONE, "EXACTLY_ONE"},
    {ExecutionCardinality::MULTIPLE, "MULTIPLE"},
    {ExecutionCardinality::SPLIT_GROUP, "SPLIT_GROUP"}
}};

template <class E, std::size_t N>
constexpr std::string_view enum_to_string(
    E value,
    const EnumTable<E, N>& table) noexcept {
    for (const auto& entry : table) {
        if (entry.first == value) return entry.second;
    }
    return "UNKNOWN";
}

template <class E, std::size_t N>
bool enum_from_string(
    std::string_view text,
    const EnumTable<E, N>& table,
    E& result) noexcept {
    for (const auto& entry : table) {
        if (entry.second == text) {
            result = entry.first;
            return true;
        }
    }
    return false;
}

} // namespace detail

inline const char* to_string(IntentStatus value) noexcept {
    return detail::enum_to_string(value, detail::intent_status_table).data();
}

inline const char* to_string(ExecutionCardinality value) noexcept {
    return detail::enum_to_string(value, detail::execution_cardinality_table).data();
}

inline IntentStatus intent_status_from_string(const std::string& value) {
    IntentStatus result{};
    if (detail::enum_from_string(
            std::string_view(value), detail::intent_status_table, result)) {
        return result;
    }
    throw std::invalid_argument("Unknown trading-system intent status: " + value);
}

inline ExecutionCardinality execution_cardinality_from_string(
    const std::string& value) {
    ExecutionCardinality result{};
    if (detail::enum_from_string(
            std::string_view(value), detail::execution_cardinality_table, result)) {
        return result;
    }
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
    ///
    /// Batch 1 only validates the fixed identity shape. `MULTIPLE` means that
    /// several independent physical executions are allowed, while
    /// `SPLIT_GROUP` means that several physical executions share one logical
    /// group and remain pending until the group policy settles them. The
    /// execution-group runtime must define dynamic fan-out and final member
    /// accounting before either policy is used for admission.
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
