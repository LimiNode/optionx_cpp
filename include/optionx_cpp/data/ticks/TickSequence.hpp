#pragma once
#ifndef OPTIONX_HEADER_DATA_TICKS_TICK_SEQUENCE_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TICKS_TICK_SEQUENCE_HPP_INCLUDED

/// \file TickSequence.hpp
/// \brief Defines the TickSequence structure for storing a sequence of tick data with metadata.

#include <cstdint>
#include <vector>
#include <string>
#include <utility>

namespace optionx {

    /// \struct TickSequence
    /// \brief Represents a sequence of tick data with metadata.
    struct TickSequence {
        std::vector<Tick> ticks;    ///< Sequence of tick data of the specified type.
        std::string symbol;         ///< Symbol associated with the tick sequence.
        std::string provider;       ///< Data provider associated with the tick sequence.
        std::string source;         ///< Provider-owned source used to obtain the sequence.
        std::uint32_t price_digits; ///< Number of decimal places for price.
        std::uint32_t volume_digits; ///< Number of decimal places for volume.

        /// \brief Default constructor initializing all fields to default values.
        TickSequence() : price_digits(0), volume_digits(0) {}

        /// \brief Constructor to initialize all fields.
        /// \param ts Sequence of tick data.
        /// \param s Symbol associated with the tick sequence.
        /// \param p Provider associated with the tick sequence.
        /// \param d Number of decimal places for price.
        /// \param vd Number of decimal places for volume.
        /// \param src Provider-owned source used to obtain the sequence.
        TickSequence(
                std::vector<Tick> ts,
                std::string s,
                std::string p,
                std::uint32_t d,
                std::uint32_t vd,
                std::string src = {})
            : ticks(std::move(ts)),
              symbol(std::move(s)),
              provider(std::move(p)),
              source(std::move(src)),
              price_digits(d),
              volume_digits(vd) {}
    };

} // namespace optionx

#endif // OPTIONX_HEADER_DATA_TICKS_TICK_SEQUENCE_HPP_INCLUDED
