/// \file telegram_live_bridge_smoke.cpp
/// \brief Runs the Telegram worker and C++ signal bridge against a live chat.

#include "example_utils.hpp"

#include <optionx_cpp/bridges/telegram.hpp>
#include <tg_client_stdio/worker_client.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string worker_root;
    std::string python;
    std::string chat;
    std::string marker;
    std::string api_id;
    std::string api_hash;
    std::string session;
    std::string proxy;
    std::chrono::seconds timeout{300};
};

void load_env_file(const std::filesystem::path& path);
Options parse_options(int argc, char** argv);
bool wait_for_signal(
        std::condition_variable& condition,
        std::mutex& mutex,
        bool& matched,
        bool& failed,
        std::chrono::seconds timeout);
std::string required_setting(const std::string& value, const char* name);

} // namespace

int main(int argc, char** argv) {
    try {
        std::cout.setf(std::ios::unitbuf);
        const auto env_file_argument = optionx::examples::option_value(
            argc, argv, "--env-file");
        const auto env_file = env_file_argument.empty()
            ? optionx::examples::env_or("TG_CLIENT_STDIO_ENV_FILE")
            : env_file_argument;
        if (!env_file.empty()) {
            load_env_file(env_file);
        }

        const auto options = parse_options(argc, argv);
        if (options.chat.empty()) {
            throw std::invalid_argument(
                "chat is required; use --chat or TG_CLIENT_STDIO_TEST_CHAT");
        }
        const auto api_id = required_setting(
            options.api_id, "TG_CLIENT_STDIO_API_ID");
        const auto api_hash = required_setting(
            options.api_hash, "TG_CLIENT_STDIO_API_HASH");
        const auto session = required_setting(
            options.session, "TG_CLIENT_STDIO_SESSION");
        const auto worker_root = required_setting(
            options.worker_root, "TG_CLIENT_STDIO_WORKER_ROOT");

        std::vector<std::string> command{
            options.python,
            "-m",
            "tg_client_stdio_worker",
            "--backend",
            "telethon",
            "--api-id",
            api_id,
            "--api-hash",
            api_hash,
            "--session",
            session,
        };
        if (!options.proxy.empty()) {
            command.insert(command.end(), {"--proxy", options.proxy});
        }

        tg_client_stdio::WorkerProcessConfig worker_config;
        worker_config.command = std::move(command);
        worker_config.working_directory = worker_root;
        worker_config.on_stderr = [](const std::string& text) {
            std::cerr << "[tg-worker] " << text;
        };

        tg_client_stdio::WorkerClient worker;
        if (!worker.start(std::move(worker_config))) {
            std::cerr << "failed to start tg-client-stdio worker\n";
            return 2;
        }

        const auto auth = worker.get_auth_status();
        if (!auth.authorized) {
            std::cerr << "Telegram session is not authorized\n";
            worker.stop();
            return 2;
        }

        auto source = std::make_shared<
            optionx::bridges::telegram::TelegramWorkerMessageSource<
                tg_client_stdio::WorkerClient>>(
            worker,
            optionx::bridges::telegram::TelegramWorkerSourceConfig{
                {options.chat},
                {},
            });
        optionx::bridges::telegram::TelegramSignalBridge bridge(source);

        auto config = std::make_unique<
            optionx::bridges::telegram::TelegramSignalBridgeConfig>();
        config->bridge_id = 9101;
        config->fixed_amount = 1.0;
        if (!bridge.configure(std::move(config))) {
            std::cerr << "failed to configure Telegram signal bridge\n";
            worker.stop();
            return 2;
        }

        std::mutex state_mutex;
        std::condition_variable state_condition;
        bool matched = false;
        bool failed = false;
        std::int64_t next_signal_id = 0;

        bridge.on_signal_id() = [&next_signal_id]() {
            return ++next_signal_id;
        };
        bridge.on_status_update() = [&state_condition, &state_mutex, &failed](
                const optionx::BridgeStatusUpdate& update) {
            std::cout << "status=" << optionx::to_str(update.status);
            if (!update.message.empty()) {
                std::cout << " message=" << update.message;
            }
            std::cout << '\n';
            if (update.status == optionx::BridgeStatus::SERVER_START_FAILED ||
                update.status == optionx::BridgeStatus::CONNECTION_ERROR) {
                std::lock_guard<std::mutex> lock(state_mutex);
                failed = true;
                state_condition.notify_all();
            }
        };
        bridge.on_signal_report() = [](const optionx::BridgeSignalReport& report) {
            std::cout << "report=" << report.reason_code
                      << " status=" << optionx::to_str(report.status);
            if (!report.message.empty()) {
                std::cout << " message=" << report.message;
            }
            std::cout << '\n';
        };
        bridge.on_trade_signal() = [
                &state_condition,
                &state_mutex,
                &matched,
                &options](std::unique_ptr<optionx::TradeSignal> signal) {
            std::cout << "signal=" << signal->symbol
                      << " direction=" << optionx::to_str(signal->order_type)
                      << " duration=" << signal->duration
                      << " name=" << signal->signal_name
                      << " text=" << signal->comment << '\n';
            if (options.marker.empty() ||
                signal->comment.find(options.marker) != std::string::npos) {
                std::lock_guard<std::mutex> lock(state_mutex);
                matched = true;
                state_condition.notify_all();
            }
        };

        bridge.run();
        const auto success = wait_for_signal(
            state_condition,
            state_mutex,
            matched,
            failed,
            options.timeout);
        bridge.shutdown();
        worker.stop();

        std::cout << "matched=" << (success ? "true" : "false") << '\n';
        return success ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}

namespace {

void load_env_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open env file: " + path.string());
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }
        const auto separator = line.find('=', first);
        if (separator == std::string::npos) {
            continue;
        }
        auto key = line.substr(first, separator - first);
        auto value = line.substr(separator + 1);
        const auto key_end = key.find_last_not_of(" \t");
        key.resize(key_end == std::string::npos ? 0 : key_end + 1);
        const auto value_first = value.find_first_not_of(" \t");
        value = value_first == std::string::npos
            ? std::string()
            : value.substr(value_first);
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (key.empty() || std::getenv(key.c_str()) != nullptr) {
            continue;
        }
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 0);
#endif
    }
}

