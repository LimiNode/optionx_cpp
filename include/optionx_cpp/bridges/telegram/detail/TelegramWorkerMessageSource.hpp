#pragma once
#ifndef OPTIONX_HEADER_BRIDGES_TELEGRAM_DETAIL_TELEGRAM_WORKER_MESSAGE_SOURCE_HPP_INCLUDED
#define OPTIONX_HEADER_BRIDGES_TELEGRAM_DETAIL_TELEGRAM_WORKER_MESSAGE_SOURCE_HPP_INCLUDED

/// \file bridges/telegram/detail/TelegramWorkerMessageSource.hpp
/// \brief Adapter from a tg-client-stdio-style worker client to Telegram bridge input.

#include <optionx_cpp/bridges/telegram/TelegramSignalBridge.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace optionx::bridges::telegram {

    /// \struct TelegramWorkerSourceConfig
    /// \brief Chat and topic selection for one worker live listener.
    struct TelegramWorkerSourceConfig {
        std::vector<std::string> chats;
        std::vector<std::string> topic_ids;
    };

    /// \class TelegramWorkerMessageSource
    /// \brief Binds a WorkerClient-like object to TelegramMessageSource.
    ///
    /// The worker type is a template so this header does not force OptionX to
    /// include or link a particular worker repository. It is compatible with
    /// `tg_client_stdio::WorkerClient` and deterministic test doubles that
    /// provide the same `start_listening` and `stop_listening` operations.
    template <typename WorkerClient>
    class TelegramWorkerMessageSource final : public TelegramMessageSource {
    public:
        TelegramWorkerMessageSource(
                WorkerClient& worker,
                TelegramWorkerSourceConfig config)
            : m_worker(worker),
              m_config(std::move(config)) {}

        bool start(message_callback_t on_message,
                   error_callback_t on_error) override {
            if (m_started || m_config.chats.empty() || !on_message) {
                return false;
            }
            m_on_message = std::move(on_message);
            m_on_error = std::move(on_error);
            try {
                if (!m_worker.start_listening(
                        m_config.chats,
                        [this](const auto& record) { handle_record(record); },
                        m_config.topic_ids)) {
                    clear_callbacks();
                    return false;
                }
                m_started = true;
                return true;
            }
            catch (const std::exception& error) {
                report_error(error.what());
                clear_callbacks();
                return false;
            }
            catch (...) {
                report_error("Telegram worker listener failed to start.");
                clear_callbacks();
                return false;
            }
        }

        void stop() noexcept override {
            if (!m_started) {
                clear_callbacks();
                return;
            }
            try {
                (void)m_worker.stop_listening();
            }
            catch (const std::exception& error) {
                report_error(error.what());
            }
            catch (...) {
                report_error("Telegram worker listener failed to stop.");
            }
            m_started = false;
            clear_callbacks();
        }

    private:
        void handle_record(const nlohmann::json& record) {
            if (record.value("message_type", "") == "error") {
                const auto payload = record.value("payload", nlohmann::json::object());
                report_error(payload.value("message", "Telegram worker live error."));
                return;
            }
            if (record.value("operation", "") != "message.received") {
                return;
            }
            try {
                const auto payload = record.at("payload");
                const auto raw = TelegramRawMessage::from_json(payload.at("message"));
                if (m_on_message) {
                    m_on_message(raw);
                }
            }
            catch (const std::exception& error) {
                report_error(error.what());
            }
            catch (...) {
                report_error("Telegram worker message record was invalid.");
            }
        }

        void report_error(const std::string& message) {
            if (m_on_error) {
                try {
                    m_on_error(message);
                }
                catch (...) {
                }
            }
        }

        void clear_callbacks() noexcept {
            m_on_message = {};
            m_on_error = {};
        }

        WorkerClient& m_worker;
        TelegramWorkerSourceConfig m_config;
        message_callback_t m_on_message;
        error_callback_t m_on_error;
        bool m_started = false;
    };

} // namespace optionx::bridges::telegram

#endif // OPTIONX_HEADER_BRIDGES_TELEGRAM_DETAIL_TELEGRAM_WORKER_MESSAGE_SOURCE_HPP_INCLUDED
