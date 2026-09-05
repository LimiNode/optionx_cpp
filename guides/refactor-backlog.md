# Refactor Backlog

This file tracks remaining follow-up work after the 2026 refactor-audit PR
series. Keep it short and remove items once they are handled.

## Next PR Candidates

- Extend market-data continuity beyond the first bar-only route implementation:
  define provider support for tick history, retries, and a documented
  history-to-live boundary for each provider.
- Add robust gap recovery policy with provider-aware retry/backoff, sequence or
  timestamp validation, and an explicit deduplication policy for overlapping
  historical, backfill, and live bar snapshots.
- Add route-scoped continuity metrics and failure visibility for applications
  that need to prove that a chart or strategy has a complete time series.
- Replace the dense-bar assumption with an explicit provider completeness
  capability or `range_complete` history result for session-based markets.
- Allow reconnect candle boundaries to use a broker-aligned clock instead of
  relying only on the application wall clock.
- Add a fuller CMake package/export story for consumers that do not use the
  project as a direct submodule. The current `optionx_cpp::optionx_cpp`
  interface target covers build-tree/submodule consumption.

## Explicitly Deferred

- Add a generic tick-history provider contract. Current continuity support is
  intentionally bar-first because providers expose bar history only.
- Continue generation-safe lifecycle hardening for legacy bridge transports
  when their behavior is changed; do not mix that work into market-data API
  PRs.
- `TradeUpPlatform` remains a partial implementation. Do not refactor it as
  part of generic cleanup PRs unless the task is specifically about TradeUp.
