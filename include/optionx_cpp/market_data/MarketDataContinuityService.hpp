#pragma once
#ifndef OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_SERVICE_HPP_INCLUDED
#define OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_SERVICE_HPP_INCLUDED

/// \file MarketDataContinuityService.hpp
/// \brief Defines helpers for routing historical bars through market-data batches.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace optionx::market_data {

    /// \class MarketDataContinuityService
    /// \brief Bridges historical bar requests into the live market-data batch pipeline.
    ///
    /// The service intentionally stays thin: providers still own transport,
    /// retry, and stream lifecycle. This helper only tags recovered payloads
    /// as historical/backfill data and packages them into BarDataBatch objects.
    class MarketDataContinuityService {
    public:
        /// \brief Callback that receives a failed history request.
        using error_callback_t = std::function<void(BarHistoryResult)>;

        /// \brief Constructs the service around a market-data provider.
        /// \param provider Provider used for historical data requests.
        explicit MarketDataContinuityService(BaseMarketDataProvider& provider)
            : m_provider(provider) {}

        /// \brief Converts a non-negative Unix-second value to milliseconds.
        /// \param seconds Unix timestamp in seconds.
        /// \return Saturated millisecond timestamp, or zero for non-positive input.
        static std::uint64_t seconds_to_milliseconds(
                std::int64_t seconds) noexcept {
            if (seconds <= 0) return 0;
            const auto value = static_cast<std::uint64_t>(seconds);
            if (value > std::numeric_limits<std::uint64_t>::max() / 1000U) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            return value * 1000U;
        }

        /// \brief Builds a count-based prefill request for a bar subscription.
        /// \param request Live bar subscription whose symbol and timeframe are reused.
        /// \param now_ms Current Unix timestamp in milliseconds.
        /// \param bars Number of inclusive timeframe slots requested before
        ///        the live boundary. Providers may return fewer bars when
        ///        data is unavailable for part of the requested range.
        /// \return A provider history request using Unix-second boundaries.
        static BarHistoryRequest make_prefill_request(
                const BarSubscriptionRequest& request,
                std::uint64_t now_ms,
                std::size_t bars) {
            const auto timeframe_ms = safe_multiply(
                static_cast<std::uint64_t>(request.timeframe), 1000U);
            const auto depth_ms = safe_multiply(
                timeframe_ms,
                bars > 0 ? bars - 1 : 0);
            const auto from_ms = now_ms > depth_ms ? now_ms - depth_ms : 1U;

            return make_history_request(request, from_ms, now_ms);
        }

        /// \brief Builds a bounded request for a missing bar range.
        /// \param request Live bar subscription whose symbol and price source are reused.
        /// \param from_ms Inclusive start of the missing range.
        /// \param to_ms Inclusive end of the missing range.
        /// \param max_bars Optional upper bound for one backfill operation.
        /// \return A provider history request using Unix-second boundaries.
        static BarHistoryRequest make_gap_request(
                const BarSubscriptionRequest& request,
                std::uint64_t from_ms,
                std::uint64_t to_ms,
                std::size_t max_bars = 0) {
            if (max_bars > 0 && to_ms >= from_ms && request.timeframe > 0) {
                const auto span_ms = safe_multiply(
                    safe_multiply(
                        static_cast<std::uint64_t>(request.timeframe),
                        1000U),
                    max_bars - 1);
                const auto bounded_to_ms = from_ms >
                        std::numeric_limits<std::uint64_t>::max() - span_ms
                    ? std::numeric_limits<std::uint64_t>::max()
                    : from_ms + span_ms;
                to_ms = std::min(to_ms, bounded_to_ms);
            }
            return make_history_request(request, from_ms, to_ms);
        }

        /// \brief Requests historical bars and delivers them as one batch.
        /// \param request Historical bar range to fetch.
        /// \param subscription Optional live subscription related to the backfill.
        /// \param callback Batch callback used by the consumer pipeline.
        /// \param error_callback Optional callback for typed fetch failures.
        /// \param backfill_marks Whether to add the BACKFILL flag in addition to HISTORICAL.
        /// \return True if the provider accepted the history request.
        bool request_bar_history_batch(
                BarHistoryRequest request,
                MarketDataSubscriptionHandle subscription,
                BaseMarketDataProvider::bars_callback_t callback,
                error_callback_t error_callback = nullptr,
                bool backfill_marks = true) {
            if (!callback) return false;

            const auto callback_request = request;
            return m_provider.fetch_bar_history(
                request,
                [request = callback_request,
                 subscription = std::move(subscription),
                 callback = std::move(callback),
                 error_callback = std::move(error_callback),
                 backfill_marks](BarHistoryResult result) mutable {
                    if (!result) {
                        if (error_callback) {
                            error_callback(std::move(result));
                        }
                        return;
                    }

                    auto batch = make_bar_batch(
                        std::move(result.sequence),
                        request,
                        std::move(subscription),
                        backfill_marks);
                    callback(std::move(batch));
                });
        }

        /// \brief Converts a historical bar sequence into a market-data batch.
        /// \param sequence Historical sequence returned by a provider.
        /// \param request Original request used as metadata fallback.
        /// \param subscription Optional related live subscription handle.
        /// \param backfill_marks Whether to add the BACKFILL flag.
        /// \return Batch ready for delivery to bar consumers.
        static std::unique_ptr<BarDataBatch> make_bar_batch(
                BarSequence sequence,
                const BarHistoryRequest& request,
                MarketDataSubscriptionHandle subscription = {},
                bool backfill_marks = true) {
            auto batch = std::make_unique<BarDataBatch>();
            batch->subscription = std::move(subscription);
            batch->type = MarketDataType::BARS;
            batch->symbol = sequence.symbol.empty() ? request.symbol : sequence.symbol;
            batch->timeframe = sequence.timeframe > 0 ? sequence.timeframe : request.timeframe;
            batch->price_digits = sequence.price_digits;
            batch->volume_digits = sequence.volume_digits;
            batch->items = std::move(sequence.bars);

            const auto price_type = market_price_type_from_bar_price_source(
                sequence.price_source == BarPriceSource::UNKNOWN
                    ? request.price_source
                    : sequence.price_source);

            for (auto& bar : batch->items) {
                bar.set_flag(MarketDataFlags::HISTORICAL);
                bar.set_flag(MarketDataFlags::BACKFILL, backfill_marks);
                bar.set_price_type(price_type);
            }

            return batch;
        }

    private:
        static std::uint64_t safe_multiply(
                std::uint64_t left,
                std::uint64_t right) noexcept {
            if (left == 0 || right == 0) return 0;
            if (left > std::numeric_limits<std::uint64_t>::max() / right) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            return left * right;
        }

        static BarHistoryRequest make_history_request(
                const BarSubscriptionRequest& request,
                std::uint64_t from_ms,
                std::uint64_t to_ms) {
            BarHistoryRequest history;
            history.symbol = request.symbol;
            history.timeframe = request.timeframe;
            const auto max_seconds = static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max());
            const auto from_seconds = from_ms / 1000U;
            const auto to_seconds = to_ms / 1000U;
            history.from_ts = static_cast<std::int64_t>(std::min(
                from_seconds,
                max_seconds));
            history.to_ts = static_cast<std::int64_t>(std::min(
                to_seconds,
                max_seconds));
            history.price_source = request.price_source;
            return history;
        }

        BaseMarketDataProvider& m_provider; ///< Provider used for history fetches.
    };

} // namespace optionx::market_data

#endif // OPTIONX_HEADER_MARKET_DATA_MARKET_DATA_CONTINUITY_SERVICE_HPP_INCLUDED
