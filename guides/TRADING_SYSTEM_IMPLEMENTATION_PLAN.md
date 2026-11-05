# Trading System Implementation Plan

## Delivery Rule

The new domain is delivered as small, testable batches. Batches 1 through 4 are
merged in `main`. Batch 5 is deliberately split into architecture, durable
storage, and recovery stages. Each stage must preserve the existing broker,
Router, task and storage contracts and must not open live trades.

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

The in-memory implementation and its terminality, replay and partial-submission
rules are described in [Trading System Batch 3](trading-system-batch3.md).

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

The in-memory contract and lifecycle rules are documented in
[Trading System Batch 4](trading-system-batch4.md).

## Batch 5: Persistence and Recovery

Batch 5 is governed by [Batch 5A architecture decisions](trading-system-batch5a.md)
and is delivered in three separate review gates.

### Batch 5A: Architecture and Contracts

Document account identity and aliases, risk scopes, multi-account execution
rosters, native currency and valuation provenance, money-management state,
module composition, journal ownership, MDBX transaction ownership and recovery
boundaries. Separate existing in-memory owners from missing durable capabilities,
define owner hydration and per-risk-scope admission gates, and specify how
cross-currency physical settlements remain non-terminal without valuation facts.
Do not add runtime code or schema migrations. Stop for review.

### Batch 5B: Durable Storage and Local Atomicity

After 5A approval, extend existing storage through an explicit shared
environment/transaction adapter or narrowly scoped named tables. Preserve the
standalone `TradeRecordDB` and `SignalRecordDB` APIs. Implement durable
idempotency, revisions, roster/reservation facts and one local Unit of Work for
settlement application. Do not claim atomicity with a broker call.

### Batch 5C: Simulated Recovery and Reconciliation

After 5B approval, add an offline simulated execution/reconciliation adapter.
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

Batch 5A, 5B and 5C must remain separate Draft PRs until each review gate is
accepted. None of them includes live Polymarket integration, an exchange
orderbook/position engine, a GUI/graph editor, an FX aggregation engine, or a
new scheduler/event bus.
