#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_CORE_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_CORE_HPP_INCLUDED

/// \file SimulatorCore.hpp
/// \brief Bounded single-owner offline binary-option execution and integer ledger.

#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>

namespace optionx::platforms {

    enum class SimulatorPriceSide { BID, ASK, MID, REFERENCE };

    /// \brief Immutable fixture input: positive fixed-scale prices, Unix UTC milliseconds.
    struct SimulatorTick {
        std::string symbol;
        std::int64_t timestamp_ms = 0;
        std::int64_t price_units = 0;
        std::size_t observation_id = 0; ///< Assigned from input order, not caller data.
        std::int64_t received_ms = 0; ///< Simulator owner observation time; fixtures use zero.
        std::uint64_t provider_key = 0;
        std::uint64_t provider_instance = 0;
        std::uint64_t source_generation = 0;
        std::uint64_t provider_received_ms = 0; ///< Provider metadata, not the decision clock.
        std::uint32_t price_decimals = 0;
        SimulatorPriceSide price_side = SimulatorPriceSide::REFERENCE;
    };

    /// \class SimulatorQuoteFixture
    /// \brief Owning indexed fixture; equal timestamps use the last input observation.
    class SimulatorQuoteFixture {
    public:
        explicit SimulatorQuoteFixture(std::vector<SimulatorTick> ticks,
                                       std::size_t maximum_ticks = 100000) : m_ticks(std::move(ticks)) {
            if (maximum_ticks == 0 || maximum_ticks > 1000000 || m_ticks.size() > maximum_ticks)
                throw std::invalid_argument("simulator fixture capacity");
            for (std::size_t i = 0; i < m_ticks.size(); ++i) {
                auto& tick = m_ticks[i];
                if (tick.symbol.empty() || tick.symbol.size() > 64 ||
                    tick.timestamp_ms < 0 || tick.price_units <= 0)
                    throw std::invalid_argument("simulator fixture tick");
                tick.observation_id = i + 1;
            }
            std::stable_sort(m_ticks.begin(), m_ticks.end(), [](const auto& a, const auto& b) {
                return std::tie(a.symbol, a.timestamp_ms) < std::tie(b.symbol, b.timestamp_ms);
            });
        }

        /// \brief Latest observation <= event; freshness is checked by the execution owner.
        std::optional<SimulatorTick> latest(const std::string& symbol, std::int64_t event_ms) const {
            auto it = std::upper_bound(m_ticks.begin(), m_ticks.end(),
                std::make_pair(symbol, event_ms), [](const auto& key, const SimulatorTick& tick) {
                    return std::tie(key.first, key.second) < std::tie(tick.symbol, tick.timestamp_ms);
                });
            if (it == m_ticks.begin() || (--it)->symbol != symbol) return std::nullopt;
            return *it;
        }

    private:
        std::vector<SimulatorTick> m_ticks;
    };

    /// \brief All balances/stakes are integer units of 10^-money_decimals account currency.
    struct SimulatorAccountConfig {
        std::string account_id;
        CurrencyType currency = CurrencyType::USD;
        std::uint32_t money_decimals = 2;
        std::int64_t initial_balance_minor = 0;
    };

    /// \brief Explicit timing/freshness and lifetime record budget; no eviction or auto-retry.
    struct SimulatorExecutionConfig {
        std::int64_t open_delay_ms = 0;
        std::int64_t response_delay_ms = 0;
        std::int64_t max_quote_age_ms = 0;
        std::size_t max_records = 4096;
    };

    /// \brief Queue acceptance does not imply admission, reservation or opening.
    struct SimulatorOrder {
        std::string symbol;
        OrderType direction = OrderType::UNKNOWN;
        std::int64_t stake_minor = 0;
        std::uint32_t duration_seconds = 0;
    };

