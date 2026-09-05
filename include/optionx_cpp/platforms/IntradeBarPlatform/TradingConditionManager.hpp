#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_TRADING_CONDITION_MANAGER_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_TRADING_CONDITION_MANAGER_HPP_INCLUDED

/// \file TradingConditionManager.hpp
/// \brief Publishes computed Intrade Bar trading-condition snapshots.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace optionx::platforms::intrade_bar {

    /// \class TradingConditionManager
    /// \brief Mirrors the Intrade Bar account condition model into public events.
    class TradingConditionManager final : public components::BaseComponent {
    public:
        /// \brief Constructs and registers the condition manager.
        /// \param platform Owning platform.
        /// \param account_info Platform account snapshot.
        explicit TradingConditionManager(
                BaseTradingPlatform& platform,
                std::shared_ptr<BaseAccountInfoData> account_info)
            : BaseComponent(platform.event_bus()),
              m_account_info(std::dynamic_pointer_cast<AccountInfoData>(
                  std::move(account_info))) {
            subscribe<events::AccountInfoUpdateEvent>();
            platform.register_component(this);
        }

        /// \brief Handles account lifecycle and state changes.
        /// \param event Incoming event.
        void on_event(const utils::Event* const event) override {
            const auto* update =
                dynamic_cast<const events::AccountInfoUpdateEvent*>(event);
            if (!update) return;

            if (auto account_info =
                    std::dynamic_pointer_cast<AccountInfoData>(update->account_info)) {
                m_account_info = std::move(account_info);
            }
            if (!m_account_info) return;

            switch (update->status) {
            case AccountUpdateStatus::CONNECTING:
            case AccountUpdateStatus::DISCONNECTED:
            case AccountUpdateStatus::FAILED_TO_CONNECT:
                m_connected = false;
                break;
            case AccountUpdateStatus::CONNECTED:
                m_connected = true;
                break;
            case AccountUpdateStatus::BALANCE_UPDATED:
            case AccountUpdateStatus::ACCOUNT_TYPE_CHANGED:
            case AccountUpdateStatus::CURRENCY_CHANGED:
            case AccountUpdateStatus::OPEN_TRADES_CHANGED:
                m_connected = m_account_info->connect;
                break;
            case AccountUpdateStatus::UNKNOWN:
                return;
            }

            refresh(current_timestamp_sec());
        }

        /// \brief Refreshes time-dependent conditions once per Unix second.
        void process() override {
            const auto timestamp = current_timestamp_sec();
            if (timestamp == m_last_refresh_sec) return;
            refresh(timestamp);
        }

        /// \brief Clears local condition state during platform shutdown.
        void shutdown() override {
            const auto timestamp = current_timestamp_sec();
            for (const auto& previous : m_last_snapshots) {
                TradingConditionUpdate retired;
                retired.symbol = previous.symbol;
                retired.platform_type = previous.platform_type;
                retired.account_type = previous.account_type;
                retired.currency = previous.currency;
                retired.option_type = previous.option_type;
                retired.timestamp = timestamp;
                retired.tradable = false;
                retired.message = "Intrade Bar account condition manager stopped.";
                publish(retired);
            }
            m_connected = false;
            m_last_refresh_sec = 0;
            m_last_snapshots.clear();
        }

    private:
        std::shared_ptr<AccountInfoData> m_account_info; ///< Current Intrade account data.
        std::vector<TradingConditionUpdate> m_last_snapshots; ///< Last published values by scope.
        std::int64_t m_last_refresh_sec = 0; ///< Last evaluated Unix second.
        bool m_connected = false; ///< Account lifecycle state from account events.

        /// \brief Returns the current Unix timestamp in seconds.
        static std::int64_t current_timestamp_sec() noexcept {
            return time_shield::ms_to_sec(OPTIONX_TIMESTAMP_MS);
        }

        /// \brief Clamps a signed condition value to its public DTO width.
        static std::uint32_t clamp_u32(std::int64_t value) noexcept {
            if (value <= 0) return 0;
            const auto maximum =
                static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
            return static_cast<std::uint32_t>(std::min(value, maximum));
        }

        /// \brief Returns the earliest aligned classic expiry candidate.
        static std::int64_t next_classic_expiry(std::int64_t timestamp) noexcept {
            const auto future = timestamp + (8 * time_shield::SEC_PER_MIN);
            return future - (future % time_shield::SEC_PER_5_MIN);
        }

        /// \brief Builds one current condition snapshot.
        static TradingConditionUpdate make_snapshot(
                const AccountInfoData& account,
                const std::string& symbol,
                OptionType option_type,
                std::int64_t timestamp,
                bool connected) {
            TradingConditionUpdate snapshot;
            snapshot.symbol = symbol;
            snapshot.platform_type = PlatformType::INTRADE_BAR;
            snapshot.account_type = account.account_type;
            snapshot.currency = account.currency;
            snapshot.option_type = option_type;
            snapshot.timestamp = timestamp;

            const bool btc = is_btc_symbol(symbol);
            const auto second_of_day = time_shield::sec_of_day(timestamp);
            const auto session_start = btc ? account.start_btc_time : account.start_time;
            const auto session_end = btc ? account.end_btc_time : account.end_time;
            const bool session_open =
                second_of_day >= session_start && second_of_day < session_end;
            const bool market_open = session_open &&
                (btc || !time_shield::is_day_off(timestamp));

            AccountInfoRequest request;
            request.symbol = symbol;
            request.account_type = account.account_type;
            request.currency = account.currency;
            request.option_type = option_type;
            request.timestamp = timestamp;

            request.type = AccountInfoType::MIN_AMOUNT;
            snapshot.min_amount = account.get_info<double>(request);
            request.type = AccountInfoType::MAX_AMOUNT;
            snapshot.max_amount = account.get_info<double>(request);
            request.type = AccountInfoType::MAX_TRADES;
            snapshot.max_open_trades = clamp_u32(account.get_info<std::int64_t>(request));

            snapshot.market_open = market_open;
            snapshot.session_start = session_start;
            snapshot.session_end = session_end;

            bool expiration_available = true;
            if (option_type == OptionType::SPRINT) {
                request.type = AccountInfoType::MIN_DURATION;
                const auto min_duration = account.get_info<std::int64_t>(request);
                request.type = AccountInfoType::MAX_DURATION;
                const auto max_duration = account.get_info<std::int64_t>(request);
                snapshot.min_duration = clamp_u32(min_duration);
                snapshot.max_duration = clamp_u32(max_duration);
                expiration_available = max_duration >= min_duration;
            } else {
                const auto expiry_time = next_classic_expiry(timestamp);
                const auto expiry_second_of_day =
                    time_shield::sec_of_day(expiry_time);
                expiration_available =
                    expiry_second_of_day >= session_start &&
                    expiry_second_of_day <= session_end &&
                    (expiry_time % time_shield::SEC_PER_5_MIN) == 0 &&
                    (expiry_time - timestamp) >= time_shield::SEC_PER_3_MIN;
            }

            const bool trade_slot_available =
                account.open_trades < static_cast<std::int64_t>(*snapshot.max_open_trades);
            snapshot.tradable = connected && market_open &&
                expiration_available && trade_slot_available;
            return snapshot;
        }

        /// \brief Builds current snapshots for all supported symbol/option scopes.
        std::vector<TradingConditionUpdate> make_snapshots(
                std::int64_t timestamp) const {
            std::vector<TradingConditionUpdate> snapshots;
            snapshots.reserve((supported_symbols().size() * 2) - 1);
            for (const auto* symbol : supported_symbols()) {
                snapshots.push_back(make_snapshot(
                    *m_account_info,
                    symbol,
                    OptionType::SPRINT,
                    timestamp,
                    m_connected));
                if (!is_btc_symbol(symbol)) {
                    snapshots.push_back(make_snapshot(
                        *m_account_info,
                        symbol,
                        OptionType::CLASSIC,
                        timestamp,
                        m_connected));
                }
            }
            return snapshots;
        }

        /// \brief Compares generated condition values while ignoring timestamp.
        static bool same_values(
                const TradingConditionUpdate& left,
                const TradingConditionUpdate& right) {
            return left.same_scope(right) &&
                left.market_open == right.market_open &&
                left.tradable == right.tradable &&
                left.payout == right.payout &&
                left.min_amount == right.min_amount &&
                left.max_amount == right.max_amount &&
                left.min_refund == right.min_refund &&
                left.max_refund == right.max_refund &&
                left.min_duration == right.min_duration &&
                left.max_duration == right.max_duration &&
                left.max_open_trades == right.max_open_trades &&
                left.session_start == right.session_start &&
                left.session_end == right.session_end &&
                left.message == right.message;
        }

        /// \brief Publishes one condition update synchronously on the platform bus.
        void publish(const TradingConditionUpdate& update) const {
            notify(events::TradingConditionUpdateEvent(update));
        }

        /// \brief Marks cached scopes unavailable before replacing their identity.
        void retire_missing_scopes(
                const std::vector<TradingConditionUpdate>& next,
                std::int64_t timestamp) const {
            for (const auto& previous : m_last_snapshots) {
                const auto found = std::find_if(
                    next.begin(),
                    next.end(),
                    [&previous](const TradingConditionUpdate& candidate) {
                        return previous.same_scope(candidate);
                    });
                if (found != next.end()) continue;

                TradingConditionUpdate retired;
                retired.symbol = previous.symbol;
                retired.platform_type = previous.platform_type;
                retired.account_type = previous.account_type;
                retired.currency = previous.currency;
                retired.option_type = previous.option_type;
                retired.timestamp = timestamp;
                retired.tradable = false;
                retired.message = "Intrade Bar account condition scope changed.";
                publish(retired);
            }
        }

        /// \brief Recomputes snapshots and publishes only changed scopes.
        void refresh(std::int64_t timestamp) {
            m_last_refresh_sec = timestamp;
            if (!m_account_info ||
                m_account_info->account_type == AccountType::UNKNOWN ||
                m_account_info->currency == CurrencyType::UNKNOWN) {
                retire_missing_scopes({}, timestamp);
                m_last_snapshots.clear();
                return;
            }

            auto next = make_snapshots(timestamp);
            retire_missing_scopes(next, timestamp);
            for (const auto& current : next) {
                const auto previous = std::find_if(
                    m_last_snapshots.begin(),
                    m_last_snapshots.end(),
                    [&current](const TradingConditionUpdate& candidate) {
                        return current.same_scope(candidate);
                    });
                if (previous == m_last_snapshots.end() ||
                    !same_values(*previous, current)) {
                    publish(current);
                }
            }
            m_last_snapshots = std::move(next);
        }
    };

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_TRADING_CONDITION_MANAGER_HPP_INCLUDED
