#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_RAW_MESSAGE_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_RAW_MESSAGE_HPP_INCLUDED

/// \file TelegramRawMessage.hpp
/// \brief Stable transport DTO for one Telegram message.

#include "data/trading.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace optionx::bridges::telegram {

    /// \struct TelegramRawMessage
    /// \brief Raw Telegram message data used by live intake and replay.
    struct TelegramRawMessage {
        std::string chat_id;
        std::string chat_title;
        std::string topic_id;
        std::int64_t message_id = 0;
        std::int64_t date_ms = 0;
        std::int64_t edit_date_ms = 0;
        std::string sender_id;
        std::int64_t reply_to_message_id = 0;
        std::string grouped_id;
        std::string text;
        nlohmann::json media = nlohmann::json::array();

        /// \brief Returns the identity of the physical source message.
        std::string message_identity() const {
            return "telegram:" + chat_id + ":" +
                (topic_id.empty() ? "0" : topic_id) + ":" +
                std::to_string(message_id);
        }

        /// \brief Returns an identity that includes the edit revision.
        std::string revision_identity() const {
            return message_identity() + ":" + std::to_string(edit_date_ms);
        }

        /// \brief Returns the identity of the replied-to message, if present.
        std::string reply_to_message_identity() const {
            if (reply_to_message_id <= 0) {
                return {};
            }
            return "telegram:" + chat_id + ":" +
                (topic_id.empty() ? "0" : topic_id) + ":" +
                std::to_string(reply_to_message_id);
        }

        /// \brief Builds the DTO from a worker `export.message` payload.
        static TelegramRawMessage from_json(const nlohmann::json& value) {
            if (!value.is_object()) {
                throw std::invalid_argument("Telegram raw message must be an object");
            }
            TelegramRawMessage message;
            message.chat_id = value.at("chat_id").get<std::string>();
            message.chat_title = value.value("chat_title", "");
            message.topic_id = value.value("topic_id", "");
            message.message_id = value.at("message_id").get<std::int64_t>();
            message.date_ms = value.at("date_ms").get<std::int64_t>();
            message.edit_date_ms = value.value("edit_date_ms", 0ll);
            message.sender_id = value.value("sender_id", "");
            message.reply_to_message_id = value.value("reply_to_message_id", 0ll);
            message.grouped_id = value.value("grouped_id", "");
            message.text = value.value("text", "");
            message.media = value.value("media", nlohmann::json::array());
            message.validate();
            return message;
        }

        /// \brief Serializes the DTO using worker-compatible field names.
        nlohmann::json to_json() const {
            return nlohmann::json{
                {"chat_id", chat_id},
                {"chat_title", chat_title},
                {"topic_id", topic_id},
                {"message_id", message_id},
                {"date_ms", date_ms},
                {"edit_date_ms", edit_date_ms},
                {"sender_id", sender_id},
                {"reply_to_message_id", reply_to_message_id},
                {"grouped_id", grouped_id},
                {"text", text},
                {"media", media},
                {"message_identity", message_identity()},
                {"revision_identity", revision_identity()},
                {"reply_to_message_identity", reply_to_message_identity()},
            };
        }

        /// \brief Validates fields shared by worker and parser boundaries.
        void validate() const {
            if (chat_id.empty() || message_id <= 0 || date_ms < 0 ||
                edit_date_ms < 0 || reply_to_message_id < 0) {
                throw std::invalid_argument("invalid Telegram raw message identity or timestamp");
            }
            if (!media.is_array()) {
                throw std::invalid_argument("Telegram raw message media must be an array");
            }
        }
    };

} // namespace optionx::bridges::telegram

#endif // OPTIONX_HEADER_BRIDGES_TELEGRAM_TELEGRAM_RAW_MESSAGE_HPP_INCLUDED