    /// \brief Core-only lifecycle facts; UNRESOLVED still owns an opened active slot.
    enum class SimulatorTradePhase { QUEUED, PENDING, OPEN, UNRESOLVED, SETTLED, FAILED_OPEN, CANCELED };
    /// \brief Why no eligible execution quote or economic result could be established.
    enum class SimulatorUnresolvedReason { NONE, MISSING_QUOTE, STALE_QUOTE, SHUTDOWN, INCOMPATIBLE_SOURCE };

    /// \brief Read-only copy of account projections; unresolved trades remain open exposure.
    struct SimulatorAccountSnapshot {
        SimulatorAccountConfig config;
        std::int64_t balance_minor = 0;
        std::int64_t available_minor = 0;
        std::int64_t reserved_minor = 0;
        std::int64_t exposure_minor = 0;
        std::size_t pending_trades = 0;
        std::size_t open_trades = 0;
    };

    /// \brief Integer result/query DTO. A virtual ID is never a real broker identity.
    struct SimulatorTrade {
        std::uint64_t command_id = 0;
        std::string virtual_trade_id; ///< Empty until reservation succeeds.
        std::string account_id;
        CurrencyType currency = CurrencyType::UNKNOWN;
        SimulatorOrder order;
        SimulatorTradePhase phase = SimulatorTradePhase::QUEUED;
        TradeState outcome = TradeState::UNKNOWN;
        TradeErrorCode error = TradeErrorCode::SUCCESS;
        SimulatorUnresolvedReason unresolved_reason = SimulatorUnresolvedReason::NONE;
        std::int64_t received_ms = 0;
        std::optional<std::int64_t> admitted_ms;
        std::optional<std::int64_t> open_ms;
        std::optional<std::int64_t> expiry_ms;
        std::optional<std::int64_t> settlement_ms;
        std::optional<std::int64_t> callback_ms;
        std::uint32_t policy_version = 0;
        std::optional<std::int64_t> payout_ppm;
        std::vector<std::string> applied_rule_ids;
        std::optional<SimulatorTick> open_quote;
        std::optional<SimulatorTick> close_quote;
        std::optional<std::int64_t> net_profit_minor; ///< Authoritative net P&L, not total return.
    };

    /// \class SimulatorCore
    /// \brief Manual-clock single-owner scenario, separate from the future SIM-4 facade.
    /// All calls are owner-thread confined. Callbacks may enqueue/query/update policy;
    /// recursive process/advance is rejected. The owner must outlive callback execution.
    class SimulatorCore {
    public:
        using callback_t = std::function<void(const SimulatorTrade&)>;
        using quote_selector_t = std::function<std::optional<SimulatorTick>(const std::string&, std::int64_t)>;

        SimulatorCore(SimulatorAccountConfig account, SimulatorExecutionConfig execution,
                      SimulatorPolicySnapshot policy, SimulatorQuoteFixture quotes,
                      std::int64_t start_ms = 0)
            : SimulatorCore(std::move(account), execution, std::move(policy),
                quote_selector_t{[fixture = std::make_shared<const SimulatorQuoteFixture>(std::move(quotes))]
                    (const std::string& symbol, std::int64_t when) { return fixture->latest(symbol, when); }}, start_ms) {}

        /// \brief Uses a host-owned as-of selector; the fixture overload remains the reference path.
        SimulatorCore(SimulatorAccountConfig account, SimulatorExecutionConfig execution,
                      SimulatorPolicySnapshot policy, quote_selector_t quotes,
                      std::int64_t start_ms = 0)
            : m_execution(execution), m_policy(std::move(policy)), m_quotes(std::move(quotes)),
              m_now(start_ms) {
            if (account.account_id.empty() || account.account_id.size() > 64 ||
                account.currency < CurrencyType::USD || account.currency > CurrencyType::KZT ||
                account.money_decimals > 8 || account.initial_balance_minor < 0 || start_ms < 0 ||
                execution.open_delay_ms < 0 || execution.response_delay_ms < 0 ||
                execution.max_quote_age_ms < 0 || execution.max_records == 0 ||
                execution.max_records > 1000000 || !m_quotes)
                throw std::invalid_argument("simulator account/execution configuration");
            m_account.config = std::move(account);
            m_account.balance_minor = m_account.config.initial_balance_minor;
            m_records.reserve(execution.max_records);
            m_events.reserve(execution.max_records);
        }

