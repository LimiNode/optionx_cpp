#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_PARSED_MESSAGE_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_PARSED_MESSAGE_HPP_INCLUDED

/// \file TelegramParsedMessage.hpp
/// \brief Parser result DTOs for Telegram signals and outcomes.

#include "TelegramRawMessage.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace optionx::bridges::telegram {

    /// \enum TelegramAssetMarket
    /// \brief Market qualifier carried by a Telegram asset symbol.
    enum class TelegramAssetMarket {
        UNKNOWN,
        OTC
    };

    /// \struct TelegramParsedSignal
    /// \brief One normalized executable signal extracted from a raw message.
    struct TelegramParsedSignal {
        std::string source_message_identity;
        std::int64_t source_date_ms = 0;
        std::string symbol;
        OrderType order_type = OrderType::UNKNOWN;
        OptionType option_type = OptionType::UNKNOWN;
        std::uint32_t duration = 0;
        std::int64_t expiry_time = 0;
        std::string signal_name;
        std::optional<std::int32_t> martingale_step;
        std::string raw_text;
        TelegramAssetMarket market = TelegramAssetMarket::UNKNOWN;
    };

    /// \enum TelegramOutcomeResult
    /// \brief Normalized result of a Telegram outcome message.
    enum class TelegramOutcomeResult {
        UNKNOWN,
        WIN,
        LOSS,
        REFUND
    };

    /// \struct TelegramParsedOutcome
    /// \brief A non-executable result that may be correlated with a signal.
    struct TelegramParsedOutcome {
        std::string source_message_identity;
        std::int64_t source_date_ms = 0;
        std::string reply_to_message_identity;
        std::string symbol;
        OrderType order_type = OrderType::UNKNOWN;
        TelegramOutcomeResult result = TelegramOutcomeResult::UNKNOWN;
        std::optional<std::int32_t> martingale_step;
        std::string signal_name;
        std::string raw_text;
        TelegramAssetMarket market = TelegramAssetMarket::UNKNOWN;
    };

    /// \struct TelegramParseDiagnostic
    /// \brief Non-fatal parser explanation for a message or text span.
    struct TelegramParseDiagnostic {
        std::string code;
        std::string message;
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    /// \struct TelegramParsedMessage
    /// \brief Complete parser output for one raw Telegram message.
    struct TelegramParsedMessage {
        TelegramRawMessage raw;
        std::vector<TelegramParsedSignal> signals;
        std::vector<TelegramParsedOutcome> outcomes;
        std::vector<TelegramParseDiagnostic> diagnostics;
    };

} // namespace optionx::bridges::telegram

#endif // OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_PARSED_MESSAGE_HPP_INCLUDED
