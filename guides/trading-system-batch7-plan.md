# Trading System Batch 7: Delivery Gates

## Status

Batch 6 merged as `47ec37b825da9163bfa7ed3fe09f2d4dff0d1797`.
[Batch 7A](trading-system-batch7a.md) was accepted and merged in PR #193 as
`e81a43db85a31d6828fac72422e0fc5d384e718a`.
[Batch 7B](trading-system-batch7b.md) was accepted at `dad7bcf8` and merged
through PR #194 as `6e5f0adac14ddb5aa135dc5b67a2d108a2d7f1cc`.
[Batch 7C-A](trading-system-batch7c-a.md) is the separate failed-open evidence
slice authorized for Draft review. Stop after 7C-A; do not merge it or begin
7C-B/7D automatically.

All slices preserve the existing Intrade platform, managers, `TradeState`,
callbacks and retry behavior. Changes to the new Trading System's internal
receipt/evidence boundary require explicit review but must not become a broker
API rewrite. MegaConnector/ADELIA integration, GUI, graph runtime, full MM
formulas and live trading are outside these slices.

## 7A: Architecture And Contract

Scope: current source audit, exhaustive state correspondence, identity/account
binding, retry ownership, lifecycle, failure/reconciliation and offline test
design. Refresh Batches 1-6 completion status and projection upgrade procedure.

Acceptance: docs-only diff, working relative links, no contradictions with
existing broker semantics, open design questions named, no account/network
actions. No C++ API, durable schema or dependency changes.

## 7B: Pure Observation And Request Adapter

**Pure mapper slice, independently reviewed and merged in PR #194.**

Scope: deterministic functions translating a bound physical roster member to
`TradeRequest` data and `TradeResult` data to a typed internal observation.
No submission, DB transaction, platform lifecycle or scheduler ownership.

Reuse: `TradeIntent`, `ExecutionRosterRecord`, `TradeRequest`, `TradeResult`,
`TradeResultQuery`, `MoneyAmount`, existing enums and supported aggregates.
Keep original broker outcome and error provenance rather than parsing error
strings or broker HTML again. Pure failed-open observations do not yet invent
a durable `NOT_SUBMITTED` settlement for an attempted HTTP opening.

Public API: none required in the broker/platform layer. Keep candidate mapper
types internal until their use by the execution owner is reviewed; no new
mandatory public facade is needed for a pure mapping test.

Invariants: `OPEN_ERROR` remains failed opening, `CHECK_ERROR` preserves
previously established acceptance but does not independently establish it,
only confirmed financial states produce normalized financial facts, and local
queue admission is not broker acceptance. All ID-width and native-money
conversions are checked, including preservation of source/account identity.

Negative tests: every state plus unknown numeric enum values; `live_state`
differing from `trade_state`; empty-body parser fixture; missing/foreign IDs;
uint32/int64 overflow; invalid amount/currency/precision; a check-error profit
estimate; unsupported refund economics; duplicate observation equivalence.

Acceptance: no broker calls or credentials, exhaustive mapping tests, existing
Intrade parser/result-query regressions unchanged, C++17 aggregate consumer,
Windows MSVC/MinGW and Ubuntu GCC where supported, exact-head CI.

Dependencies/risks: review the initial numeric profile. The durable rejected
evidence gap remains explicit; do not solve it by changing platform semantics.

## 7C-A: Failed-Open Evidence

Scope: append NOT_EXECUTED_PROVEN without renumbering existing evidence, version
its settlement encoding explicitly, and reuse the atomic physical/risk/group
completion paths. Preserve legacy NOT_SUBMITTED and existing records. No
platform receipt adapter or result-query integration in this slice.

Acceptance: legacy/new codec and reader/writer failures, unknown nonproof,
duplicate/conflicting facts, risk-release rollback/reopen, series/MM replay,
Batch 1-7B regressions, C++17 consumers and exact-head Windows/Ubuntu CI. See the
[evidence contract](trading-system-batch7c-a.md) for the audit and compatibility
decision. Separate Draft PR; independent review required before 7C-B.

## 7C-B: Durable Receipt Application And Known-ID Reconciliation

Scope: factor the minimal receipt path in the existing execution recovery owner,
apply acceptance/check-failure/final receipts with revision and idempotency
guards, and drive `fetch_trade_result()` through a programmed `BaseTradingApi`
fake for known broker IDs. No real Intrade execution. This follows independent
acceptance of 7C-A; it is not part of the evidence-only PR and is not yet authorized.