        SimulatorCore(const SimulatorCore&) = delete;
        SimulatorCore& operator=(const SimulatorCore&) = delete;

        /// \brief Enqueues one bounded command, returning a scenario-local command handle.
        /// No funds/slots are reserved until the ADMIT event runs. False/empty means no record.
        std::optional<std::uint64_t> enqueue(SimulatorOrder order, callback_t callback = {}) {
            if (m_stopped || m_shutdown_requested || m_records.size() == m_execution.max_records ||
                order.symbol.empty() || order.symbol.size() > 64) return std::nullopt;
            Record record;
            record.trade.command_id = m_records.size() + 1;
            record.trade.account_id = m_account.config.account_id;
            record.trade.currency = m_account.config.currency;
            record.trade.order = std::move(order);
            record.trade.received_ms = m_now;
            record.callback = std::move(callback);
            const auto index = m_records.size();
            m_records.push_back(std::move(record));
            schedule(m_now, EventKind::ADMIT, index);
            return m_records.back().trade.command_id;
        }

        /// \brief Replaces future opens only, requiring a strictly increasing policy version.
        bool set_policy(SimulatorPolicySnapshot policy) {
            if (m_stopped || m_shutdown_requested || policy.version() <= m_policy.version()) return false;
            m_policy = std::move(policy);
            return true;
        }

        /// \brief Executes due events at their own timestamps, then advances to target.
        bool advance_to(std::int64_t target_ms) {
            if (m_processing || m_stopped || target_ms < m_now) return false;
            m_processing = true;
            try {
                while (!m_events.empty() && m_events.front().when <= target_ms && !m_shutdown_requested) {
                    std::pop_heap(m_events.begin(), m_events.end(), Later{});
                    const auto event = m_events.back();
                    m_events.pop_back();
                    m_now = event.when;
                    auto& record = m_records[event.index];
                    switch (event.kind) {
                    case EventKind::ADMIT: admit(record, event.index); break;
                    case EventKind::OPEN: open(record, event.index); break;
                    case EventKind::SETTLE: settle(record, event.index); break;
                    case EventKind::NOTIFY: deliver(record); break;
                    }
                }
            } catch (...) {
                // Allocation failures stop the scenario without releasing opened risk.
                m_processing = false;
                shutdown();
                throw;
            }
            m_processing = false;
            if (m_shutdown_requested) shutdown();
            else m_now = target_ms;
            return true;
        }

        bool process() { return advance_to(m_now); }
        std::int64_t now_ms() const noexcept { return m_now; }
        bool is_stopped() const noexcept { return m_stopped; }
        std::size_t callback_errors() const noexcept { return m_callback_errors; }

        SimulatorAccountSnapshot account() const {
            auto result = m_account;
            result.available_minor = result.balance_minor - result.reserved_minor - result.exposure_minor;
            return result;
        }

        std::optional<SimulatorTrade> query(std::uint64_t command_id) const {
            if (command_id == 0 || command_id > m_records.size()) return std::nullopt;
            return m_records[static_cast<std::size_t>(command_id - 1)].trade;
        }

        std::optional<SimulatorTrade> query(const std::string& virtual_trade_id) const {
            if (virtual_trade_id.empty()) return std::nullopt;
            for (const auto& record : m_records)
                if (record.trade.virtual_trade_id == virtual_trade_id) return record.trade;
            return std::nullopt;
        }

        /// \brief History is command-order, including rejected commands and unresolved risk.
        std::vector<SimulatorTrade> history() const {
            std::vector<SimulatorTrade> result;
            result.reserve(m_records.size());
            for (const auto& record : m_records) result.push_back(record.trade);
            return result;
        }

