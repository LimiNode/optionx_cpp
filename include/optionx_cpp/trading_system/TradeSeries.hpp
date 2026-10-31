#pragma once
#ifndef OPTIONX_HEADER_TRADING_SYSTEM_TRADE_SERIES_HPP_INCLUDED
#define OPTIONX_HEADER_TRADING_SYSTEM_TRADE_SERIES_HPP_INCLUDED

/// \file TradeSeries.hpp
/// \brief Defines the identity and revision envelope for a trade series.

#include "Identity.hpp"

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>

namespace optionx::trading_system {

/// \class TradeSeries
/// \brief Pure identity state for a future series progression component.
///
/// This type deliberately does not implement progression, settlement, risk,
/// or broker calls. Those concerns belong to later trading-system batches.
struct TradeSeries {
    SeriesId series_id = 0;
    SignalId origin_signal_id = 0;
    std::uint32_t next_step = 0;
    std::uint64_t state_revision = 0;

    bool valid() const noexcept {
        return series_id != 0;
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

    TradeSeries clone() const {
        return *this;
    }
};

inline void to_json(nlohmann::json& json, const TradeSeries& value) {
    json = nlohmann::json{
        {"series_id", value.series_id},
        {"origin_signal_id", value.origin_signal_id},
        {"next_step", value.next_step},
        {"state_revision", value.state_revision}
    };
}

inline void from_json(const nlohmann::json& json, TradeSeries& value) {
    value.series_id = json.value("series_id", SeriesId{0});
    value.origin_signal_id = json.value("origin_signal_id", SignalId{0});
    value.next_step = json.value("next_step", std::uint32_t{0});
    value.state_revision = json.value("state_revision", std::uint64_t{0});
}

} // namespace optionx::trading_system

#endif // OPTIONX_HEADER_TRADING_SYSTEM_TRADE_SERIES_HPP_INCLUDED