Options parse_options(const int argc, char** argv) {
    Options options;
    options.worker_root = optionx::examples::env_or("TG_CLIENT_STDIO_WORKER_ROOT");
    options.python = optionx::examples::env_or(
        "TG_CLIENT_STDIO_PYTHON",
        "python");
    options.chat = optionx::examples::env_or("TG_CLIENT_STDIO_TEST_CHAT");
    options.marker = optionx::examples::env_or("TG_CLIENT_STDIO_BRIDGE_MARKER");
    options.api_id = optionx::examples::env_or("TG_CLIENT_STDIO_API_ID");
    options.api_hash = optionx::examples::env_or("TG_CLIENT_STDIO_API_HASH");
    options.session = optionx::examples::env_or("TG_CLIENT_STDIO_SESSION");
    options.proxy = optionx::examples::env_or("TG_CLIENT_STDIO_PROXY");

    const auto assign = [&](const char* name, std::string& target) {
        const auto value = optionx::examples::option_value(argc, argv, name);
        if (!value.empty()) {
            target = value;
        }
    };
    assign("--worker-root", options.worker_root);
    assign("--python", options.python);
    assign("--chat", options.chat);
    assign("--marker", options.marker);
    assign("--api-id", options.api_id);
    assign("--api-hash", options.api_hash);
    assign("--session", options.session);
    assign("--proxy", options.proxy);

    const auto timeout = optionx::examples::option_value(argc, argv, "--timeout");
    if (!timeout.empty()) {
        const auto seconds = std::stoi(timeout);
        if (seconds <= 0) {
            throw std::invalid_argument("timeout must be positive");
        }
        options.timeout = std::chrono::seconds(seconds);
    }
    return options;
}

bool wait_for_signal(
        std::condition_variable& condition,
        std::mutex& mutex,
        bool& matched,
        bool& failed,
        const std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait_for(lock, timeout, [&matched, &failed]() {
        return matched || failed;
    });
    return matched && !failed;
}

std::string required_setting(
        const std::string& value,
        const char* name) {
    if (value.empty()) {
        throw std::invalid_argument(std::string(name) + " is required");
    }
    return value;
}

} // namespace