        /// \brief Cancels only never-opened work; preserves opened exposure as unresolved.
        /// Flushes undelivered result notifications at shutdown time; idempotent, no async work.
        void shutdown() noexcept {
            if (m_stopped) return;
            if (m_processing) { m_shutdown_requested = true; return; }
            m_stopped = true;
            m_events.clear();
            for (auto& record : m_records) {
                auto& trade = record.trade;
                if (trade.phase == SimulatorTradePhase::PENDING) release_pending(record);
                if (trade.phase == SimulatorTradePhase::QUEUED || trade.phase == SimulatorTradePhase::PENDING) {
                    trade.phase = SimulatorTradePhase::CANCELED;
                    trade.outcome = TradeState::CANCELED_TRADE;
                    trade.error = TradeErrorCode::CANCELED_TRADE;
                } else if (trade.phase == SimulatorTradePhase::OPEN) {
                    trade.phase = SimulatorTradePhase::UNRESOLVED;
                    trade.unresolved_reason = SimulatorUnresolvedReason::SHUTDOWN;
                }
            }
            for (auto& record : m_records) deliver(record);
        }

        /// \brief Half-up rounded nonnegative stake * payout_ppm / 1000000, checked int64.
        static std::optional<std::int64_t> win_profit(std::int64_t stake, std::int64_t payout) noexcept {
            if (stake <= 0 || payout < 0) return std::nullopt;
            constexpr std::int64_t scale = 1000000;
            const auto quotient = stake / scale;
            const auto remainder = stake % scale;
            if (payout != 0 && quotient > max_money / payout) return std::nullopt;
            const auto high = quotient * payout;
            const auto middle = remainder * (payout / scale);
            const auto low = (remainder * (payout % scale) + scale / 2) / scale;
            if (middle > max_money - high || low > max_money - high - middle) return std::nullopt;
            return high + middle + low;
        }

    private:
        static constexpr std::int64_t max_money = (std::numeric_limits<std::int64_t>::max)();
        enum class EventKind { ADMIT, OPEN, SETTLE, NOTIFY };
        struct Event { std::int64_t when; std::uint64_t sequence; EventKind kind; std::size_t index; };
        struct Later {
            bool operator()(const Event& a, const Event& b) const noexcept {
                return std::tie(a.when, a.sequence) > std::tie(b.when, b.sequence);
            }
        };
        struct Record {
            SimulatorTrade trade;
            callback_t callback;
            std::int64_t maximum_profit = 0;
            bool delivered = false;
        };

        void schedule(std::int64_t when, EventKind kind, std::size_t index) {
            // Each accepted record has at most one outstanding lifecycle/notification event.
            m_events.push_back({when, ++m_sequence, kind, index});
            std::push_heap(m_events.begin(), m_events.end(), Later{});
        }

        SimulatorPolicyResult conditions(const SimulatorTrade& trade) const {
            return m_policy.resolve({trade.order.symbol, trade.order.direction,
                static_cast<std::uint32_t>((m_now / 1000) % 86400), trade.order.duration_seconds});
        }

        TradeErrorCode eligibility(const SimulatorTrade& trade, const SimulatorPolicyResult& policy,
                                   bool owns_slot) const {
            switch (policy.error) {
            case SimulatorPolicyError::NONE: break;
            case SimulatorPolicyError::INVALID_SYMBOL: return TradeErrorCode::INVALID_SYMBOL;
            case SimulatorPolicyError::INVALID_DIRECTION: return TradeErrorCode::INVALID_ORDER;
            case SimulatorPolicyError::INVALID_DURATION: return TradeErrorCode::INVALID_DURATION;
            default: return TradeErrorCode::INVALID_REQUEST;
            }
            const auto& values = policy.values;
            if (trade.order.stake_minor < *values.min_stake_minor) return TradeErrorCode::AMOUNT_TOO_LOW;
            if (trade.order.stake_minor > *values.max_stake_minor) return TradeErrorCode::AMOUNT_TOO_HIGH;
            std::size_t symbol_active = 0;
            for (const auto& record : m_records) {
                const auto phase = record.trade.phase;
                if (record.trade.order.symbol == trade.order.symbol &&
                    (phase == SimulatorTradePhase::PENDING || phase == SimulatorTradePhase::OPEN ||
                     phase == SimulatorTradePhase::UNRESOLVED)) ++symbol_active;
            }
            const auto active = m_account.pending_trades + m_account.open_trades;
            // On delayed open, the caller's already reserved slot is part of these counts.
            if ((values.max_active_global && (owns_slot ? active > *values.max_active_global : active >= *values.max_active_global)) ||
                (values.max_active_symbol && (owns_slot ? symbol_active > *values.max_active_symbol : symbol_active >= *values.max_active_symbol)))
                return TradeErrorCode::LIMIT_OPEN_TRADES;
            return TradeErrorCode::SUCCESS;
        }

