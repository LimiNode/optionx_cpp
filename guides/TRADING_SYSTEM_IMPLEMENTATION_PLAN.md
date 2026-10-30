# Trading System Implementation Plan

## Delivery Rule

The new domain is delivered as small, testable batches. The current batch is
the architecture/reuse audit only. It adds no production trading runtime and
does not open live trades. Each later batch must preserve the existing broker,
Router, task and storage contracts.

## Batch 0: Foundation Audit

Deliverables:

- architecture boundary and adapter rules;
- reuse matrix for existing DTOs, platform APIs, Router and storage;
- proposed lifecycle, context, branching and risk contracts;
- staged implementation plan.

Acceptance:

- no dependency on ADELIA or MegaConnector in OptionX;
- no second scheduler, event bus, graph runtime or storage engine;
- unresolved execution is explicitly different from cancellation;
- existing `trade_id` and broker identity paths are preserved.

## Batch 1: Identity and Pure Domain DTOs

Add the smallest tested representation for `TradeIntent`, `TradeSeries` and
execution-group identity. Keep `TradeSignal`, `TradeRequest`, `TradeResult` and
`TradeRecord` source compatibility where possible.

Tests must cover cloning, serialization compatibility, invalid IDs, revision
checks and the four cardinality policies. No broker calls are involved.

## Batch 2: Stateless Policies

Implement pure filters and sizing policies first:

- symbol/source filter;
- trading-window filter;
- fixed stake;
- percent-of-balance sizing with explicit unavailable/stale behavior.

Every policy consumes a deterministic context and returns a typed reason on
rejection. Existing MM enums and parameter interfaces are reused; formula
implementations are not copied into broker managers.

## Batch 3: Series and Idempotent Progression

Implement a minimal `TradeSeries` state machine and progression policy. The
first scenario must prove:

- a rejected intent does not count as a loss;
- a duplicate settlement does not advance twice;
- a revision conflict is rejected;
- a split group waits for every required member;
- an unknown member blocks progression.

Martingale is a later policy test, not a reason to weaken the lifecycle.

## Batch 4: Risk Admission

Add a single owner for reservations and a fake/simulated account context.
Prove that two concurrent candidates cannot reserve the same available risk.
Prove that rejected-before-submit releases risk while unknown execution keeps
it reserved. Do not couple this batch to Intrade HTTP details.

## Batch 5: Persistence and Recovery

Extend existing storage through an explicit adapter or narrowly scoped tables.
The recovery test must simulate a crash at each boundary:

1. before admission;
2. after reservation;
3. after request persistence and before broker submission;
4. after submission and before broker ID;
5. after broker ID and before result;
6. after result before settlement application.

Restart must find unresolved operations, reconcile them, and apply a settlement
identity at most once. No automatic retry is permitted for unresolved broker
submission until reconciliation says it is safe.

## Batch 6: Synthetic Vertical Slice

The first end-to-end test is:

```text
SyntheticSignal
  -> SymbolFilter
  -> TimeFilter
  -> FixedStake
  -> RiskAdmission
  -> SimulatedExecution
  -> TradeOutcome
  -> PersistentState
```

The same slice then adds one martingale step, one filter rejection and a
restart/recovery cycle. It must run as ordinary C++ with no GUI, graph engine,
real account or broker network.

## Batch 7: Host and Broker Adapters

Only after the synthetic slice is stable:

- map ADELIA/MegaConnector nodes to small OptionX capabilities;
- connect existing platform execution through the intent/group adapter;
- preserve owner-loop dispatch and staged shutdown;
- add broker-specific reconciliation paths where the broker provides enough
  identity to make them safe.

This batch must not move strategy formulas into `IntradeBarPlatform`.

## Review Gates

Every batch requires:

- focused unit/regression tests;
- a compile consumer through supported aggregate headers;
- an explicit negative test for cancellation, unknown result and duplicate
  delivery where relevant;
- no secret or live-account material in fixtures;
- a diff review showing that unrelated user changes were preserved.

The first production PR after this audit should therefore be Batch 1 only. It
should not include Kelly, Labouchere, news providers, GUI integration or a
graph runtime.
