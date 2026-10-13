#pragma once
#ifndef OPTIONX_HEADER_DATA_TRADING_TRADE_RESULT_QUERY_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TRADING_TRADE_RESULT_QUERY_HPP_INCLUDED

/// \file TradeResultQuery.hpp
/// \brief Defines broker-side trade result lookup parameters.

namespace optionx {

    /// \class TradeResultQuery
    /// \brief Identifies a broker-side trade whose final result should be fetched.
    class TradeResultQuery {
    public:
        std::uint32_t trade_id = 0;   ///< Local persistent trade ID, used for tracing/result propagation.
        std::int64_t option_id = 0;   ///< Numeric broker-side trade ID.
        std::string option_hash;      ///< String/hash broker-side trade ID.
        int retry_attempts = 15;      ///< Broker result retry attempts for temporarily empty/error responses (rate-limited by the platform).

        /// \brief Checks whether a local persistent trade ID is available.
        /// \return True when trade_id is non-zero.
        bool has_local_identity() const noexcept {
            return trade_id != 0;
        }

        /// \brief Checks whether at least one broker-side identity is available.
        /// \return True when option_id or option_hash can identify the trade at a broker.
        bool has_broker_identity() const noexcept {
            return option_id > 0 || !option_hash.empty();
        }

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(
            TradeResultQuery,
            trade_id,
            option_id,
            option_hash,
            retry_attempts
        )
    };

} // namespace optionx

#endif // OPTIONX_HEADER_DATA_TRADING_TRADE_RESULT_QUERY_HPP_INCLUDED
