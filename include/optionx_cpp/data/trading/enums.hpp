#pragma once
#ifndef OPTIONX_HEADER_DATA_TRADING_ENUMS_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TRADING_ENUMS_HPP_INCLUDED

/// \file enums.hpp
/// \brief Defines trading-related enumerations and their utility functions.

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include "types.hpp"

namespace optionx {

	/// \enum PlatformType
    /// \brief Represents different trading platform types.
	enum class PlatformType {
        UNKNOWN = 0,    ///< Unknown platform type
        SIMULATOR,      ///< Simulation platform
        CLICKER,        ///< Click-based trading platform
        INTRADE_BAR,    ///< Intrade Bar platform
        TRADEUP         ///< TradeUp platform
    };

	/// \brief Converts PlatformType to its string representation.
    /// \param value The PlatformType enumeration value.
    /// \param mode Format mode (0-uppercase, 1-lowercase, 2-title case).
    /// \return Constant reference to the corresponding string.
	inline const std::string &to_str(PlatformType value, int mode = 0) noexcept {
        static const std::vector<std::string> str_data_0 = {"UNKNOWN", "SIMULATOR", "CLICKER", "INTRADE_BAR", "TRADEUP"};
        static const std::vector<std::string> str_data_1 = {"unknown", "simulator", "clicker", "intrade.bar", "tradeup"};
        static const std::vector<std::string> str_data_2 = {"Unknown", "Simulator", "Clicker", "Intrade Bar", "TradeUp"};

        switch (mode) {
            case 0: return utils::enum_string_or_unknown(str_data_0, static_cast<size_t>(value));
            case 1: return utils::enum_string_or_unknown(str_data_1, static_cast<size_t>(value));
            case 2: return utils::enum_string_or_unknown(str_data_2, static_cast<size_t>(value));
            default: break;
        }
		return utils::enum_string_or_unknown(str_data_0, static_cast<size_t>(value));
    };

	/// \brief Converts string to PlatformType enumeration.
    /// \param str Input string to convert.
    /// \param value Output enumeration value.
    /// \return True if conversion succeeded.
	inline const bool to_enum(const std::string &str, PlatformType &value) noexcept {
        static const std::unordered_map<std::string, PlatformType> str_data =    {
            {"UNKNOWN",                 PlatformType::UNKNOWN,     },
            {"SIMULATOR",               PlatformType::SIMULATOR,   },
            {"CLICKER",                 PlatformType::CLICKER,     },
            {"INTRADE_BAR",             PlatformType::INTRADE_BAR, },
            {"TRADEUP",                 PlatformType::TRADEUP,     }
        };
        auto it = str_data.find(utils::to_upper_case(str));
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

	/// \brief Template specialization for PlatformType enum conversion.
	template <>
    inline PlatformType to_enum<PlatformType>(const std::string &str) {
        PlatformType value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid PlatformType string: " + str);
        }
		return value;
    }

	/// \brief Converts PlatformType to JSON.
    inline void to_json(nlohmann::json& j, const PlatformType& type) {
        j = optionx::to_str(type);
    }

    /// \brief Converts JSON to PlatformType.
    inline void from_json(const nlohmann::json& j, PlatformType& type) {
        type = optionx::to_enum<PlatformType>(j.get<std::string>());
    }

