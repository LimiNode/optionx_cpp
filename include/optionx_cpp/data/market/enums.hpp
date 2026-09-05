#pragma once
#ifndef OPTIONX_HEADER_DATA_MARKET_ENUMS_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_MARKET_ENUMS_HPP_INCLUDED

/// \file enums.hpp
/// \brief Defines shared market-data payload flags.

#include <cstdint>
#include <string>

namespace optionx {

    /// \enum MarketDataFlags
    /// \brief Flags describing origin, delivery mode, and state of market data.
    enum class MarketDataFlags : std::uint32_t {
        NONE       = 0,       ///< No market-data flags are set.
        LIVE_SOURCE = 1u << 16,///< Payload came from a live stream or polling source.
        HISTORICAL = 1u << 17,///< Payload came from a history request.
        BACKFILL   = 1u << 18,///< Payload was loaded to fill a stream gap.
        INCOMPLETE = 1u << 19,///< Bar payload is still forming.
        FINALIZED  = 1u << 20,///< Payload is complete and will not be updated.
        INITIALIZED = 1u << 21,///< Payload has enough fields to be consumed.
        REALTIME   = 1u << 22,///< Live-source payload delivered from the current live edge.
        CATCHUP    = 1u << 23 ///< Live-source payload replayed from a continuity backlog.
    };

    /// \enum MarketPriceType
    /// \brief Price stream represented by a market-data payload.
    enum class MarketPriceType : std::uint8_t {
        UNKNOWN = 0, ///< Price type is not specified.
        BID,         ///< Bid price.
        ASK,         ///< Ask price.
        MID,         ///< Bid/ask midpoint.
        LAST         ///< Last traded price.
    };

    /// \enum MarketDataUpdateSource
    /// \brief Transport-level source that produced a market-data update event.
    enum class MarketDataUpdateSource : std::uint8_t {
        UNKNOWN = 0, ///< Source is not specified.
        POLLING,     ///< Periodic snapshot/polling source.
        WEBSOCKET    ///< Websocket streaming source.
    };

    /// \brief Converts MarketPriceType to its string representation.
    inline const char* to_str(MarketPriceType value) noexcept {
        switch (value) {
        case MarketPriceType::BID:
            return "BID";
        case MarketPriceType::ASK:
            return "ASK";
        case MarketPriceType::MID:
            return "MID";
        case MarketPriceType::LAST:
            return "LAST";
        case MarketPriceType::UNKNOWN:
        default:
            return "UNKNOWN";
        }
    }

    /// \brief Converts MarketDataUpdateSource to its string representation.
    inline const char* to_str(MarketDataUpdateSource value) noexcept {
        switch (value) {
        case MarketDataUpdateSource::POLLING:
            return "POLLING";
        case MarketDataUpdateSource::WEBSOCKET:
            return "WEBSOCKET";
        case MarketDataUpdateSource::UNKNOWN:
        default:
            return "UNKNOWN";
        }
    }

    /// \brief Bit offset used to encode MarketPriceType inside payload flags.
    inline constexpr std::uint32_t MARKET_PRICE_TYPE_SHIFT = 24;

    /// \brief Bit mask reserved for the encoded MarketPriceType value.
    inline constexpr std::uint32_t MARKET_PRICE_TYPE_MASK = 0xFu << MARKET_PRICE_TYPE_SHIFT;

    /// \brief Checks whether market-data flags contain a specific flag.
    [[nodiscard]] inline bool has_flag(std::uint32_t flags, MarketDataFlags flag) noexcept {
        return (flags & static_cast<std::uint32_t>(flag)) != 0U;
    }

    /// \brief Returns a flags value with the given market-data flag set or cleared.
    [[nodiscard]] inline std::uint32_t set_flag(
            std::uint32_t flags,
            MarketDataFlags flag,
            bool value = true) noexcept {
        if (value) {
            return flags | static_cast<std::uint32_t>(flag);
        }
        return flags & ~static_cast<std::uint32_t>(flag);
    }

    /// \brief Sets or clears a market-data flag in-place.
    inline void set_flag_in_place(
            std::uint32_t& flags,
            MarketDataFlags flag,
            bool value = true) noexcept {
        flags = set_flag(flags, flag, value);
    }

