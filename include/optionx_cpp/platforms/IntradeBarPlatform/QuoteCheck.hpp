#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_QUOTE_CHECK_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_QUOTE_CHECK_HPP_INCLUDED

/// \file QuoteCheck.hpp
/// \brief Defines the broker execution-second quote-check contract.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace optionx::platforms::intrade_bar {

    /// \brief Stable provenance label for the broker's quote-check page.
    inline constexpr const char* INTRADE_QUOTE_CHECK_SOURCE =
        "INTRADE_BAR_QUOTES";

    /// \brief Maximum broker-local dates fetched by one public history request.
    inline constexpr std::size_t MAX_INTRADE_QUOTE_CHECK_DAYS = 2;

    /// \brief Maximum interval accepted by one broker `/quotes` request.
    /// \details The current quote-check page accepts at most 30 minutes for
    ///          currencies and metals. Longer public ranges are split into
    ///          consecutive requests by the runtime adapter.
    inline constexpr std::uint64_t MAX_INTRADE_QUOTE_CHECK_INTERVAL_SEC =
        30U * 60U;

    /// \struct BrokerQuoteCheckRequest
    /// \brief Requests the broker's execution-second values for one range.
    /// \details The broker endpoint accepts second-formatted boundaries and
    ///          one local broker date. The runtime adapter may split a request
    ///          at the 30-minute limit or local midnight, but each returned
    ///          observation remains second-aligned.
    struct BrokerQuoteCheckRequest {
        std::string symbol; ///< Public or normalized broker symbol.
        std::uint64_t from_time_ms = 0; ///< Inclusive UTC Unix timestamp.
        std::uint64_t to_time_ms = 0; ///< Inclusive UTC Unix timestamp.
        std::int64_t broker_utc_offset_sec = 3 * 60 * 60; ///< Observed broker offset.

        /// \brief Returns true for a usable inclusive request.
        [[nodiscard]] bool valid() const noexcept {
            return !symbol.empty() &&
                from_time_ms > 0 &&
                to_time_ms >= from_time_ms &&
                broker_utc_offset_sec >= -24 * 60 * 60 &&
                broker_utc_offset_sec <= 24 * 60 * 60;
        }
    };

    /// \brief Returns the first whole-second timestamp inside a request.
    inline std::uint64_t first_quote_check_time_ms(
            const BrokerQuoteCheckRequest& request) noexcept {
        const auto remainder = request.from_time_ms % 1000U;
        if (remainder == 0) return request.from_time_ms;
        const auto increment = 1000U - remainder;
        if (request.from_time_ms >
                (std::numeric_limits<std::uint64_t>::max)() - increment) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        return request.from_time_ms + increment;
    }

    /// \brief Returns the number of whole-second timestamps inside a request.
    inline std::uint64_t quote_check_expected_count(
            const BrokerQuoteCheckRequest& request) noexcept {
        if (!request.valid()) return 0;
        const auto first = first_quote_check_time_ms(request);
        const auto last = request.to_time_ms - request.to_time_ms % 1000U;
        if (first > last) return 0;
        return (last - first) / 1000U + 1U;
    }

    /// \struct BrokerQuoteCheckObservation
    /// \brief One price selected from an authenticated broker quote row.
    struct BrokerQuoteCheckObservation {
        std::uint64_t time_ms = 0; ///< UTC Unix timestamp of the row.
        double price = 0.0; ///< Quote value selected by the broker page.
    };

    /// \struct BrokerQuoteCheck
    /// \brief Parsed execution-second observations returned by `/quotes`.
    /// \details `range_complete` is true only when every broker whole-second
    ///          timestamp inside the requested inclusive range is present and
    ///          no malformed row was observed. It does not claim that all
    ///          market events inside those seconds are represented.
    struct BrokerQuoteCheck {
        std::string symbol; ///< Normalized symbol associated with the rows.
        std::string source = INTRADE_QUOTE_CHECK_SOURCE; ///< Provenance label.
        std::vector<BrokerQuoteCheckObservation> observations;
        bool range_complete = false; ///< Whether the requested second grid is dense.
        std::size_t malformed_rows = 0; ///< Rows rejected by the parser.
    };

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_QUOTE_CHECK_HPP_INCLUDED
