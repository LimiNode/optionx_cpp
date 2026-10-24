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
- Bar continuity now consumes explicit provider completeness proof instead of
  inferring dense coverage from returned candle count. Session-aware providers
  may prove sparse ranges, while unproved results remain degraded.
- Reconnect bar recovery uses a fresh provider-aligned wall-clock estimate for
  the last closed candle and falls back to the application wall clock when the
  estimate is unavailable. Retry, timeout, and duration logic remains monotonic.
- Router tick continuity now uses that provider history contract for opt-in
  `PREFILL` and `PREFILL_AND_RECOVER` routes. It buffers live ticks, treats
  `range_complete` as the authority, retries transport failures through
  `process()`, performs bounded recovery, preserves distinct same-second
  events, removes only exact overlaps, and keeps incomplete history
  `DEGRADED`.

## Next PR Candidates

- Add an authoritative provider tick-history implementation only if a broker
  later exposes one; do not treat `trade_check2.php` as a range-history API.
- Add a fuller CMake package/export story for consumers that do not use the
  project as a direct submodule. The current `optionx_cpp::optionx_cpp`
  interface target covers build-tree/submodule consumption.

## Explicitly Deferred

- Continue generation-safe lifecycle hardening for legacy bridge transports
  when their behavior is changed; do not mix that work into market-data API
  PRs.
- `TradeUpPlatform` is a legacy/retired unsupported integration retained for
  historical reference. Do not repair, extend, or refactor it unless support
  is explicitly reactivated.