    /// \brief Marks a payload as live-source data and normalizes its delivery mode.
    /// \param flags Payload flags to update.
    /// \param catchup Whether the payload is being replayed from a continuity backlog.
    ///
    /// This preserves bar lifecycle flags such as INCOMPLETE and FINALIZED while
    /// making origin and delivery mode mutually consistent.
    inline void mark_live_payload(
            std::uint32_t& flags,
            bool catchup = false) noexcept {
        set_flag_in_place(flags, MarketDataFlags::LIVE_SOURCE);
        set_flag_in_place(flags, MarketDataFlags::HISTORICAL, false);
        set_flag_in_place(flags, MarketDataFlags::BACKFILL, false);
        set_flag_in_place(flags, MarketDataFlags::REALTIME, !catchup);
        set_flag_in_place(flags, MarketDataFlags::CATCHUP, catchup);
    }

    /// \brief Marks a payload as data returned by a history request.
    /// \param flags Payload flags to update.
    /// \param backfill Whether the history request repairs a stream gap.
    inline void mark_historical_payload(
            std::uint32_t& flags,
            bool backfill = false) noexcept {
        set_flag_in_place(flags, MarketDataFlags::LIVE_SOURCE, false);
        set_flag_in_place(flags, MarketDataFlags::HISTORICAL);
        set_flag_in_place(flags, MarketDataFlags::BACKFILL, backfill);
        set_flag_in_place(flags, MarketDataFlags::REALTIME, false);
        set_flag_in_place(flags, MarketDataFlags::CATCHUP, false);
    }

    /// \brief Checks the origin and delivery-mode invariants of market data flags.
    /// \param flags Bitmask containing MarketDataFlags and an encoded price type.
    /// \return True when origin, delivery, and backfill flags are consistent.
    [[nodiscard]] inline bool market_data_flags_valid(
            std::uint32_t flags) noexcept {
        const bool live_source = has_flag(flags, MarketDataFlags::LIVE_SOURCE);
        const bool historical = has_flag(flags, MarketDataFlags::HISTORICAL);
        const bool backfill = has_flag(flags, MarketDataFlags::BACKFILL);
        const bool realtime = has_flag(flags, MarketDataFlags::REALTIME);
        const bool catchup = has_flag(flags, MarketDataFlags::CATCHUP);
        return !(live_source && historical) &&
            !(realtime && catchup) &&
            (!realtime || live_source) &&
            (!catchup || live_source) &&
            (!backfill || historical);
    }

    /// \brief Reads the encoded market price type from a flags value.
    [[nodiscard]] inline MarketPriceType market_price_type(std::uint32_t flags) noexcept {
        return static_cast<MarketPriceType>(
            (flags & MARKET_PRICE_TYPE_MASK) >> MARKET_PRICE_TYPE_SHIFT);
    }

    /// \brief Returns a flags value with an encoded market price type.
    [[nodiscard]] inline std::uint32_t set_market_price_type(
            std::uint32_t flags,
            MarketPriceType type) noexcept {
        flags &= ~MARKET_PRICE_TYPE_MASK;
        flags |= (static_cast<std::uint32_t>(type) << MARKET_PRICE_TYPE_SHIFT) &
                 MARKET_PRICE_TYPE_MASK;
        return flags;
    }

    /// \brief Encodes a market price type in-place.
    inline void set_market_price_type_in_place(
            std::uint32_t& flags,
            MarketPriceType type) noexcept {
        flags = set_market_price_type(flags, type);
    }

    /// \brief Formats market-data origin/completeness flags.
    /// \param flags Bitmask containing MarketDataFlags and an encoded price type.
    /// \return Stable pipe-separated flag names, or NONE when no known flag is set.
    inline std::string market_data_flags_to_string(std::uint32_t flags) {
        std::string result;
        const auto append = [&result](const char* name) {
            if (!result.empty()) result += '|';
            result += name;
        };

        if (has_flag(flags, MarketDataFlags::LIVE_SOURCE)) append("LIVE_SOURCE");
        if (has_flag(flags, MarketDataFlags::HISTORICAL)) append("HISTORICAL");
        if (has_flag(flags, MarketDataFlags::BACKFILL)) append("BACKFILL");
        if (has_flag(flags, MarketDataFlags::REALTIME)) append("REALTIME");
        if (has_flag(flags, MarketDataFlags::CATCHUP)) append("CATCHUP");
        if (has_flag(flags, MarketDataFlags::INCOMPLETE)) append("INCOMPLETE");
        if (has_flag(flags, MarketDataFlags::FINALIZED)) append("FINALIZED");
        if (has_flag(flags, MarketDataFlags::INITIALIZED)) append("INITIALIZED");

        return result.empty() ? std::string("NONE") : result;
    }

} // namespace optionx

#endif // OPTIONX_HEADER_DATA_MARKET_ENUMS_HPP_INCLUDED
