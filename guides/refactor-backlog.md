# Refactor Backlog

This file tracks remaining follow-up work after the 2026 refactor-audit PR
series. Keep it short and remove items once they are handled.

## Next PR Candidates

- Add route-scoped continuity metrics and failure visibility for applications
  that need to prove that a chart or strategy has a complete time series.
- Add a generic tick-history provider contract and extend continuity from bars
  to ticks where a provider can supply historical tick data.
- Add a fuller CMake package/export story for consumers that do not use the
  project as a direct submodule. The current `optionx_cpp::optionx_cpp`
  interface target covers build-tree/submodule consumption.

## Explicitly Deferred

- `TradeUpPlatform` sources remain historical examples. The broker is no
  longer available, so do not add new production work or generic cleanup for
  this platform.
- Legacy bridge lifecycle hardening is complete. Preserve the existing
  generation, callback, and shutdown patterns when changing those bridges, but
  do not track the already completed audit as active work here.