	/// \brief Stream output operator for PlatformType.
	inline std::ostream& operator<<(std::ostream& os, PlatformType value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

    /// \enum BridgeType
    /// \brief Represents different types of bridges.
    enum class BridgeType {
        UNKNOWN = 0,                    ///< Unknown bridge type.
        LEGACY_TRADING_NAMED_PIPE,      ///< Legacy named-pipe trading bridge.
        TRADING_VIEW_EXTENSION_HTTP,    ///< TradingView browser extension HTTP bridge.
        METATRADER_FILE_TRANSPORT,      ///< MetaTrader common-files JSON-RPC bridge transport.
        BRIDGE_PROTOCOL_V1_HTTP_WEBSOCKET, ///< Bridge Protocol v1 HTTP/WebSocket server.
        BRIDGE_PROTOCOL_V1_NAMED_PIPE,  ///< Bridge Protocol v1 named-pipe server.
        BOT_BINARY,                      ///< BotBinary/BinaryBot compatibility bridge.
        TELEGRAM_SIGNAL                  ///< Telegram user-client signal bridge.
    };

    /// \brief Converts BridgeType to its string representation.
    /// \param value The BridgeType enumeration value.
    /// \return Constant reference to the corresponding string.
    inline const std::string& to_str(BridgeType value, int mode = 0) noexcept {
        (void)mode;
        static const std::vector<std::string> str_data = {
            "UNKNOWN",
            "LEGACY_TRADING_NAMED_PIPE",
            "TRADING_VIEW_EXTENSION_HTTP",
            "METATRADER_FILE_TRANSPORT",
            "BRIDGE_PROTOCOL_V1_HTTP_WEBSOCKET",
            "BRIDGE_PROTOCOL_V1_NAMED_PIPE",
            "BOT_BINARY",
            "TELEGRAM_SIGNAL"
        };
        return utils::enum_string_or_unknown(str_data, static_cast<size_t>(value));
    }

    /// \brief Converts string to BridgeType enumeration.
    /// \param str Input string to convert.
    /// \param value Output enumeration value.
    /// \return True if conversion succeeded.
    inline bool to_enum(const std::string& str, BridgeType& value) noexcept {
        static const std::unordered_map<std::string, BridgeType> str_data = {
            {"UNKNOWN",                    BridgeType::UNKNOWN},
            {"LEGACY_TRADING_NAMED_PIPE",  BridgeType::LEGACY_TRADING_NAMED_PIPE},
            {"TRADING_VIEW_EXTENSION_HTTP", BridgeType::TRADING_VIEW_EXTENSION_HTTP},
            {"METATRADER_FILE_TRANSPORT",  BridgeType::METATRADER_FILE_TRANSPORT},
            {"BRIDGE_PROTOCOL_V1_HTTP_WEBSOCKET", BridgeType::BRIDGE_PROTOCOL_V1_HTTP_WEBSOCKET},
            {"BRIDGE_PROTOCOL_V1_NAMED_PIPE", BridgeType::BRIDGE_PROTOCOL_V1_NAMED_PIPE},
            {"BOT_BINARY", BridgeType::BOT_BINARY},
            {"TELEGRAM_SIGNAL", BridgeType::TELEGRAM_SIGNAL},
            {"BINARYBOT", BridgeType::BOT_BINARY},
            {"BOTBINARY", BridgeType::BOT_BINARY}
        };
        auto it = str_data.find(utils::to_upper_case(str));
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

    /// \brief Template specialization for BridgeType enum conversion.
    template <>
    inline BridgeType to_enum<BridgeType>(const std::string& str) {
        BridgeType value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid BridgeType string: " + str);
        }
        return value;
    }

    /// \brief Converts BridgeType to JSON.
    inline void to_json(nlohmann::json& j, const BridgeType& type) {
        j = optionx::to_str(type);
    }

    /// \brief Converts JSON to BridgeType.
    inline void from_json(const nlohmann::json& j, BridgeType& type) {
        type = optionx::to_enum<BridgeType>(j.get<std::string>());
    }

    /// \brief Stream output operator for BridgeType.
    inline std::ostream& operator<<(std::ostream& os, BridgeType value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

	/// \enum AccountType
    /// \brief Represents account types (Demo/Real).
	enum class AccountType {
        UNKNOWN = 0,    ///< Unknown account type
        DEMO    = 1,    ///< Demo trading account
        REAL    = 2     ///< Real money account
    };

	/// \brief Converts AccountType to its string representation.
    /// \param value The AccountType enumeration value.
    /// \param mode Format mode (0-uppercase, 1-title case).
	inline const std::string &to_str(AccountType value, int mode = 0) noexcept {
        static const std::vector<std::string> str_data_0 = {"UNKNOWN", "DEMO", "REAL"};
        static const std::vector<std::string> str_data_1 = {"Unknown", "Demo", "Real"};

		switch (mode) {
            case 0: return utils::enum_string_or_unknown(str_data_0, static_cast<size_t>(value));
            case 1: return utils::enum_string_or_unknown(str_data_1, static_cast<size_t>(value));
            default: break;
        }
		return utils::enum_string_or_unknown(str_data_0, static_cast<size_t>(value));
    };

	/// \brief Converts string to AccountType enumeration.
	inline const bool to_enum(const std::string &str, AccountType &value) noexcept {
        static const std::unordered_map<std::string, AccountType> str_data = {
            {"UNKNOWN", AccountType::UNKNOWN},
            {"DEMO", AccountType::DEMO},
            {"REAL", AccountType::REAL}
        };

        auto it = str_data.find(utils::to_upper_case(str));
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

	/// \brief Template specialization for AccountType enum conversion.
    template <>
    inline AccountType to_enum<AccountType>(const std::string &str) {
        AccountType value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid AccountType string: " + str);
        }
		return value;
    }

	/// \brief Converts AccountType to JSON.
    inline void to_json(nlohmann::json& j, const AccountType& type) {
        j = optionx::to_str(type);
    }

    /// \brief Converts JSON to AccountType.
    inline void from_json(const nlohmann::json& j, AccountType& type) {
        type = optionx::to_enum<AccountType>(j.get<std::string>());
    }

	/// \brief Stream output operator for AccountType.
	inline std::ostream& operator<<(std::ostream& os, AccountType value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

	/// \enum OptionType
    /// \brief Represents types of binary options.
	enum class OptionType {
        UNKNOWN = 0,    ///< Unknown option type
        SPRINT  = 1,    ///< Short-term option
        CLASSIC = 2     ///< Standard option
    };

	/// \brief Converts OptionType to its string representation.
    /// \param value The OptionType enumeration value.
    /// \param mode Format mode (0-uppercase, 1-title case).
	inline const std::string &to_str(OptionType value, int mode = 0) noexcept {
        static const std::vector<std::string> str_data_0 = {"UNKNOWN", "SPRINT", "CLASSIC"};
        static const std::vector<std::string> str_data_1 = {"Unknown", "Sprint", "Classic"};
        switch (mode) {
            case 0: return utils::enum_string_or_unknown(str_data_0, static_cast<size_t>(value));
            case 1: return utils::enum_string_or_unknown(str_data_1, static_cast<size_t>(value));
            default: break;
        };
		return utils::enum_string_or_unknown(str_data_0, static_cast<size_t>(value));
    };

	/// \brief Converts string to OptionType enumeration.
	inline const bool to_enum(const std::string &str, OptionType &value) noexcept {
        static const std::unordered_map<std::string, OptionType> str_data = {
            {"UNKNOWN", OptionType::UNKNOWN},
            {"SPRINT", OptionType::SPRINT},
            {"CLASSIC", OptionType::CLASSIC}
        };
        auto it = str_data.find(utils::to_upper_case(str));
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

	/// \brief Template specialization for OptionType enum conversion.
	template <>
    inline OptionType to_enum<OptionType>(const std::string &str) {
        OptionType value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid OptionType string: " + str);
        }
        return value;
    }

	/// \brief Converts OptionType to JSON.
    inline void to_json(nlohmann::json& j, const OptionType& state) {
		j = optionx::to_str(state);
    }

    /// \brief Converts JSON to OptionType.
    inline void from_json(const nlohmann::json& j, OptionType& state) {
        state = optionx::to_enum<OptionType>(j.get<std::string>());
    }

	/// \brief Stream output operator for OptionType.
	inline std::ostream& operator<<(std::ostream& os, OptionType value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

    /// \enum OrderType
    /// \brief Represents order type (Buy/Sell).
    enum class OrderType {
        UNKNOWN = 0,    ///< Unknown order type
        BUY     = 1,    ///< Buy order
        SELL    = 2     ///< Sell order
    };

	/// \brief Converts OrderType to its string representation.
    /// \param value The OrderType enumeration value.
    /// \param mode Format mode (0-uppercase, 1-title case, 2-put/call, 3-Put/Call).
	inline const std::string& to_str(OrderType value, int mode = 0) noexcept {
        static const std::vector<std::string> str_data_0 = {"UNKNOWN", "BUY", "SELL"};
        static const std::vector<std::string> str_data_1 = {"Unknown", "Buy", "Sell"};
        static const std::vector<std::string> str_data_2 = {"UNKNOWN", "PUT", "CALL"};
        static const std::vector<std::string> str_data_3 = {"Unknown", "Put", "Call"};

        switch (mode) {
            case 0: return utils::enum_string_or_unknown(str_data_0, static_cast<size_t>(value));
            case 1: return utils::enum_string_or_unknown(str_data_1, static_cast<size_t>(value));
            case 2: return utils::enum_string_or_unknown(str_data_2, static_cast<size_t>(value));
            case 3: return utils::enum_string_or_unknown(str_data_3, static_cast<size_t>(value));
            default: break;
        };
		return utils::enum_string_or_unknown(str_data_0, static_cast<size_t>(value));
    };

	/// \brief Converts string to OrderType enumeration.
	inline const bool to_enum(const std::string &str, OrderType &value) noexcept {
        static const std::unordered_map<std::string, OrderType> str_map = {
            {"UNKNOWN", OrderType::UNKNOWN},
            {"BUY", OrderType::BUY}, {"SELL", OrderType::SELL},
            {"PUT", OrderType::BUY}, {"CALL", OrderType::SELL},
            {"UP", OrderType::BUY}, {"DN", OrderType::SELL}
        };

        auto it = str_map.find(utils::to_upper_case(str));
        if (it != str_map.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

	/// \brief Template specialization for OrderType enum conversion.
	template <>
    inline OrderType to_enum<OrderType>(const std::string &str) {
        OrderType value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid OrderType string: " + str);
        }
        return value;
    }

	/// \brief Converts OrderType to JSON.
    inline void to_json(nlohmann::json& j, const OrderType& type) {
		j = optionx::to_str(type);
    }

    /// \brief Converts JSON to OrderType.
    inline void from_json(const nlohmann::json& j, OrderType& type) {
		type = optionx::to_enum<OrderType>(j.get<std::string>());
    }

	/// \brief Stream output operator for OrderType.
	inline std::ostream& operator<<(std::ostream& os, OrderType value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

	/// \enum CurrencyType
    /// \brief Represents currency type.
	enum class CurrencyType {
        UNKNOWN = 0,    ///< Unknown currency
        USD,            ///< US Dollar
        EUR,            ///< Euro
        GBP,            ///< British Pound
        BTC,            ///< Bitcoin
        ETH,            ///< Ethereum
        USDT,           ///< Tether
        USDC,           ///< USD Coin
        RUB,            ///< Russian Ruble
        UAH,            ///< Ukrainian Hryvnia
        KZT             ///< Kazakhstani Tenge
    };

	/// \brief Converts CurrencyType to its uppercase string representation.
	inline const std::string &to_str(const CurrencyType value) noexcept {
        static const std::vector<std::string> str_data = {
            "UNKNOWN",
            "USD",
            "EUR",
            "GBP",
            "BTC",
            "ETH",
            "USDT",
            "USDC",
            "RUB",
            "UAH",
            "KZT"
        };
        return utils::enum_string_or_unknown(str_data, static_cast<size_t>(value));
    };

	/// \brief Converts string to CurrencyType enumeration.
	inline const bool to_enum(const std::string &str, CurrencyType &value) noexcept {
        static const std::unordered_map<std::string, CurrencyType> str_data = {
            {"UNKNOWN", CurrencyType::UNKNOWN},
            {"USD",     CurrencyType::USD},
            {"EUR",     CurrencyType::EUR},
            {"GBP",     CurrencyType::GBP},
            {"BTC",     CurrencyType::BTC},
            {"ETH",     CurrencyType::ETH},
            {"USDT",    CurrencyType::USDT},
            {"USDC",    CurrencyType::USDC},
            {"RUB",     CurrencyType::RUB},
            {"UAH",     CurrencyType::UAH},
            {"KZT",     CurrencyType::KZT}
        };

        auto it = str_data.find(utils::to_upper_case(str));
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

	/// \brief Template specialization for CurrencyType enum conversion.
	template <>
    inline CurrencyType to_enum<CurrencyType>(const std::string &str) {
        CurrencyType value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid CurrencyType string: " + str);
        }
        return value;
    }

	/// \brief Converts CurrencyType to JSON.
    inline void to_json(nlohmann::json& j, const CurrencyType& type) {
		j = optionx::to_str(type);
    }

    /// \brief Converts JSON to CurrencyType.
    inline void from_json(const nlohmann::json& j, CurrencyType& type) {
        type = optionx::to_enum<CurrencyType>(j.get<std::string>());
    }

	/// \brief Stream output operator for CurrencyType.
	inline std::ostream& operator<<(std::ostream& os, CurrencyType value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

    /// \enum TradeState
    /// \brief Represents the state of a binary option during its lifecycle.
    enum class TradeState {
        UNKNOWN,            ///< Initial state
        WAITING_OPEN,       ///< Waiting to open
        OPEN_SUCCESS,       ///< Successfully opened
        OPEN_ERROR,         ///< Error opening
        IN_PROGRESS,        ///< Trade is active
        WAITING_CLOSE,      ///< Waiting to close
        CHECK_ERROR,        ///< Error checking status
        WIN,                ///< Trade won
        LOSS,               ///< Trade lost
        STANDOFF,           ///< No clear outcome
        REFUND,             ///< Trade refunded
        CANCELED_TRADE      ///< Trade canceled
    };

	/// \brief Converts TradeState to its string representation.
    /// \param value The TradeState enumeration value.
    /// \return A constant reference to the string representation.
    inline const std::string& to_str(TradeState value) noexcept {
        static const std::vector<std::string> str_data = {
            "UNKNOWN",
            "WAITING_OPEN",
            "OPEN_SUCCESS",
            "OPEN_ERROR",
            "IN_PROGRESS",
            "WAITING_CLOSE",
            "CHECK_ERROR",
            "WIN",
            "LOSS",
            "STANDOFF",
            "REFUND",
            "CANCELED_TRADE"
        };
        return utils::enum_string_or_unknown(str_data, static_cast<size_t>(value));
    }

	/// \brief Converts a string to its corresponding TradeState enumeration value.
    /// \param str The string representation of the TradeState.
    /// \param value The TradeState to populate.
    /// \return True if the conversion succeeded, false otherwise.
    inline bool to_enum(const std::string &str, TradeState &value) noexcept {
        static const std::unordered_map<std::string, TradeState> str_data = {
            {"UNKNOWN",         TradeState::UNKNOWN},
            {"WAITING_OPEN",    TradeState::WAITING_OPEN},
            {"OPEN_SUCCESS",    TradeState::OPEN_SUCCESS},
            {"OPEN_ERROR",      TradeState::OPEN_ERROR},
            {"IN_PROGRESS",     TradeState::IN_PROGRESS},
            {"WAITING_CLOSE",   TradeState::WAITING_CLOSE},
            {"CHECK_ERROR",     TradeState::CHECK_ERROR},
            {"WIN",             TradeState::WIN},
            {"LOSS",            TradeState::LOSS},
            {"STANDOFF",        TradeState::STANDOFF},
            {"REFUND",          TradeState::REFUND},
            {"CANCELED_TRADE",  TradeState::CANCELED_TRADE}
        };
        auto it = str_data.find(utils::to_upper_case(str));
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

    /// \brief Template specialization to convert a string to TradeState.
    /// \param str The string representation.
    /// \return The corresponding TradeState value.
    /// \throws std::invalid_argument If the string cannot be converted.
    template <>
    inline TradeState to_enum<TradeState>(const std::string &str) {
        TradeState value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid TradeState string: " + str);
        }
        return value;
    }

	/// \brief Converts TradeState to JSON.
    inline void to_json(nlohmann::json& j, const TradeState& state) {
		j = optionx::to_str(state);
    }

    /// \brief Converts JSON to TradeState.
    inline void from_json(const nlohmann::json& j, TradeState& state) {
        state = optionx::to_enum<TradeState>(j.get<std::string>());
    }

	/// \brief Stream output operator for TradeState.
	inline std::ostream& operator<<(std::ostream& os, TradeState value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

    /// \enum SignalStatus
    /// \brief Represents lifecycle states of a stored trading signal.
    enum class SignalStatus {
        UNKNOWN,     ///< Unknown signal state.
        RECEIVED,    ///< Signal was received but not accepted yet.
        ACCEPTED,    ///< Signal passed validation and may produce trades.
        REJECTED,    ///< Signal was rejected before producing trades.
        IN_PROGRESS, ///< Signal is producing or waiting for trades.
        COMPLETED,   ///< Signal processing is complete.
        CANCELED,    ///< Signal was canceled.
        FAILED       ///< Signal processing ended with an error.
    };

    /// \brief Converts SignalStatus to its string representation.
    inline const std::string& to_str(SignalStatus value) noexcept {
        static const std::vector<std::string> str_data = {
            "UNKNOWN",
            "RECEIVED",
            "ACCEPTED",
            "REJECTED",
            "IN_PROGRESS",
            "COMPLETED",
            "CANCELED",
            "ERROR"
        };
        return utils::enum_string_or_unknown(str_data, static_cast<size_t>(value));
    }

    /// \brief Converts a string to SignalStatus.
    inline bool to_enum(const std::string& str, SignalStatus& value) noexcept {
        static const std::unordered_map<std::string, SignalStatus> str_data = {
            {"UNKNOWN",     SignalStatus::UNKNOWN},
            {"RECEIVED",    SignalStatus::RECEIVED},
            {"ACCEPTED",    SignalStatus::ACCEPTED},
            {"REJECTED",    SignalStatus::REJECTED},
            {"IN_PROGRESS", SignalStatus::IN_PROGRESS},
            {"COMPLETED",   SignalStatus::COMPLETED},
            {"CANCELED",    SignalStatus::CANCELED},
            {"ERROR",       SignalStatus::FAILED},
            {"FAILED",      SignalStatus::FAILED}
        };
        auto it = str_data.find(utils::to_upper_case(str));
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

    /// \brief Template specialization to convert a string to SignalStatus.
    template <>
    inline SignalStatus to_enum<SignalStatus>(const std::string& str) {
        SignalStatus value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid SignalStatus string: " + str);
        }
        return value;
    }

    /// \brief Converts SignalStatus to JSON.
    inline void to_json(nlohmann::json& j, const SignalStatus& status) {
        j = optionx::to_str(status);
    }

    /// \brief Converts JSON to SignalStatus.
    inline void from_json(const nlohmann::json& j, SignalStatus& status) {
        status = optionx::to_enum<SignalStatus>(j.get<std::string>());
    }

    /// \brief Stream output operator for SignalStatus.
    inline std::ostream& operator<<(std::ostream& os, SignalStatus value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

    /// \enum SignalRejectCode
    /// \brief Represents signal-level rejection reasons.
    enum class SignalRejectCode {
        NONE,            ///< Signal was not rejected.
        INVALID_SIGNAL,  ///< Signal payload is invalid.
        INVALID_REQUEST, ///< Embedded trade request is invalid.
        FILTERED,        ///< Signal was filtered by strategy/risk rules.
        RISK_LIMIT,      ///< Risk or money-management limit rejected it.
        DUPLICATE_SIGNAL, ///< Duplicate signal was detected.
        NO_CONNECTION,   ///< Required broker/platform connection was unavailable.
        INTERNAL_ERROR   ///< Internal processing error.
    };

    /// \brief Converts SignalRejectCode to its string representation.
    inline const std::string& to_str(SignalRejectCode value) noexcept {
        static const std::vector<std::string> str_data = {
            "NONE",
            "INVALID_SIGNAL",
            "INVALID_REQUEST",
            "FILTERED",
            "RISK_LIMIT",
            "DUPLICATE",
            "NO_CONNECTION",
            "INTERNAL_ERROR"
        };
        return utils::enum_string_or_unknown(str_data, static_cast<size_t>(value));
    }

    /// \brief Converts a string to SignalRejectCode.
    inline bool to_enum(const std::string& str, SignalRejectCode& value) noexcept {
        static const std::unordered_map<std::string, SignalRejectCode> str_data = {
            {"NONE",            SignalRejectCode::NONE},
            {"INVALID_SIGNAL",  SignalRejectCode::INVALID_SIGNAL},
            {"INVALID_REQUEST", SignalRejectCode::INVALID_REQUEST},
            {"FILTERED",        SignalRejectCode::FILTERED},
            {"RISK_LIMIT",      SignalRejectCode::RISK_LIMIT},
            {"DUPLICATE",       SignalRejectCode::DUPLICATE_SIGNAL},
            {"DUPLICATE_SIGNAL", SignalRejectCode::DUPLICATE_SIGNAL},
            {"NO_CONNECTION",   SignalRejectCode::NO_CONNECTION},
            {"INTERNAL_ERROR",  SignalRejectCode::INTERNAL_ERROR}
        };
        auto it = str_data.find(utils::to_upper_case(str));
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

    /// \brief Template specialization to convert a string to SignalRejectCode.
    template <>
    inline SignalRejectCode to_enum<SignalRejectCode>(const std::string& str) {
        SignalRejectCode value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid SignalRejectCode string: " + str);
        }
        return value;
    }

    /// \brief Converts SignalRejectCode to JSON.
    inline void to_json(nlohmann::json& j, const SignalRejectCode& code) {
        j = optionx::to_str(code);
    }

    /// \brief Converts JSON to SignalRejectCode.
    inline void from_json(const nlohmann::json& j, SignalRejectCode& code) {
        code = optionx::to_enum<SignalRejectCode>(j.get<std::string>());
    }

    /// \brief Stream output operator for SignalRejectCode.
    inline std::ostream& operator<<(std::ostream& os, SignalRejectCode value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

    /// \enum SignalOutcome
    /// \brief Represents aggregated outcome of a signal and its produced trades.
    enum class SignalOutcome {
        UNKNOWN,  ///< Outcome has not been calculated.
        WIN,      ///< Signal ended with positive profit.
        LOSS,     ///< Signal ended with negative profit.
        STANDOFF, ///< Signal ended flat.
        REFUND,   ///< Signal was refunded.
        MIXED,    ///< Produced trades have mixed terminal states.
        FAILED    ///< Signal ended with an error state.
    };

    /// \brief Converts SignalOutcome to its string representation.
    inline const std::string& to_str(SignalOutcome value) noexcept {
        static const std::vector<std::string> str_data = {
            "UNKNOWN",
            "WIN",
            "LOSS",
            "STANDOFF",
            "REFUND",
            "MIXED",
            "ERROR"
        };
        return utils::enum_string_or_unknown(str_data, static_cast<size_t>(value));
    }

    /// \brief Converts a string to SignalOutcome.
    inline bool to_enum(const std::string& str, SignalOutcome& value) noexcept {
        static const std::unordered_map<std::string, SignalOutcome> str_data = {
            {"UNKNOWN",  SignalOutcome::UNKNOWN},
            {"WIN",      SignalOutcome::WIN},
            {"LOSS",     SignalOutcome::LOSS},
            {"STANDOFF", SignalOutcome::STANDOFF},
            {"REFUND",   SignalOutcome::REFUND},
            {"MIXED",    SignalOutcome::MIXED},
            {"ERROR",    SignalOutcome::FAILED},
            {"FAILED",   SignalOutcome::FAILED}
        };
        auto it = str_data.find(utils::to_upper_case(str));
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

    /// \brief Template specialization to convert a string to SignalOutcome.
    template <>
    inline SignalOutcome to_enum<SignalOutcome>(const std::string& str) {
        SignalOutcome value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid SignalOutcome string: " + str);
        }
        return value;
    }

    /// \brief Converts SignalOutcome to JSON.
    inline void to_json(nlohmann::json& j, const SignalOutcome& outcome) {
        j = optionx::to_str(outcome);
    }

    /// \brief Converts JSON to SignalOutcome.
    inline void from_json(const nlohmann::json& j, SignalOutcome& outcome) {
        outcome = optionx::to_enum<SignalOutcome>(j.get<std::string>());
    }

    /// \brief Stream output operator for SignalOutcome.
    inline std::ostream& operator<<(std::ostream& os, SignalOutcome value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

    /// \enum TradeErrorCode
    /// \brief Represents error codes for order validation and processing.
    enum class TradeErrorCode {
        SUCCESS,                ///< Operation successful
        INVALID_SYMBOL,         ///< Invalid trading symbol
        INVALID_OPTION,         ///< Invalid option type
        INVALID_ORDER,          ///< Invalid order type
        INVALID_ACCOUNT,        ///< Invalid account type
        INVALID_CURRENCY,       ///< Invalid currency
        AMOUNT_TOO_LOW,         ///< Amount below minimum
        AMOUNT_TOO_HIGH,        ///< Amount above maximum
        REFUND_TOO_LOW,         ///< Refund too low
        REFUND_TOO_HIGH,        ///< Refund too high
        PAYOUT_TOO_LOW,         ///< Payout too low
        INVALID_DURATION,       ///< Invalid trade duration
        INVALID_EXPIRY_TIME,    ///< Invalid expiration time
        LIMIT_OPEN_TRADES,      ///< Open trades limit reached
        INVALID_REQUEST,        ///< Malformed request
        LONG_QUEUE_WAIT,        ///< Queue wait timeout
        LONG_RESPONSE_WAIT,     ///< Server response timeout
        NO_CONNECTION,          ///< Network connection lost
        CLIENT_FORCED_CLOSE,    ///< Client forced close
        PARSING_ERROR,          ///< Data parsing error
        CANCELED_TRADE,         ///< Trade canceled
        INSUFFICIENT_BALANCE    ///< Not enough funds
    };

	/// \brief Converts an TradeErrorCode value to its corresponding string representation.
    /// \param value The error code to convert.
    /// \return A string representation of the provided error code.
    inline const std::string &to_str(TradeErrorCode value) noexcept {
        static const std::vector<std::string> str_data = {
            "Success.",
            "Invalid symbol.",
            "Invalid option type.",
            "Invalid order type.",
            "Invalid account type.",
            "Invalid currency.",
            "Amount below minimum.",
            "Amount above maximum.",
            "Refund below minimum.",
            "Refund above maximum.",
            "Low payout percentage.",
            "Invalid duration.",
            "Invalid expiry time.",
            "Reached open trades limit.",
            "Invalid request.",
            "Long wait in the order queue.",
            "Long wait for server response.",
            "No network connection.",
            "Forced client shutdown.",
            "Parser error.",
            "Canceled.",
            "Insufficient balance."
        };
        static const std::string unknown = "Unknown error code.";
        size_t index = static_cast<size_t>(value);
        return (index < str_data.size()) ? str_data[index] : unknown;
    }

    /// \brief Converts a string to its corresponding TradeErrorCode value.
    /// \param str The string representation of the error code.
    /// \param value The TradeErrorCode to populate.
    /// \return True if the conversion succeeded, false otherwise.
    inline bool to_enum(const std::string &str, TradeErrorCode &value) noexcept {
        static const std::map<std::string,TradeErrorCode> str_data = {
            {"Success.",                  TradeErrorCode::SUCCESS},
            {"Invalid symbol.",           TradeErrorCode::INVALID_SYMBOL},
            {"Invalid option type.",      TradeErrorCode::INVALID_OPTION},
            {"Invalid order type.",       TradeErrorCode::INVALID_ORDER},
            {"Invalid account type.",     TradeErrorCode::INVALID_ACCOUNT},
            {"Invalid currency.",         TradeErrorCode::INVALID_CURRENCY},
            {"Amount below minimum.",     TradeErrorCode::AMOUNT_TOO_LOW},
            {"Amount above maximum.",     TradeErrorCode::AMOUNT_TOO_HIGH},
            {"Refund below minimum.",     TradeErrorCode::REFUND_TOO_LOW},
            {"Refund above maximum.",     TradeErrorCode::REFUND_TOO_HIGH},
            {"Low payout percentage.",    TradeErrorCode::PAYOUT_TOO_LOW},
            {"Invalid duration.",         TradeErrorCode::INVALID_DURATION},
            {"Invalid expiry time.",      TradeErrorCode::INVALID_EXPIRY_TIME},
            {"Reached open trades limit.",TradeErrorCode::LIMIT_OPEN_TRADES},
            {"Invalid request.",          TradeErrorCode::INVALID_REQUEST},
            {"Long wait in the order queue.", TradeErrorCode::LONG_QUEUE_WAIT},
            {"Long wait for server response.",TradeErrorCode::LONG_RESPONSE_WAIT},
            {"No network connection.",    TradeErrorCode::NO_CONNECTION},
            {"Forced client shutdown.",   TradeErrorCode::CLIENT_FORCED_CLOSE},
            {"Parser error.",             TradeErrorCode::PARSING_ERROR},
            {"Canceled.",                 TradeErrorCode::CANCELED_TRADE},
            {"Insufficient balance.",     TradeErrorCode::INSUFFICIENT_BALANCE}
        };

        auto it = str_data.find(str);
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

    /// \brief Template specialization to convert a string to TradeErrorCode.
    /// \param str The string representation.
    /// \return The corresponding TradeErrorCode value.
    /// \throws std::invalid_argument If the string cannot be converted.
    template <>
    inline TradeErrorCode to_enum<TradeErrorCode>(const std::string &str) {
        TradeErrorCode value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid TradeErrorCode string: " + str);
        }
        return value;
    }

	/// \brief Converts TradeErrorCode to JSON.
    inline void to_json(nlohmann::json& j, const TradeErrorCode& state) {
		j = optionx::to_str(state);
    }

    /// \brief Converts JSON to TradeErrorCode.
    inline void from_json(const nlohmann::json& j, TradeErrorCode& state) {
        state = optionx::to_enum<TradeErrorCode>(j.get<std::string>());
    }

	/// \brief Stream output operator for TradeErrorCode.
    inline std::ostream& operator<<(std::ostream& os, TradeErrorCode value) {
        os << optionx::to_str(value);
        return os;
    }

//------------------------------------------------------------------------------

	/// \enum MmSystemType
    /// \brief Defines various money management strategies.
    enum class MmSystemType {
        NONE,                      ///< No money management.
        FIXED,                     ///< Fixed trade amount.
        PERCENT,                   ///< Percentage of balance.
        KELLY_CRITERION,           ///< Kelly criterion for optimal bet sizing.

        // Martingale variations
        MARTINGALE_SIGNAL,          ///< Martingale per signal instance.
        MARTINGALE_SYMBOL,          ///< Martingale per trading symbol.
        MARTINGALE_BAR,             ///< Martingale per market bar.

        // Anti-Martingale variations
        ANTI_MARTINGALE_SIGNAL,     ///< Anti-Martingale per signal instance.
        ANTI_MARTINGALE_SYMBOL,     ///< Anti-Martingale per trading symbol.
        ANTI_MARTINGALE_BAR,        ///< Anti-Martingale per market bar.

        // Labouchere variations
        LABOUCHERE_SIGNAL,          ///< Labouchere per signal.
        LABOUCHERE_SYMBOL,          ///< Labouchere per symbol.
        LABOUCHERE_BAR,             ///< Labouchere per bar.

        // SKU (Some unknown strategy)
        SKU_SIGNAL,                 ///< SKU per signal.
        SKU_SYMBOL,                 ///< SKU per symbol.
        SKU_BAR                     ///< SKU per bar.
    };

	/// \brief Converts MmSystemType to its string representation.
    /// \param value The MmSystemType enumeration value.
    /// \return A constant reference to the string representation.
    inline const std::string& to_str(MmSystemType value) noexcept {
        static const std::vector<std::string> str_data = {
            "NONE",
            "FIXED",
            "PERCENT",
            "KELLY_CRITERION",
            "MARTINGALE_SIGNAL",
            "MARTINGALE_SYMBOL",
            "MARTINGALE_BAR",
            "ANTI_MARTINGALE_SIGNAL",
            "ANTI_MARTINGALE_SYMBOL",
            "ANTI_MARTINGALE_BAR",
            "LABOUCHERE_SIGNAL",
            "LABOUCHERE_SYMBOL",
            "LABOUCHERE_BAR",
            "SKU_SIGNAL",
            "SKU_SYMBOL",
            "SKU_BAR"
        };
        return utils::enum_string_or_unknown(str_data, static_cast<size_t>(value));
    }

    /// \brief Converts a string to its corresponding MmSystemType enumeration value.
    /// \param str The string representation of the MmSystemType.
    /// \param value The MmSystemType to populate.
    /// \return True if the conversion succeeded, false otherwise.
    inline bool to_enum(const std::string& str, MmSystemType& value) noexcept {
        static const std::unordered_map<std::string, MmSystemType> str_data = {
            {"NONE", 					MmSystemType::NONE						},
            {"FIXED", 					MmSystemType::FIXED						},
            {"PERCENT", 				MmSystemType::PERCENT					},
            {"KELLY_CRITERION", 		MmSystemType::KELLY_CRITERION			},
            {"MARTINGALE_SIGNAL", 		MmSystemType::MARTINGALE_SIGNAL			},
            {"MARTINGALE_SYMBOL", 		MmSystemType::MARTINGALE_SYMBOL			},
            {"MARTINGALE_BAR", 			MmSystemType::MARTINGALE_BAR			},
            {"ANTI_MARTINGALE_SIGNAL", 	MmSystemType::ANTI_MARTINGALE_SIGNAL	},
            {"ANTI_MARTINGALE_SYMBOL", 	MmSystemType::ANTI_MARTINGALE_SYMBOL	},
            {"ANTI_MARTINGALE_BAR", 	MmSystemType::ANTI_MARTINGALE_BAR		},
            {"LABOUCHERE_SIGNAL", 		MmSystemType::LABOUCHERE_SIGNAL			},
            {"LABOUCHERE_SYMBOL", 		MmSystemType::LABOUCHERE_SYMBOL			},
            {"LABOUCHERE_BAR", 			MmSystemType::LABOUCHERE_BAR			},
            {"SKU_SIGNAL", 				MmSystemType::SKU_SIGNAL				},
            {"SKU_SYMBOL", 				MmSystemType::SKU_SYMBOL				},
            {"SKU_BAR", 				MmSystemType::SKU_BAR					}
        };
        auto it = str_data.find(str);
        if (it != str_data.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

    /// \brief Template specialization to convert a string to MmSystemType.
    /// \param str The string representation.
    /// \return The corresponding MmSystemType value.
    /// \throws std::invalid_argument If the string cannot be converted.
    template <>
    inline MmSystemType to_enum<MmSystemType>(const std::string& str) {
        MmSystemType value;
        if (!to_enum(str, value)) {
            throw std::invalid_argument("Invalid MmSystemType string: " + str);
        }
        return value;
    }

    /// \brief Converts MmSystemType to JSON.
    /// \param j The JSON object to populate.
    /// \param state The MmSystemType value to convert.
    inline void to_json(nlohmann::json& j, const MmSystemType& state) {
        j = optionx::to_str(state);
    }

    /// \brief Converts JSON to MmSystemType.
    /// \param j The JSON object to read.
    /// \param state The MmSystemType variable to populate.
    inline void from_json(const nlohmann::json& j, MmSystemType& state) {
        state = optionx::to_enum<MmSystemType>(j.get<std::string>());
    }

    /// \brief Stream output operator for MmSystemType.
    /// \param os The output stream.
    /// \param value The MmSystemType enumeration value.
    /// \return The output stream with the string representation of MmSystemType.
    inline std::ostream& operator<<(std::ostream& os, MmSystemType value) {
        os << optionx::to_str(value);
        return os;
    }

} // namespace optionx

#endif // OPTIONX_HEADER_DATA_TRADING_ENUMS_HPP_INCLUDED
