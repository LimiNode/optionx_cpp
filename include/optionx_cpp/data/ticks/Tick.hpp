#pragma once
#ifndef OPTIONX_HEADER_DATA_TICKS_TICK_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TICKS_TICK_HPP_INCLUDED

/// \file Tick.hpp
/// \brief Defines the Tick structure used for representing market data.

#include <cstdint>

namespace optionx {

    /// \struct Tick
    /// \brief Represents a market tick with quote, trade, and reference fields.
    struct Tick {
        double ask;              ///< Ask price
        double bid;              ///< Bid price
        double last;             ///< Last traded price, when the feed provides trade ticks.
        double reference;        ///< Provider price with unknown bid/ask/last semantics.
        double volume;           ///< Trade volume (can store both whole units and high precision)
        std::uint64_t time_ms;   ///< Tick timestamp in milliseconds.
        std::uint64_t received_ms; ///< Time when tick was received from the server.
        std::uint32_t flags;     ///< Tick update and market-data flags.

        /// \brief Default constructor that initializes all fields to zero or equivalent values
        Tick()
            : ask(0.0), bid(0.0), last(0.0), reference(0.0), volume(0.0),
              time_ms(0), received_ms(0), flags(0) {}

        /// \brief Constructor to initialize all fields
        /// \param a Ask price
        /// \param b Bid price
        /// \param v Trade volume in whole units
        /// \param ts Tick timestamp in milliseconds
        /// \param rt Time when tick was received from the server
        /// \param f Flags representing tick characteristics
        Tick(double a, double b, double v,
            std::uint64_t ts, std::uint64_t rt, std::uint32_t f)
            : ask(a), bid(b), last(0.0), reference(0.0), volume(v),
              time_ms(ts), received_ms(rt), flags(f) {}

        /// \brief Constructor to initialize quote, trade, and metadata fields.
        /// \param a Ask price.
        /// \param b Bid price.
        /// \param l Last traded price.
        /// \param v Trade volume in whole units.
        /// \param ts Tick timestamp in milliseconds.
        /// \param rt Time when tick was received from the server.
        /// \param f Flags representing tick characteristics.
        Tick(double a, double b, double l, double v,
            std::uint64_t ts, std::uint64_t rt, std::uint32_t f)
            : ask(a), bid(b), last(l), reference(0.0), volume(v),
              time_ms(ts), received_ms(rt), flags(f) {}

        /// \brief Selects a representative price from the available tick fields.
        /// \return The quote midpoint, trade/one-sided price, or neutral reference.
        double mid_price() const {
            if (ask != 0.0 && bid != 0.0) return (ask + bid) / 2.0;
            if (last != 0.0) return last;
            if (ask != 0.0) return ask;
            if (bid != 0.0) return bid;
            return reference;
        }

        /// \brief Returns the representative price rounded to symbol precision.
        /// \param price_digits Number of decimal places for price rounding.
        /// \return The value returned by mid_price() rounded to price_digits.
        double mid_price(std::uint32_t price_digits) const {
            return utils::normalize_double(mid_price(), price_digits);
        }

        /// \brief Sets a specific flag in the tick's flags.
        /// \param flag The flag to set (from TickUpdateFlags).
        void set_flag(TickUpdateFlags flag) {
            flags |= static_cast<std::uint32_t>(flag);
        }

        /// \brief Sets or clears a tick update flag.
        /// \param flag Tick update flag to update.
        /// \param value Whether the flag should be set.
        void set_flag(TickUpdateFlags flag, bool value) {
            flags = value
                ? flags | static_cast<std::uint32_t>(flag)
                : flags & ~static_cast<std::uint32_t>(flag);
        }

        /// \brief Checks if a specific flag is set in the tick's flags.
        /// \param flag The flag to check (from TickUpdateFlags).
        /// \return True if the flag is set, otherwise false.
        bool has_flag(TickUpdateFlags flag) const {
            return (flags & static_cast<std::uint32_t>(flag)) != 0;
        }

        /// \brief Sets or clears a market-data payload flag.
        /// \param flag Market-data flag to update.
        /// \param value Whether the flag should be set.
        void set_flag(MarketDataFlags flag, bool value = true) noexcept {
            set_flag_in_place(flags, flag, value);
        }

        /// \brief Checks whether a market-data payload flag is set.
        /// \param flag Market-data flag to check.
        /// \return True if the flag is present.
        [[nodiscard]] bool has_flag(MarketDataFlags flag) const noexcept {
            return optionx::has_flag(flags, flag);
        }

    };

} // namespace optionx

#endif // OPTIONX_HEADER_DATA_TICKS_TICK_HPP_INCLUDED
