#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_API_RESPONSES_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_API_RESPONSES_HPP_INCLUDED

/// \file ApiResponses.hpp
/// \brief Typed response payloads for Intrade Bar HTTP workflows.

namespace optionx::platforms::intrade_bar {

    /// \brief Result of selecting an available Intrade Bar domain.
    struct DomainSelection {
        std::string host;
    };

    /// \brief Availability check for the current HTTP host.
    struct HostAvailability {
        bool available = false;
    };

    /// \brief Login protocol detected from the broker page contents.
    enum class LoginPageFlow {
        LEGACY_FORM, ///< Retired hidden-input login form.
        MODERN_NUXT, ///< Nuxt landing page with the current SPA login API.
        SITE_FORM    ///< Server-rendered site form with challenge proof-of-work.
    };

    /// \brief Challenge and cookies prepared for the detected login protocol.
    struct MainPageChallenge {
        LoginPageFlow flow = LoginPageFlow::LEGACY_FORM;
        std::string origin;
        std::string req_id;
        std::string req_value;
        std::string cookies;
        std::string site_api_path;
        std::string site_csrf;
        std::string site_pow;
        std::string site_nonce;
        std::uint32_t site_pow_bits = 0;
    };

    /// \brief Converts a login protocol to a stable diagnostic string.
    inline const char* login_page_flow_to_string(LoginPageFlow flow) noexcept {
        switch (flow) {
        case LoginPageFlow::LEGACY_FORM:
            return "legacy_form";
        case LoginPageFlow::MODERN_NUXT:
            return "modern_nuxt";
        case LoginPageFlow::SITE_FORM:
            return "site_form";
        }
        return "unknown";
    }

    /// \brief User credentials returned by the login page flow.
    struct LoginCredentials {
        std::string user_id;
        std::string user_hash;
        std::string cookies;
    };

    /// \brief Auth endpoint acknowledgement payload.
    struct AuthCheck {
    };

    /// \brief Profile settings visible after authentication.
    struct ProfileInfo {
        CurrencyType currency = CurrencyType::UNKNOWN;
        AccountType account_type = AccountType::UNKNOWN;
    };

    /// \brief Current account balance.
    struct BalanceInfo {
        double balance = 0.0;
        CurrencyType currency = CurrencyType::UNKNOWN;
    };

    /// \brief Diagnostic reason for a failed account settings switch.
    enum class SettingsSwitchFailureReason {
        NONE,                ///< The switch succeeded or has not failed.
        TRANSPORT_ERROR,     ///< HTTP, proxy, timeout, status, or DDoS validation failed.
        BROKER_REJECTED,     ///< Broker returned a rejection, usually while active trades exist.
        UNEXPECTED_RESPONSE  ///< Broker returned a syntactically valid but unknown response body.
    };

    /// \brief Account settings switch acknowledgement.
    struct SettingsSwitch {
        SettingsSwitchFailureReason failure_reason = SettingsSwitchFailureReason::NONE; ///< Failure reason.
        std::string response_body; ///< Raw response body, when available.

        /// \brief Whether this failure is expected to become successful after a retry.
        /// \return True when broker rejection may be caused by temporary active trades.
        bool should_retry() const noexcept {
            return failure_reason == SettingsSwitchFailureReason::BROKER_REJECTED;
        }
    };

    /// \brief Active trade visible on the authenticated main page.
    struct ActiveTradeInfo {
        int64_t id = 0;
        std::string symbol;
        double open_price = 0.0;
        int64_t open_time_ms = 0;
        int64_t close_time_ms = 0;
        int status = 0;
        int contract = 0;
    };

    /// \brief Active trades snapshot from the authenticated main page.
    struct ActiveTradesSnapshot {
        std::vector<ActiveTradeInfo> trades;
    };

    /// \brief Tick snapshot returned by the polling price endpoint.
    struct PriceSnapshot {
        std::vector<SingleTick> ticks;
    };

    /// \brief Trade open response payload.
    struct TradeOpenInfo {
        int64_t option_id = 0;
        int64_t open_date = 0;
        double open_price = 0.0;
    };

