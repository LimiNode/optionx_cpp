#pragma once
#ifndef OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_OBSERVED_TICK_HISTORY_HPP_INCLUDED
#define OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_OBSERVED_TICK_HISTORY_HPP_INCLUDED

/// \file ObservedTickHistory.hpp
/// \brief Defines the bounded in-memory history of Intrade polling observations.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace optionx::platforms::intrade_bar {

    /// \struct IntradeObservedTickHistoryOptions
    /// \brief Bounds the in-memory Intrade polling snapshot archive.
    struct IntradeObservedTickHistoryOptions {
        /// Maximum number of distinct observations retained per symbol.
        std::size_t max_items_per_symbol = 10000;

        /// Maximum broker-time span accepted by one history query.
        /// Zero disables this query-size limit.
        std::uint64_t max_lookback_ms = 7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;

        /// Expected granularity of Intrade polling timestamps.
        /// It is used only to make a conservative completeness assertion.
        std::uint64_t sampling_interval_ms = 1000;

        /// \brief Returns true when the archive can be used safely.
        [[nodiscard]] bool valid() const noexcept {
            return max_items_per_symbol > 0 && sampling_interval_ms > 0;
        }
    };

    /// \class IntradeObservedTickHistory
    /// \brief Stores bounded one-second Intrade polling observations.
    ///
    /// `/price_now` is a current-price snapshot rather than a broker history
    /// endpoint. This archive makes observations available to consumers after
    /// they have been collected during the current authenticated session. It
    /// is intentionally non-persistent and never fabricates samples that were
    /// not observed by the polling path.
    class IntradeObservedTickHistory final {
    public:
        /// \brief Constructs an archive with bounded retention.
        explicit IntradeObservedTickHistory(
                IntradeObservedTickHistoryOptions options = {})
                : m_options(std::move(options)) {}

        /// \brief Records polling batches, preserving distinct same-second ticks.
        /// \param batches Tick batches produced by the Intrade polling request.
        void record(const std::vector<events::TickUpdateBatch>& batches) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& batch : batches) {
                if (batch.symbol.empty()) continue;

                auto& history = m_symbols[batch.symbol];
                history.provider = batch.provider.empty()
                    ? "INTRADE_BAR"
                    : batch.provider;
                history.price_digits = batch.price_digits;
                history.volume_digits = batch.volume_digits;

                for (const auto& tick : batch.items) {
                    if (tick.time_ms == 0 || contains_observation(history, tick)) {
                        continue;
                    }
                    history.items.push_back(StoredTick{tick});
                }
                prune(history);
            }
        }

        /// \brief Returns observations in an inclusive broker-time range.
        /// \details `range_complete` is true only when the request is aligned
        ///          to the configured sampling interval and every expected
        ///          interval has an archived observation. An ordinary archive
        ///          miss therefore remains a successful but incomplete result.
        [[nodiscard]] TickHistoryResult fetch(
                const TickHistoryRequest& request) const {
            if (!request.valid()) {
                return TickHistoryResult::fail(
                    "Invalid Intrade observed tick history request.");
            }
            if (m_options.max_lookback_ms > 0 &&
                request.to_time_ms - request.from_time_ms > m_options.max_lookback_ms) {
                return TickHistoryResult::fail(
                    "Intrade observed tick history request exceeds the configured lookback.");
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            TickSequence sequence;
            sequence.symbol = request.symbol;

            const auto symbol_it = m_symbols.find(request.symbol);
            if (symbol_it == m_symbols.end()) {
                return TickHistoryResult::ok(std::move(sequence), false);
            }

            const auto& history = symbol_it->second;
            sequence.provider = history.provider;
            sequence.price_digits = history.price_digits;
            sequence.volume_digits = history.volume_digits;
            sequence.ticks.reserve(history.items.size());
            for (const auto& stored : history.items) {
                if (stored.tick.time_ms >= request.from_time_ms &&
                    stored.tick.time_ms <= request.to_time_ms) {
                    sequence.ticks.push_back(stored.tick);
                }
            }

            std::stable_sort(
                sequence.ticks.begin(),
                sequence.ticks.end(),
                [](const Tick& lhs, const Tick& rhs) {
                    return lhs.time_ms < rhs.time_ms;
                });

            const bool complete = has_complete_coverage(
                sequence.ticks,
                request.from_time_ms,
                request.to_time_ms);
            return TickHistoryResult::ok(std::move(sequence), complete);
        }

        /// \brief Removes all observations collected by this archive.
        void clear() noexcept {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_symbols.clear();
        }

        /// \brief Returns the number of retained observations for a symbol.
        [[nodiscard]] std::size_t size(const std::string& symbol) const {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_symbols.find(symbol);
            return it == m_symbols.end() ? 0U : it->second.items.size();
        }

        /// \brief Returns the immutable archive options.
        [[nodiscard]] const IntradeObservedTickHistoryOptions& options() const noexcept {
            return m_options;
        }

    private:
        struct StoredTick {
            Tick tick;
        };

        struct SymbolHistory {
            std::deque<StoredTick> items;
            std::string provider;
            std::uint32_t price_digits = 0;
            std::uint32_t volume_digits = 0;
        };

        static bool same_market_observation(
                const Tick& lhs,
                const Tick& rhs) noexcept {
            return lhs.ask == rhs.ask &&
                lhs.bid == rhs.bid &&
                lhs.last == rhs.last &&
                lhs.volume == rhs.volume &&
                lhs.time_ms == rhs.time_ms;
        }

        static bool contains_observation(
                const SymbolHistory& history,
                const Tick& tick) noexcept {
            return std::any_of(
                history.items.begin(),
                history.items.end(),
                [&tick](const StoredTick& stored) {
                    return same_market_observation(stored.tick, tick);
                });
        }

        void prune(SymbolHistory& history) {
            const auto max_items = m_options.max_items_per_symbol;
            while (history.items.size() > max_items) {
                const auto oldest = std::min_element(
                    history.items.begin(),
                    history.items.end(),
                    [](const StoredTick& lhs, const StoredTick& rhs) {
                        return lhs.tick.time_ms < rhs.tick.time_ms;
                    });
                history.items.erase(oldest);
            }

            if (m_options.max_lookback_ms == 0 || history.items.empty()) return;

            const auto newest = std::max_element(
                history.items.begin(),
                history.items.end(),
                [](const StoredTick& lhs, const StoredTick& rhs) {
                    return lhs.tick.time_ms < rhs.tick.time_ms;
                })->tick.time_ms;
            const auto cutoff = newest > m_options.max_lookback_ms
                ? newest - m_options.max_lookback_ms
                : 0U;
            history.items.erase(
                std::remove_if(
                    history.items.begin(),
                    history.items.end(),
                    [cutoff](const StoredTick& stored) {
                        return stored.tick.time_ms < cutoff;
                    }),
                history.items.end());
        }

        bool has_complete_coverage(
                const std::vector<Tick>& ticks,
                std::uint64_t from_time_ms,
                std::uint64_t to_time_ms) const noexcept {
            const auto interval = m_options.sampling_interval_ms;
            if (interval == 0 || from_time_ms % interval != 0 ||
                to_time_ms % interval != 0) {
                return false;
            }

            std::size_t tick_index = 0;
            std::uint64_t expected = from_time_ms;
            while (true) {
                while (tick_index < ticks.size() &&
                       ticks[tick_index].time_ms < expected) {
                    ++tick_index;
                }
                if (tick_index == ticks.size() ||
                    ticks[tick_index].time_ms != expected) {
                    return false;
                }
                if (expected == to_time_ms) return true;
                if (expected > std::numeric_limits<std::uint64_t>::max() - interval) {
                    return false;
                }
                expected += interval;
            }
        }

        IntradeObservedTickHistoryOptions m_options;
        mutable std::mutex m_mutex;
        std::unordered_map<std::string, SymbolHistory> m_symbols;
    };

} // namespace optionx::platforms::intrade_bar

#endif // OPTIONX_HEADER_PLATFORMS_INTRADE_BAR_PLATFORM_OBSERVED_TICK_HISTORY_HPP_INCLUDED
