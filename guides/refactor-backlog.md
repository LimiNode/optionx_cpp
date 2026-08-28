# Refactor Backlog

This file tracks remaining follow-up work after the 2026 refactor-audit PR
series. Keep it short and remove items once they are handled.

## Next PR Candidates

- Add `MarketDataRouter` above provider subscriptions. The router should own
  provider handles, expose move-only RAII subscription handles, correlate
  statuses with concrete subscriptions, and replay the current stream status
  to late subscriptions.
- Add `MarketDataSubscriberBase` as optional convenience API for bots and
  charts that subscribe from their own methods and retain router handles.
- Publish real broker/platform payout, expiry, amount-limit, and market-open
  changes through `TradingConditionUpdate` instead of using the hub only as a
  manually populated snapshot cache.
- Add a fuller CMake package/export story for consumers that do not use the
  project as a direct submodule. The current `optionx_cpp::optionx_cpp`
  interface target covers build-tree/submodule consumption.

## Explicitly Deferred

- Finalize live bars from platform time/process even when no tick arrives for
  the next bar. Tick-driven aggregation alone cannot close an idle stream.
- Remove `SingleTick` from the internal price event path after all remaining
  parser/manager consumers use `TickUpdateBatch` directly.
- Continue generation-safe lifecycle hardening for legacy bridge transports
  when their behavior is changed; do not mix that work into market-data API
  PRs.
- `TradeUpPlatform` remains a partial implementation. Do not refactor it as
  part of generic cleanup PRs unless the task is specifically about TradeUp.
