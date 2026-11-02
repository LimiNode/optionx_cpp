#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_TRADE_INTENT_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_TRADE_INTENT_HPP_INCLUDED

/// \file TradeIntent.hpp
/// \brief Defines the pure intent envelope used before broker submission.

#include "Identity.hpp"

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace optionx::trading_system {

/// \class TradeIntent
/// \brief Identifies one proposed logical trade without executing it.
///
/// The embedded TradeRequest remains the existing broker-compatible request
/// DTO. This envelope adds identity, cardinality, and revision data without
/// changing TradeRequest serialization or runtime callback ownership.
struct TradeIntent {
    IntentId intent_id = 0;
    SignalId origin_signal_id = 0;
    SeriesId series_id = 0;
    std::int32_t series_step = -1;
    ExecutionGroupIdentity execution_group;
    ExecutionCardinality cardinality = ExecutionCardinality::ZERO_OR_ONE;
    IntentStatus status = IntentStatus::PROPOSED;
    TradeRequest request;
    std::uint64_t state_revision = 0;
    std::string idempotency_key;

    /// \brief Validates identity and cardinality invariants only.
    bool valid() const noexcept {
        const bool standalone = series_id == 0 && series_step == -1;
        const bool series_bound = series_id != 0 && series_step >= 0;
        return intent_id != 0 &&
               (standalone || series_bound) &&
               !idempotency_key.empty() &&
               execution_group.valid_for(cardinality);
    }

    bool revision_matches(std::uint64_t expected_revision) const noexcept {
        return state_revision == expected_revision;
    }

    /// \brief Advances the local revision after an expected revision check.
    ///
    /// This is a sequential guard for one state owner. It is not thread-safe
    /// and does not provide a persistent compare-and-swap operation. Callers
    /// must serialize access to this DTO; durable state transitions belong to
    /// the owning runtime or persistence adapter.
    bool advance_revision(std::uint64_t expected_revision) noexcept {
        if (!revision_matches(expected_revision)) return false;
        if (state_revision == (std::numeric_limits<std::uint64_t>::max)()) return false;
        ++state_revision;
        return true;
    }

    TradeIntent clone() const {
        return *this;
    }
};

inline void to_json(nlohmann::json& json, const TradeIntent& value) {
    json = nlohmann::json{
        {"intent_id", value.intent_id},
        {"origin_signal_id", value.origin_signal_id},
        {"series_id", value.series_id},
        {"series_step", value.series_step},
        {"execution_group", value.execution_group},
        {"cardinality", value.cardinality},
        {"status", value.status},
        {"request", value.request},
        {"state_revision", value.state_revision},
        {"idempotency_key", value.idempotency_key}
    };
}

inline void from_json(const nlohmann::json& json, TradeIntent& value) {
    value.intent_id = json.value("intent_id", IntentId{0});
    value.origin_signal_id = json.value("origin_signal_id", SignalId{0});
    value.series_id = json.value("series_id", SeriesId{0});
    value.series_step = json.value("series_step", std::int32_t{-1});
    value.execution_group = json.value("execution_group", ExecutionGroupIdentity{});
    value.cardinality = json.value("cardinality", ExecutionCardinality::ZERO_OR_ONE);
    value.status = json.value("status", IntentStatus::PROPOSED);
    value.request = TradeRequest{};
    if (json.contains("request")) {
        json.at("request").get_to(value.request);
    }
    value.state_revision = json.value("state_revision", std::uint64_t{0});
    value.idempotency_key = json.value("idempotency_key", std::string{});
}

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_TRADE_INTENT_HPP_INCLUDED
