# Refactor Backlog

This file tracks remaining follow-up work after the 2026 refactor-audit PR
series. Keep it short and remove items once they are handled.

## Completed In The Market-Data Refactor

- Bar continuity now has provider history prefill, bounded gap and reconnect
  recovery, retry/backoff, timestamp/range validation, clipping, overlap
  deduplication, sticky trust-loss, generation-safe async completion, and
  reconnect boundary handling. It remains a bar-first implementation.
- Continuity observability now exposes route snapshots with phase/status,
  ranges, watermarks, buffer pressure, request/retry/failure counters, and
  monotonic stale/degraded durations.
- The generic tick-history foundation now has typed timestamp-range request and
  result DTOs, a provider hook, ordering/range validation, explicit
  completeness semantics, and a batch adapter.
- Intrade Bar now exposes a bounded, session-scoped observed-tick archive fed by
  `/price_now`. It preserves distinct same-second snapshots and reports
  `range_complete` only for proven one-second coverage; it is not a persistent
  or authoritative broker tick archive.

## Next PR Candidates

- Integrate Intrade's observed-tick archive with Router continuity while keeping
  incomplete ranges fail-closed and documenting the session/retention limit.
- Add an authoritative provider tick-history implementation only if a broker
  later exposes one; do not treat `trade_check2.php` as a range-history API.
- Replace the dense-bar assumption with an explicit provider completeness
  capability or `range_complete` history result for session-based markets.
- Allow reconnect candle boundaries to use a broker-aligned clock instead of
  relying only on the application wall clock.
- Add a fuller CMake package/export story for consumers that do not use the
  project as a direct submodule. The current `optionx_cpp::optionx_cpp`
  interface target covers build-tree/submodule consumption.

## Explicitly Deferred

- Continue generation-safe lifecycle hardening for legacy bridge transports
  when their behavior is changed; do not mix that work into market-data API
  PRs.
- `TradeUpPlatform` sources remain examples for a retired broker. Do not
  refactor them as part of generic cleanup PRs.
