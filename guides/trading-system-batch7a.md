# Trading System Batch 7A: Platform Integration Contract

## Status And Scope

Accepted architecture contract, merged through PR #193 as
`e81a43db85a31d6828fac72422e0fc5d384e718a`, based on
`main@47ec37b825da9163bfa7ed3fe09f2d4dff0d1797` after the guarded squash merge of
[Batch 6, PR #192](https://github.com/LimiNode/optionx_cpp/pull/192).
Batches 1-6 are complete. `optionx::test_support::SyntheticHost` remains test
support, not an installed API or a production runtime. This document introduces
no C++ implementation, schema change, broker request or live-account test.

The existing OptionX platform API defines the integration contract, including
IntradeBar failed-open semantics. It is the integration boundary, not an
implementation to replace or reclassify.

The [implementation slices](trading-system-batch7-plan.md) require separate
approval. Subsequent review authorized only the [pure 7B mapper](trading-system-batch7b.md).
7C/7D and live execution remain gated; no runtime follows from the 7A merge.

## Decisions

- Preserve `IntradeBarPlatform`, `TradeManager`, `RequestManager`, `TradeState`,
  callback signatures and existing retry behavior. No broker API change without
  a separately confirmed real defect and its own review.
- IntradeBar `OPEN_ERROR` means unsuccessful opening under the existing platform
  contract. Do not reinterpret it as `EXECUTION_UNKNOWN` from generic network
  reasoning or parse `error_desc` to invent another HTTP classifier.
- `OPEN_SUCCESS` confirms opening. `CHECK_ERROR` concerns the result of an
  opened trade; it must not erase acceptance, become an opening failure or
  release risk as if no trade existed.
- Reuse the existing Trading System `EXECUTION_UNKNOWN` states for genuinely
  missing execution evidence, including process interruption before a platform
  callback is recorded. Do not add that value to broker `TradeState`.
- A request accepted by a local queue is not yet broker acceptance. Conversely,
  absence of a callback after possible invocation is not an `OPEN_ERROR` callback.
- `TradingSystem` remains the future public facade; `TradingSystemRuntime` owns
  decisions and durable work; `ExecutionManager` integrates submission and
  reconciliation. No overlapping `TradingCoordinator`, second platform manager,
  scheduler, event bus or graph engine is proposed.

## Reuse Audit

All source paths below are relative to `include/optionx_cpp/` unless linked
otherwise. Internal leaves are evidence references, not consumer include points.
Consumers keep using the supported `platforms.hpp`, `trading_system.hpp`,
`storages.hpp` and optional `lifecycle.hpp` aggregates.

| Current path / class | Existing guarantee | Integration use or concrete gap |
| --- | --- | --- |
| [BaseTradingApi](../include/optionx_cpp/platforms/common/BaseTradingApi.hpp) | `place_trade()` returns queue admission; `on_trade_result()` receives lifecycle updates; `on_trade_id()` supplies `uint32_t` IDs. | Reuse the facade. A Boolean return is not broker acceptance. |
| [BaseTradingPlatform](../include/optionx_cpp/platforms/common/BaseTradingPlatform.hpp) | `post_task(work, cancelled)` serializes owner-loop work; cancellation covers accepted work not yet executed; `run/process/shutdown` manage lifecycle. | Marshal platform access; do not call HTTP managers from the runtime thread. |
| [IntradeBarPlatform](../include/optionx_cpp/platforms/IntradeBarPlatform.hpp) | `place_trade()` delegates to the execution component; `fetch_trade_result()` and history calls marshal through `post_task()`. | Keep its implementation unchanged; result lookup is separate from reopening. |
| [TradeQueueManager](../include/optionx_cpp/components/BaseTradeExecutionComponent/TradeQueueManager.hpp) | Keeps nonzero preassigned IDs; invokes request callbacks and the platform callback with clones; manages the queue and state notifications. | Prefer one request-owned callback; preserve the host's global callback slot. Queue insertion alone is synchronized, not arbitrary account/preprocess access. |
| [TradeRequest](../include/optionx_cpp/data/trading/TradeRequest.hpp), [TradeResult](../include/optionx_cpp/data/trading/TradeResult.hpp) | Request owns `account_id`, callbacks and parameters; result has local ID, broker IDs, `trade_state`, `live_state`, errors and money fields. | Capture the immutable account/operation binding outside the result; its DTO has no `account_id`. Clones do not retain request callbacks. |
| [TradeResultQuery](../include/optionx_cpp/data/trading/TradeResultQuery.hpp), `BaseTradingApi::fetch_trade_result()` | Query carries local ID, numeric/string broker identity and check retry budget. | IntradeBar's manager requires positive numeric `option_id`; a string hash alone is insufficient there. |
| [TradeManager](../include/optionx_cpp/platforms/IntradeBarPlatform/TradeManager.hpp), [http_parsers](../include/optionx_cpp/platforms/IntradeBarPlatform/http_parsers.hpp) | Separates opening from result checks; exposes `OPEN_ERROR`, `OPEN_SUCCESS`, `CHECK_ERROR` and final outcomes. | Consume the resulting DTO, not a second interpretation of the broker page. |
| [RequestManager](../include/optionx_cpp/platforms/IntradeBarPlatform/RequestManager.hpp) | Owns HTTP creation, rate limits and result-check retries. | Audit retry ownership, but do not change it in this batch. |
| [AccountInfoUpdate](../include/optionx_cpp/data/account/AccountInfoUpdate.hpp), [TradingConditionUpdate](../include/optionx_cpp/data/account/TradingConditionUpdate.hpp) | Internal account correlation and scoped optional condition fields already exist. | Copy decision-relevant values in the platform loop; do not retain mutable provider internals. |
| [AccountInfoHub](../include/optionx_cpp/components/AccountInfoHub.hpp), [TradingConditionHub](../include/optionx_cpp/components/TradingConditionHub.hpp) | Optional fan-out and replay over existing single callback slots. | Subscribe through a host-owned hub where present; do not steal its binding or assume replay implies freshness. |
| [TradeRecordDB](../include/optionx_cpp/storages/TradeRecordDB.hpp), [TradeRecord](../include/optionx_cpp/data/trading/TradeRecord.hpp) | Persistent 32-bit trade IDs; physical snapshots; standalone transactions. | Reserve IDs before preparing the roster. Standalone writes are not part of `TradingSystemStorage`'s UOW. |
| [ExecutionRecoveryManager](../include/optionx_cpp/trading_system/ExecutionRecovery.hpp) | Durable preparation, revision checks, unknown handling and simulated recovery. | `submit()` / `reconcile()` currently take `SimulatedExecutionAdapter`; generic broker receipt application is not implemented. Do not claim a live adapter can already be passed to them. |
| [TradingSystemStorage](../include/optionx_cpp/trading_system/TradingSystemStorage.hpp) | Atomic local facts, physical evidence, group completion, idempotency and versioned admission projection. | Reuse UOW transitions, never include an external broker call inside the transaction. |
| `utils::TaskManager`, `utils::EventBus`, lifecycle stack | Existing dispatch and staged shutdown. | Reuse them for owner scheduling; any bounded observation mailbox belongs to runtime ownership, not a new generic scheduler. |

## State Correspondence

This is the target adapter contract, not a claim that a generic durable receipt
adapter exists today. The table consumes **`TradeResult::trade_state`**.
`live_state`, indicative price P&L and a locally estimated `close_balance` do
not prove final broker settlement. Acceptance and result availability are
independent facts; the current execution enum is not a Cartesian product of
those facts.

| Platform observation | Opening fact | Trading System action | Risk / progression |
| --- | --- | --- | --- |
| `place_trade() == true` | Only local queue admission. | Preserve `SUBMISSION_STARTED` until a platform observation supplies more evidence. | Hold risk; no settlement. |
| `place_trade() == false`, before enqueue | This invocation was not admitted. | Record local rejection using its captured operation binding. | Release only that member's risk through a validated durable rejection. |
| `UNKNOWN` | No new opening evidence. | Keep the existing operation state; retain diagnostic observation. | No invented result or release. |
| `WAITING_OPEN` | Queued/opening is in progress. | Keep `SUBMISSION_STARTED`; never promote from this notification alone. | Hold risk. |
| `OPEN_ERROR` (IntradeBar) | Existing platform contract says opening failed. | Failed-open observation; target operation outcome `REJECTED`, preserving the original error and retry policy. See evidence-encoding gap below. | Not a financial LOSS. Retry eligibility follows the existing host policy, not a new resend engine. |
| `OPEN_SUCCESS` | Opening confirmed. | Persist acceptance and numeric broker ID; `BROKER_ID_PERSISTED` when available, otherwise `BROKER_ACCEPTED` and reconcile identity. | Hold risk, await result. |
| `IN_PROGRESS` | Active trade. | Preserve acceptance/ID; `EXECUTED` may record active execution after the valid acceptance path. | Hold risk; ignore indicative `live_state` as settlement. |
| `WAITING_CLOSE` | Opened trade awaiting its final check. | Preserve acceptance and broker ID. | Hold risk, no financial progression. |
| `CHECK_ERROR` | Result check failed; do not undo known opening. | Retain accepted/executed state and journal the check failure; schedule an eligible `fetch_trade_result()` for the same broker ID. | No settlement from fallback profit; no release or reopen. |
| `WIN` | Confirmed final financial outcome. | `OutcomeKind::WIN`, `EXECUTED` evidence and validated positive native net profit. | Physical settlement, then group completion once all members qualify. |
| `LOSS` | Confirmed final financial outcome. | `OutcomeKind::LOSS`, `EXECUTED` evidence and validated negative native net profit. | Same group-level rule. |
| `STANDOFF` | Confirmed draw. | `OutcomeKind::DRAW` with checked zero native net profit. | Executed draw, not a rejected attempt. |
| `REFUND` | Final refund, not a failed open. | For the initial full-refund profile, checked zero net profit maps to executed `DRAW`; retain original `REFUND`. Nonzero/unsupported refund economics require an explicit policy, not an invented zero. | Aggregate the confirmed executed contribution; reject unsupported normalization without erasing the raw result. |
| `CANCELED_TRADE` | Depends on established submission history. | `CANCELLED` / `NOT_SUBMITTED` only for a demonstrated pre-execution cancellation; otherwise preserve accepted or unresolved evidence and reconcile. | Cancellation cannot erase a potentially active trade. |
| No callback after possible invocation / process crash | No platform result was captured. | Existing `EXECUTION_UNKNOWN` / reconciliation path. This is not reclassification of an observed `OPEN_ERROR`. | Hold risk; do not automatically resend. |

When a final callback is the first durable observation, validate its captured
request/account identity and broker ID, then persist the necessary acceptance
and settlement facts using allowed transitions. Do not invent that callbacks
must arrive once each or in a fixed order. A final fact cannot be demoted by a
late `WAITING_OPEN`, `OPEN_SUCCESS` or `CHECK_ERROR` notification.

### Failed-open evidence encoding

There is a real **Trading System** integration gap, not a broker API defect:
`TradeSettlement::valid()` currently accepts `REJECTED` only with
`ExecutionEvidence::NOT_SUBMITTED`, and the recovery helper is private and named
`reject_without_submission()`. That model covers the simulated pre-submit
rejection. An IntradeBar `OPEN_ERROR` can instead follow an actual HTTP attempt
that did not open a trade.

The pure 7B mapper must preserve a typed failed-open observation and its
platform provenance. Before durable integration, review the narrow encoding of
that observation separately from proof that no request was submitted. Do not
silently describe an HTTP attempt as never sent, weaken existing settlement
validation, or force `OPEN_ERROR` to `EXECUTION_UNKNOWN` to avoid this gap.
The intended runtime result is still unsuccessful opening, not LOSS, with the
existing retry eligibility. A compatible Trading System evidence change, if
needed, must be explicitly reviewed with codec/replay tests; 7A makes none.

### HTTP 200 With Empty Body

The special case is explicit in `parse_execute_trade()` before broker-ID
extraction. The current regression
[`EmptyTradeOpenResponseHasSpecificError`](../tests/intrade_bar_api/intrade_bar_api_response_test.cpp)
passes an empty string and HTTP 200, expects `success == false`, retains status
200 and checks:

```text
Trade open failed. Server returned an empty response; instrument may be closed or unavailable.
```

`TradeManager` converts the parser failure to `OPEN_ERROR`. Preserve that
classification. This is the supported broker-specific refusal rule in OptionX.
The test proves the local contract, not the absence of external server side
effects. No archived live observations establishing that external guarantee
were collected or independently verified in 7A.

A valid HTTP 200 with empty content is not the same as having received no valid
HTTP response. The adapter does not inspect raw HTTP to override either legacy
classification. If no platform observation survives a crash, use the existing
durable recovery rules for missing evidence instead.

## Identity And Request Translation

1. Allocate each physical `trade_id` before roster preparation through the
   existing persistent `TradeRecordDB` ID path. `TradeRequest`, `TradeResult`
   and `TradeRecord` use `uint32_t`; durable physical IDs use `uint64_t`.
   Reject out-of-range values, never truncate or reuse them. An unused allocated
   ID after a crash is an acceptable gap. Allocation has its own transaction and
   is not claimed to be atomic with preparation.
2. Copy the admitted intent into one physical request per immutable roster
   member. Preserve symbol, direction, option type, duration/expiry, native
   currency, source IDs and nonzero trade ID. Check account IDs fit the positive
   `int64_t` request field; do not change any serialized field type.
   `duration` and `expiry_time` remain seconds; `TradeResult` dates are
   milliseconds. Leave broker wire-time formatting to the existing platform,
   rather than applying another timezone offset in the mapper.
3. Persist `operation_id`, `client_operation_id`, intent/group IDs, physical ID,
   account/realm, risk scope and intended parameters before possible execution.
   Capture this binding in the request callback. `unique_id`, `unique_hash` and
   metadata are local correlation aids, not a broker idempotency guarantee.
4. Record the Intrade numeric `option_id` when reported. Store a deterministic
   broker identity representation and retain the original numeric/string DTO
   fields in the observation projection. Broker IDs are scoped by the recorded
   account identity and DEMO/REAL realm, not globally unique by assumption.
5. A repeated callback with the same physical/broker binding and outcome reuses
   the canonical settlement. A conflicting account, broker ID or terminal money
   fact is a conflict, not another series result. Never bind two roster members
   to one broker execution on heuristic similarity.
6. Preassigned IDs avoid the platform's fallback allocator. Do not perform a slow
   DB allocation from `on_trade_id()` in the owner loop for this integration.
   One host allocator must coordinate all consumers of the same physical ID
   namespace; a new Trading System-only counter must not collide with it.

Authoritative money remains `MoneyAmount`. The initial adapter profile is
single-native-currency fixed-outcome execution; it declares currency precision
and deterministic rounding. Validate fixed-point stake -> legacy `double` ->
fixed-point round-trip before submission. Validate finite/range/currency and
state/sign consistency when converting broker result values; preserve the
original numeric observation and normalization policy. Do not reconstruct net
profit from a fresh payout, account-balance change or a `CHECK_ERROR` estimate.
In IntradeBar, `apply_trade_check_info_to_result()` in
[ApiResponses.hpp](../include/optionx_cpp/platforms/IntradeBarPlatform/ApiResponses.hpp)
already sets `TradeResult::profit` to **net P&L**: WIN subtracts the stake from
broker gross proceeds, LOSS uses `-result.amount`, and STANDOFF uses zero.
The mapper normalizes that net value directly; it must not subtract stake again.
An unsupported numeric fact remains raw evidence awaiting resolution, not an
invented settlement. Multi-currency valuation keeps the approved Batch 5A/5B
contracts and is not expanded here.

## Submission And Durable Observations

Target sequence for the future internal adapter:

1. Build immutable decision/account snapshots and obtain authoritative risk
   admission for all actual scopes; shared aliases do not gain separate funds.
2. Commit intent, complete roster, reservations, physical requests, operation
   identities and the optional series begin transition in the existing UOW.
3. Commit `SUBMISSION_STARTED` before enqueueing any action that can call the
   platform. Do not hold the MDBX transaction across queueing or a broker call.
4. Use `platform.post_task()` to call `place_trade()` in the platform loop.
   Capture scheduling failure and its pre-execution cancellation callback as
   distinct local observations. Install `TradeRequest::add_callback()` before
   enqueueing; do not replace the host's `on_trade_result()` slot.
5. The callback transfers a small immutable observation to the runtime owner.
   It performs no strategy calculation, DB transaction or synchronous wait.
   `place_trade()` return and callback delivery may race; the owner applies
   evidence monotonically, not in assumed arrival order.
6. The runtime owner validates the durable binding/revision and records evidence
   in a new UOW. Reuse physical settlement, canonical group replay and atomic
   series/reservation/MM completion. A callback is not a storage commit.
7. Notify consumers after commit. A crash before notification delivery replays
   the committed receipt without a second MM advance.

The current recovery API has no general asynchronous acceptance/failure receipt
entry point: `mark()` and rejection application are private, while public
`submit()` / `reconcile()` take the simulated adapter. A later slice must factor
the smallest validated internal receipt application around these existing
transitions. It must not pass a live platform to the simulated overload or
duplicate its state machine in `SyntheticHost` or a second execution manager.

`mark()` currently calls full `recover()`. That is acceptable as an audited
offline implementation, not a bounded production hot-path promise. Runtime
integration must measure record visits and review point/revision reads before
using it for every live callback. 7A adds neither an unbounded copy-on-callback
design nor an unreviewed persistent index.

`TradeRecordDB` stays a physical snapshot API. Until a shared transaction-aware
path is separately approved, mirror its records only after the canonical
Trading System commit, idempotently by preassigned trade ID, with a recoverable
projection cursor in the canonical environment. A crash between commits replays
the mirror. Never claim standalone `write()` / `upsert()` share the trading UOW;
do not use an independently updated mirror as proof that a command was not sent.
Apply mirror updates in canonical journal/revision order for each physical ID;
the standalone snapshot API does not merge fields or protect against a stale
full-record overwrite. Advance the durable projection cursor only after the
mirror commit. Replaying a crash between those commits repeats the same snapshot,
not an older snapshot over a newer terminal record.

## Reconciliation And Retry Ownership

| Situation | Existing capability and target rule |
| --- | --- |
| Known positive Intrade `option_id` | Use `fetch_trade_result(TradeResultQuery, result, callback)` against the recorded account. Check-query retries are not opening retries. |
| Opening confirmed, ID not durably captured | Preserve acceptance if known; query available history and reconcile the mapping. Never fabricate an ID or automatically reopen. |
| `CHECK_ERROR`, delayed settlement or reconnect | Retain accepted identity and exposure; retry result lookup only when lifecycle/account gates permit. Reconnect alone is not an outcome. |
| Unknown ID, history unavailable/incomplete | Keep unresolved evidence and risk. `fetch_trade_history()` availability is not a guarantee of complete history or proof of no execution. |
| Multiple or merely heuristic history candidates | `AMBIGUOUS`, not authoritative binding. Even a single amount/time match can be a manual trade; consider clock skew, latency and history completeness. |
| Captured Intrade `OPEN_ERROR` | Preserve failed-open semantics and existing policy. No automatic adapter reinterpretation into unknown execution. |
| Process interruption after possible invocation, no captured outcome | Existing unknown/reconciliation mechanism; no adapter resend without a verified no-execution/idempotency basis. |

The source audit establishes these retry layers:

| Layer | Actual current behavior | Integration responsibility |
| --- | --- | --- |
| `RequestManager` HTTP client configuration | Calls `set_retry_attempts(10, MS_PER_SEC)`, with a 30-second timeout and 15-second connection timeout in auth configuration. | Preserve it; configuration is not a broker idempotency contract. |
| [Kurlyk HttpClient](../external/kurlyk/include/kurlyk/http/HttpClient.hpp) and [HttpRequestHandler](../external/kurlyk/include/kurlyk/http/HttpRequestManager/HttpRequestHandler.hpp) | `make_request()` copies client settings. The retry predicate checks curl error/invalid status and budget; it has no GET-only or POST-exclusion condition. Opening POST therefore inherits transport retries. | Document ownership; do not multiply them with Trading System resends. Exact external side effects are not proved by this source audit. |
| `RequestManager::request_trade_check()` | Repeats empty/error result responses within the query's retry budget; `TradeManager` normally supplies 15 retries after the first check. | Keep check polling/rate limits in the platform. Do not issue competing per-callback check loops. |
| Host failed-open policy | Application policy may request another attempt after an unsuccessful opening. There is no universal new retry policy in 7A. | Retain that owner. A new permitted physical attempt gets fresh identities; it is not resurrection of a terminal operation or mutation of a fixed roster. |

For successful transport with valid HTTP 200, Kurlyk does not retry because the
body is empty; the parser then applies the existing failed-open rule. Transport
errors and missing responses are a separate path. This audit changes neither.
Actual transport resend semantics and callback delivery are offline fault-test
subjects before any live gate, not grounds to rewrite today's platform API.

## Accounts And Risk Snapshots

Bind externally configured platform instances to persistent `AccountId`s and
the existing `AccountIdentityRecord`, not a container index. Authentication,
credentials and proxies stay with the host/platform; none belongs in journal
observations. Capture DEMO/REAL, currency and authoritative external identity
where available. An unspecified `AccountInfoUpdate::account_id == 0` can be
correlated by the configured binding, not mistaken for a new account.

Use existing account queries/updates for balance, payout, tier, connection and
conditions. `AccountInfoUpdate` contains a shared mutable provider, not a frozen
balance snapshot or universal freshness timestamp. Extract values in the owner
loop and record observation time, source availability and revision. Receipt
time is not proof of broker-effective freshness. Missing, stale, unsupported or
unknown balance/payout facts block decisions that require them; defaults are
not valid zero balances or payout quotes. A merged condition timestamp does
not establish per-field freshness for every cached optional value.

Aliases retain historical account IDs and share the actual financial scope.
Keep account deactivation tombstones; identity/realm changes cannot silently
rebind outstanding operations. `RiskAdmission` still owns its scope, and the
durable admission projection, account gates and reservation roster remain the
local authority. Platform order limits and open-trade snapshots are additional
platform safeguards, not replacement reservation ledgers. Reset or account
removal cannot clear unresolved exposure.

## Lifecycle And Delivery

Use the existing TaskManager/lifecycle stack with one runtime owner for strategy
and durable state, and the platform owner for platform/HTTP state. A bounded
typed observation mailbox is justified to keep MDBX off the platform loop; it
is not another scheduler or event bus. Its drain uses the existing task system
or an explicitly caller-driven `process()`.

- Attach before submission. Prefer request callbacks; if the host chooses a
  global callback fan-out, its owner must arrange it explicitly rather than
  replacing another consumer. Avoid callback-slot mutation during dispatch.
- Carry owned immutable facts and a stable binding generation. Do not retain
  references to transient callback arguments or mutable account objects across
  loops. Use a lifetime guard/weak handle; no raw runtime `this` captured beyond
  its proven drain lifetime and no callback ownership cycles.
- Define observation acceptance separately from durable commit. The existing
  callbacks return `void`, so they are not a durable acknowledgement protocol.
  Duplicate observations reuse their physical/phase/terminal-evidence identity;
  do not allocate a new settlement for each delivery or rely on callback count.
- Bound mailbox capacity. If scheduling/admission fails, latch a runtime fault,
  block new submission and let the owner persist the gate when possible. Do not
  block the platform loop, silently treat dropped evidence as rejection, or
  pretend an in-memory latch survives process loss.
- Durable `SUBMISSION_STARTED` survives lost observations. On restart, use the
  journal and available broker identity to reconcile uncommitted facts. Missing
  identity can remain unresolved; no exactly-once external delivery is claimed.
- Shutdown stops new decisions first, then drains/cancels runtime-to-platform
  work, keeps observation acceptance alive during platform shutdown, drains
  durable work, and detaches callbacks before destroying owners/storage. Never
  wait synchronously for the other loop while holding a DB/queue/callback lock.
- A rejected `post_task()` or its accepted-but-not-run cancellation proves that
  this scheduled work did not invoke `place_trade()`. Once invoked, consume the
  existing platform outcomes; a bare cancellation request is not new no-execution
  evidence. Late/stale callbacks cannot rebind an account or demote terminal facts.

## Offline Test Contract

Existing evidence to retain includes the empty-body parser regression,
[`trade_result_query_test`](../tests/trade_result_query_test.cpp) for identity
and callback-free clones,
[`lifecycle_event_safety_test`](../tests/lifecycle_event_safety_test.cpp) for
owner-loop ingress/cancellation, and all Batch 1-6 regressions.

The proposed tests below are **not implemented in 7A**:

| Deterministic scenario | Required assertion |
| --- | --- |
| All `TradeState` values, `trade_state` differing from `live_state` | Exhaustive mapping, no indicative financial progression. |
| HTTP 200 empty-body fixture through existing parser and platform outcome | `OPEN_ERROR` retained, no adapter HTTP reparser or blanket unknown mapping. |
| Queue admission versus `OPEN_SUCCESS`; pre-enqueue rejection | No early acceptance; only the demonstrated failed-open/local rejection path releases that member. |
| `CHECK_ERROR` carrying a nonzero estimated profit | Acceptance and broker ID retained, no settlement or reopen, result query uses the original numeric ID. |
| WIN/LOSS/draw/full refund, invalid numeric/currency/sign facts | Exact native normalization or explicit unsupported result; no fabricated money. |
| Duplicate/reordered callbacks and repeated result-query completion | One canonical receipt and MM advance; no regression of terminal state. |
| Known ID check, missing ID, incomplete history and manual-trade lookalikes | Authoritative identity preferred; heuristic ambiguity and unavailable history preserve risk. |
| Loss of callback/ack before durable acceptance, restart after acceptance/result | Existing unknown handling only where evidence is absent; no automatic resend of unresolved operations. |
| Shared-scope aliases, deactivation, stale data, conflicting account generation | No double reservation, identity rebind or stale-data admission. |
| Mailbox full, scheduler rejection, callback during shutdown, owner destruction | Bounded nonblocking callback path, fail-closed gate and no use-after-free/deadlock. |
| Crash between canonical UOW and TradeRecordDB mirror | Canonical outcome retained; idempotent projection replay, not cross-environment atomicity. |
| Repeated owner transitions over large completed history | Instrument actual record visits; no hidden full recovery/history copy in an ordinary callback path. |

Use programmed `BaseTradingApi` fakes first, then an offline fixture beneath the
existing platform layer (fake transport or an explicitly local fixture server).
Preserve platform retries in that fixture and count physical submissions versus
result checks. No credentials, authenticated HTML, real account requests or
real-money trades are allowed. No new general transport/provider framework is
authorized just to make the fixture convenient.

## Review Questions And Gates

1. Approve the minimal internal receipt-application boundary: the manager is
   currently simulator-specific and has no public general receipt method.
2. Resolve the failed-open durable evidence encoding described above without
   revising the broker meaning of `OPEN_ERROR` or conflating it with unknown
   execution. Any compatible codec change needs an explicit scope/replay review.
3. Approve the initial currency precision/rounding profile and unsupported-refund
   behavior before normalizing broker `double` values into durable money.
4. Set host-owned mailbox capacity, overload/shutdown policy and freshness
   budgets. None is inferable from a Boolean API return or cached snapshot.
5. Approve bounded receipt lookup and recoverable physical-history mirroring
   before a long-running runtime; current offline full-recovery costs and
   separate TradeRecordDB transactions must not be hidden.

These questions gate implementation slices, not the accepted Batch 6 merge.
Live correctness, broker-history completeness, external idempotency and the
server's empty-body behavior have not been newly established by 7A. No
production-readiness claim follows from the offline tests or green CI.

## Verification Recorded For 7A

The merged tree was compared with the reviewed Batch 6 HEAD and is identical.
Its post-merge Ubuntu/Windows workflows passed; links and projection upgrade
instructions are in the [Batch 6 completion record](trading-system-batch6.md).
Local MinGW/MSVC reruns passed 189 Batch 1-6 tests plus three C++17 consumers
per compiler. Additional source-audit checks ran locally on MinGW:
`intrade_bar_api_response_test` 111/111, `trade_result_query_test` 12/12 and
`lifecycle_event_safety_test` 9/9. These additional runs are not represented as
independently verified live behavior. Documentation validation covers relative
links, balanced code fences and `git diff --check`.