        void fail(Record& record, std::size_t index, TradeErrorCode error) {
            if (record.trade.phase == SimulatorTradePhase::PENDING) release_pending(record);
            record.trade.phase = SimulatorTradePhase::FAILED_OPEN;
            record.trade.outcome = TradeState::OPEN_ERROR;
            record.trade.error = error;
            schedule(m_now, EventKind::NOTIFY, index);
        }

        void admit(Record& record, std::size_t index) {
            auto& trade = record.trade;
            if (trade.phase != SimulatorTradePhase::QUEUED) return;
            const auto error = eligibility(trade, conditions(trade), false);
            if (error != TradeErrorCode::SUCCESS) { fail(record, index, error); return; }
            const auto occupied = m_account.reserved_minor + m_account.exposure_minor;
            if (trade.order.stake_minor > m_account.balance_minor - occupied) {
                fail(record, index, TradeErrorCode::INSUFFICIENT_BALANCE); return;
            }
            if (m_execution.open_delay_ms > max_money - m_now) {
                fail(record, index, TradeErrorCode::INVALID_EXPIRY_TIME); return;
            }
            const auto when = m_now + m_execution.open_delay_ms;
            const auto duration_ms = static_cast<std::int64_t>(trade.order.duration_seconds) * 1000;
            if (duration_ms > max_money - when || m_execution.response_delay_ms > max_money - when - duration_ms) {
                fail(record, index, TradeErrorCode::INVALID_EXPIRY_TIME); return;
            }
            // Construct identity before mutating ledger; allocation failure cannot leak reserve.
            auto identity = "sim/" + trade.account_id + "/" + std::to_string(trade.command_id);
            trade.virtual_trade_id = std::move(identity);
            trade.admitted_ms = m_now;
            trade.phase = SimulatorTradePhase::PENDING;
            trade.outcome = TradeState::WAITING_OPEN;
            m_account.reserved_minor += trade.order.stake_minor;
            ++m_account.pending_trades;
            schedule(when, EventKind::OPEN, index);
        }

        void release_pending(Record& record) noexcept {
            m_account.reserved_minor -= record.trade.order.stake_minor;
            --m_account.pending_trades;
        }

        std::optional<SimulatorTick> quote(SimulatorTrade& trade) const {
            auto tick = m_quotes(trade.order.symbol, m_now);
            if (tick && (tick->symbol != trade.order.symbol || tick->price_units <= 0 ||
                         tick->timestamp_ms < 0 || tick->timestamp_ms > m_now ||
                         tick->received_ms < 0 || tick->received_ms > m_now))
                tick.reset();
            if (!tick) trade.unresolved_reason = SimulatorUnresolvedReason::MISSING_QUOTE;
            else if (m_now - tick->timestamp_ms > m_execution.max_quote_age_ms) {
                trade.unresolved_reason = SimulatorUnresolvedReason::STALE_QUOTE;
                tick.reset();
            }
            else if (trade.open_quote &&
                std::tie(tick->provider_key, tick->provider_instance, tick->source_generation,
                         tick->price_decimals, tick->price_side) !=
                std::tie(trade.open_quote->provider_key, trade.open_quote->provider_instance,
                         trade.open_quote->source_generation, trade.open_quote->price_decimals,
                         trade.open_quote->price_side)) {
                trade.unresolved_reason = SimulatorUnresolvedReason::INCOMPATIBLE_SOURCE;
                tick.reset();
            }
            return tick;
        }

