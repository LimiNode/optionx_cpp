# Trading System Implementation Plan

## Delivery Rule

The new domain is delivered as small, testable batches. Batches 1 through 6,
including the separate 5A architecture, 5B storage and 5C recovery stages, are
merged in `main`. Batch 6 merged through PR #192 as
`47ec37b825da9163bfa7ed3fe09f2d4dff0d1797`; its post-merge
[Ubuntu](https://github.com/LimiNode/optionx_cpp/actions/runs/44958086612) and
[Windows](https://github.com/LimiNode/optionx_cpp/actions/runs/44958086599) smoke
workflows passed on that exact SHA. This establishes the offline domain, not
production runtime readiness. SyntheticHost remains test support.

Batch 7A merged through PR #193 as `e81a43db85a31d6828fac72422e0fc5d384e718a`.
[Batch 7B](trading-system-batch7b.md) merged through PR #194 as
`6e5f0adac14ddb5aa135dc5b67a2d108a2d7f1cc`.
[Batch 7C-A](trading-system-batch7c-a.md) merged through PR #195 as
`b6f293833bd724075767d3ad31738d9ec245fde6`.
[Batch 7C-B](trading-system-batch7c-b.md) adds bound durable receipts and offline
known-ID queries. [7D-B](trading-system-batch7d-b.md) integrates the offline
platform owner and existing logical completion; it is not a live runtime.
The next architecture gate is [7E-0](trading-system-batch7e.md): approve durable
delivery contracts before the separate outbox (7E-A) and recovery-index (7E-B)
code PRs. Neither is authorized for automatic merge or production execution.
Every later stage must
preserve the existing broker, Router, task and storage contracts; no live
trading follows automatically from these milestones.

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

The same slice now adds one persisted loss step, filter and risk rejection,
real MDBX reopen, four crash boundaries and a multi-account late-member group.
It runs as ordinary C++17 with no GUI, graph engine, real account or broker
network. See [Trading System Batch 6](trading-system-batch6.md). The bounded
`LossStepStake` policy is reusable but is not a complete Martingale framework.
Preparation can also apply the canonical series `begin` transition in the same
Unit of Work when an expected series revision is supplied.

## Batch 7: Host and Broker Adapters

The accepted synthetic slice is followed first by
[Batch 7A: Platform Integration Contract](trading-system-batch7a.md), with a
separate [delivery plan](trading-system-batch7-plan.md). 7A audits existing
platform guarantees and proposes the state/identity, retry ownership,
reconciliation, account/risk, lifecycle and offline-test boundaries. It adds no
runtime. Its independent review is complete and PR #193 is merged.

The accepted 7B delivers only a pure request/observation adapter.
7C-A extends failed-open evidence with explicit serialization compatibility and
is accepted/merged. 7C-B adds durable receipt application and known-ID
reconciliation on fakes; stop at its Draft PR for independent review. A later PR covers bounded offline
integration through the existing platform layer (7D). Live execution requires
a separate future gate. Graph/GUI work and ADELIA/MegaConnector integrations
are not part of Batch 7.

Do not alter `IntradeBarPlatform`, `TradeManager`, `RequestManager`, `TradeState`,
callbacks or existing retry behavior without a separately confirmed defect.
IntradeBar `OPEN_ERROR` keeps failed-open semantics; `CHECK_ERROR` preserves
previously established opening but does not independently prove it. Existing
Trading System unknown-execution states remain for genuinely missing evidence,
not blanket reinterpretation of broker errors. No strategy formula belongs in
the platform.

## Review Gates

Every batch requires:

- focused unit/regression tests;
- a compile consumer through supported aggregate headers;
- an explicit negative test for cancellation, unknown result and duplicate
  delivery where relevant;
- no secret or live-account material in fixtures;
- a diff review showing that unrelated user changes were preserved.

The separate Batch 5A, 5B, 5C, 7A, 7B and 7C-A review gates are complete. Stop
after each independent review. The later offline 7D integration does not waive
the separate 7E outbox/index review gates or authorize live execution.
These stages do not include live Polymarket integration, an exchange
orderbook/position engine, a GUI/graph editor, an FX aggregation engine, or a
new scheduler/event bus.