Reuse: `ExecutionRecoveryManager` transitions, `TradingSystemStorage::UnitOfWork`,
physical settlement/group completion, `TradeResultQuery`, existing account and
risk identities. A failed check is a diagnostic/result-retry condition, not a
newly unknown opening or another financial outcome.

Public API: no broker changes. A narrow Trading System receipt capability or
evidence extension may be necessary because current methods are simulator-only;
its exact form, serialization compatibility and include ownership are review
gates, not predetermined new interfaces.

Invariants: external calls outside UOWs; durable submission boundary first;
terminal facts survive late errors; same settlement cannot advance MM twice;
confirmed opening is never erased by a failed result query. No automatic resend
when no captured callback/authoritative identity exists. No arbitrary history
candidate is promoted to a confirmed match.

Persist the immutable normalization version together with the selected
currency/precision profile and original numeric facts for converted durable
evidence. Replay uses that version or fails closed if unsupported; it must not
recompute historical money under the latest normalization rules.

Negative tests: rejected/duplicate/conflicting receipt; stale revision; lost
acceptance/result callback; unknown ID or string-only Intrade identity; incomplete
or ambiguous history; result checks during account deactivation; storage failure
and reopen; accepted fact with unavailable outcome; mirror replay after a crash.

Acceptance: review and test failed-open evidence encoding; no duplicated state
machine; record-visit instrumentation for bounded ordinary receipt reads; no
per-callback full-journal recovery. Replay proves stable IDs, held unresolved
risk and exactly-once local progression. All earlier regressions and exact-head
Windows/Ubuntu CI pass.

Dependencies/risks: 7B accepted; approved evidence codec if changed; bounded lookup
and TradeRecordDB mirror design. The existing full `recover()`/standalone DB
methods cannot silently satisfy those contracts.

## 7D: Offline Platform Lifecycle Integration

Scope: integrate the approved adapter with the existing platform loop, request
callbacks and runtime-owned durable owner. Add a bounded observation handoff and
shutdown/restart tests. Use fake transport/local fixtures for the existing
Intrade layer; no connection to a trading account. This is still not an
unrestricted production `TradingSystem` runtime.

Reuse: `BaseTradingPlatform::post_task()`, `TradeRequest::add_callback()`,
`TaskManager`, `EventBus`, lifecycle stack, optional host account/condition hubs,
existing risk owners and admission projection. Host configuration supplies
platform instances and stable account bindings; MM does not configure proxies
or credentials or call brokers.

Public API: no platform/manager/callback/retry changes. Review only the minimal
Trading System owner surface required for the fixture; do not introduce a
generic scheduler, universal module interface or alternate platform manager.

Invariants: callbacks enqueue immutable observations without DB work, capacity
is bounded, overload fails closed, callbacks cannot outlive guards, shutdown
keeps observation intake alive until platform drain. Aliases share scope risk;
reset and removal retain unresolved exposure. Callback `void` is not durable ack.

Negative tests: scheduling rejection; accepted-but-cancelled posted work;
callback-before-return; out-of-order/duplicate callback; timeout/loss of ack;
transport retry counts distinct from new logical attempts; mailbox overflow;
shutdown while broker observations are queued; restart at every pre-submit,
acceptance, settlement and mirror boundary; changed account/realm or stale data.

Acceptance: deterministic manual owner-loop tests plus bounded thread/lifetime
tests, no hangs/lock inversion, no access after owner destruction, stable native
money and provenance, all Batch 1-6/platform regressions and exact-head CI.

Dependencies/risks: 7C accepted; host-approved mailbox/freshness budgets;
offline transport seam validated without a production API redesign. If the
fixture requires a broader platform change, stop and request a separate design
review rather than expanding this scope.

Before 7D, resolve the separately recorded [MSVC platform-target baseline](trading-system-batch7b.md#separate-msvc-platform-baseline)
or establish a reproducible isolated Windows-platform verification path.
Passing Windows domain/mapper CI does not substitute for that platform check.

## Separate Live Gate

Live execution requires a later explicit assignment, independent review of
offline integration, account/environment limits and an agreed read-only/demo
verification sequence. There is no live implementation or trading permission
in 7A-7D. This gate must assess actual broker behavior and history availability;
it cannot be discharged by local parser tests or synthetic idempotency claims.