        void open(Record& record, std::size_t index) {
            auto& trade = record.trade;
            if (trade.phase != SimulatorTradePhase::PENDING) return;
            auto policy = conditions(trade);
            const auto error = eligibility(trade, policy, true);
            if (error != TradeErrorCode::SUCCESS) { fail(record, index, error); return; }
            auto tick = quote(trade);
            if (!tick) { fail(record, index, TradeErrorCode::NO_CONNECTION); return; }
            const auto profit = win_profit(trade.order.stake_minor, *policy.values.payout_ppm);
            if (!profit || *profit > max_money - m_account.balance_minor - m_profit_headroom) {
                fail(record, index, TradeErrorCode::INVALID_REQUEST); return;
            }
            trade.applied_rule_ids = std::move(policy.applied_rule_ids);
            trade.open_quote = std::move(tick);
            trade.policy_version = m_policy.version();
            trade.payout_ppm = policy.values.payout_ppm;
            trade.open_ms = m_now;
            trade.expiry_ms = m_now + static_cast<std::int64_t>(trade.order.duration_seconds) * 1000;
            record.maximum_profit = *profit;
            release_pending(record);
            m_account.exposure_minor += trade.order.stake_minor;
            ++m_account.open_trades;
            m_profit_headroom += *profit;
            trade.phase = SimulatorTradePhase::OPEN;
            trade.outcome = TradeState::OPEN_SUCCESS;
            schedule(*trade.expiry_ms, EventKind::SETTLE, index);
        }

        void settle(Record& record, std::size_t index) {
            auto& trade = record.trade;
            if (trade.phase != SimulatorTradePhase::OPEN) return;
            auto tick = quote(trade);
            if (!tick) {
                trade.phase = SimulatorTradePhase::UNRESOLVED;
                schedule(m_now + m_execution.response_delay_ms, EventKind::NOTIFY, index);
                return;
            }
            const auto before = trade.open_quote->price_units;
            const auto after = tick->price_units;
            const bool win = trade.order.direction == OrderType::BUY ? after > before : after < before;
            const auto profit = after == before ? 0 : win ? record.maximum_profit : -trade.order.stake_minor;
            trade.close_quote = std::move(tick);
            trade.net_profit_minor = profit;
            trade.settlement_ms = m_now;
            trade.outcome = after == before ? TradeState::STANDOFF : win ? TradeState::WIN : TradeState::LOSS;
            // Opening reserved headroom for every possible win; both additions are safe here.
            m_account.balance_minor += profit;
            m_account.exposure_minor -= trade.order.stake_minor;
            --m_account.open_trades;
            m_profit_headroom -= record.maximum_profit;
            trade.phase = SimulatorTradePhase::SETTLED;
            schedule(m_now + m_execution.response_delay_ms, EventKind::NOTIFY, index);
        }

        void deliver(Record& record) noexcept {
            if (record.delivered) return;
            record.delivered = true;
            record.trade.callback_ms = m_now;
            // Release the closure after invocation, including exceptions; no delayed raw-this captures.
            auto callback = std::move(record.callback);
            if (callback) {
                try { callback(record.trade); }
                catch (...) { ++m_callback_errors; }
            }
        }

        SimulatorExecutionConfig m_execution;
        SimulatorPolicySnapshot m_policy;
        quote_selector_t m_quotes;
        SimulatorAccountSnapshot m_account;
        std::vector<Record> m_records;
        std::vector<Event> m_events;
        std::int64_t m_now = 0;
        std::int64_t m_profit_headroom = 0;
        std::uint64_t m_sequence = 0;
        std::size_t m_callback_errors = 0;
        bool m_processing = false;
        bool m_shutdown_requested = false;
        bool m_stopped = false;
    };

} // namespace optionx::platforms

#endif // OPTIONX_HEADER_PLATFORMS_SIMULATOR_PLATFORM_SIMULATOR_CORE_HPP_INCLUDED