    /// \brief Trade result check response payload.
    struct TradeCheckInfo {
        double price = 0.0;
        double profit = 0.0;
    };

    /// \brief Closed trade history payload.
    struct TradeHistory {
        std::vector<TradeRecord> records;
    };

    /// \brief Applies Intrade Bar trade-check payload to a common TradeResult.
    /// \param check Parsed broker response where profit is a gross returned amount.
    /// \param result Trade result to update.
    /// \return True if the outcome was classified; false if required context is missing.
    inline bool apply_trade_check_info_to_result(
            const TradeCheckInfo& check,
            TradeResult& result) {
        result.close_price = check.price;

        constexpr double balance_tolerance = 0.01;
        if (result.amount <= 0.0) {
            result.trade_state = result.live_state = TradeState::CHECK_ERROR;
            result.error_code = TradeErrorCode::INVALID_REQUEST;
            result.error_desc = "Trade amount is required to classify Intrade Bar trade result.";
            return false;
        }

        if (std::abs(check.profit - result.amount) < balance_tolerance) {
            result.trade_state = result.live_state = TradeState::STANDOFF;
            result.profit = 0.0;
        } else if (check.profit <= balance_tolerance) {
            result.trade_state = result.live_state = TradeState::LOSS;
            result.profit = -result.amount;
        } else {
            result.trade_state = result.live_state = TradeState::WIN;
            result.profit = check.profit - result.amount;
            result.payout = utils::normalize_double(
                (check.profit - result.amount) / result.amount,
                2);
        }

        result.error_code = TradeErrorCode::SUCCESS;
        result.error_desc.clear();
        return true;
    }

    using DomainSelectionResult = ApiResult<DomainSelection>;
    using HostAvailabilityResult = ApiResult<HostAvailability>;
    using MainPageChallengeResult = ApiResult<MainPageChallenge>;
    using LoginCredentialsResult = ApiResult<LoginCredentials>;
    using AuthCheckResult = ApiResult<AuthCheck>;
    using ProfileInfoResult = ApiResult<ProfileInfo>;
    using BalanceInfoResult = ApiResult<BalanceInfo>;
    using SettingsSwitchResult = ApiResult<SettingsSwitch>;
    using ActiveTradesSnapshotResult = ApiResult<ActiveTradesSnapshot>;
    using PriceSnapshotResult = ApiResult<PriceSnapshot>;
    using BarHistoryApiResult = ApiResult<BarSequence>;
    using TradeOpenResult = ApiResult<TradeOpenInfo>;
    using TradeCheckResult = ApiResult<TradeCheckInfo>;
    using TradeHistoryApiResult = ApiResult<TradeHistory>;

    /// \brief Converts a settings-switch failure reason to a stable log string.
    /// \param reason Failure reason.
    /// \return String literal for logs and diagnostics.
    inline const char* settings_switch_failure_reason_to_string(
            SettingsSwitchFailureReason reason) noexcept {
        switch (reason) {
        case SettingsSwitchFailureReason::NONE:
            return "none";
        case SettingsSwitchFailureReason::TRANSPORT_ERROR:
            return "transport_error";
        case SettingsSwitchFailureReason::BROKER_REJECTED:
            return "broker_rejected";
        case SettingsSwitchFailureReason::UNEXPECTED_RESPONSE:
            return "unexpected_response";
        }
        return "unknown";
    }

    /// \brief Builds a failed settings-switch result with diagnostic payload.
    /// \param reason Typed failure reason.
    /// \param message Human-readable error message.
    /// \param status HTTP status code, if available.
    /// \param response_body Raw response body, when safe and available.
    /// \return Failed typed result.
    inline SettingsSwitchResult make_settings_switch_failure(
            SettingsSwitchFailureReason reason,
            std::string message,
            long status = SettingsSwitchResult::NO_RESPONSE_STATUS,
            std::string response_body = {}) {
        auto result = SettingsSwitchResult::fail(std::move(message), status);
        result.value.failure_reason = reason;
        result.value.response_body = std::move(response_body);
        return result;
    }

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_API_RESPONSES_HPP_INCLUDED
