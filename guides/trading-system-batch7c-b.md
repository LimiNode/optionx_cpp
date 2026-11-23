# Trading System Batch 7C-B: Durable Platform Receipts

## Scope And Reviewed Base

PR #195 was independently accepted at
`a36d6050af408d979a7ef3efbcfac12d90661043` and guarded squash-merged as
`b6f293833bd724075767d3ad31738d9ec245fde6`. The trees match. Its evidence ordinals,
local evidence-version gate and global schema remain unchanged.
Post-merge [Windows](https://github.com/LimiNode/optionx_cpp/actions/runs/45124466598)
and [Ubuntu](https://github.com/LimiNode/optionx_cpp/actions/runs/45124466863)
smoke workflows passed on that exact merge SHA.

This slice connects the pure mapper to the existing `ExecutionRecoveryManager`
and `TradingSystemStorage::UnitOfWork`. It is an internal adapter protocol,
not a second execution owner or a production TradingSystem runtime. Use
`<optionx_cpp/trading_system.hpp>`; neither detail leaf is a standalone include.
The owning aggregate prepares the existing `platforms::BaseTradingApi` role
after the trading DTOs, without importing the full platform/HTTP stack.

No broker, manager, TradeState, DTO, callback, retry or dependency changes;
no live/demo traffic, automatic resend, scheduler or platform lifecycle work.
The reviewed corrective scope is authorized for guarded merge after exact-head
Windows/Ubuntu CI. Trading System development then stops at 7C-B; do not start
7D without a new assignment. The known [MSVC platform-target failures](trading-system-batch7b.md#separate-msvc-platform-baseline)
remain a separate gate before 7D.

## Internal Owner Protocol

| Existing owner method | Responsibility |
| --- | --- |
| `capture_platform_binding(storage, prepared, currency, precision)` | Verify persisted PREPARED revision 0, account, admitted roster, physical member and ACCOUNT-scope reservation; map and durably seal the callback-free request before submission. |
| `restore_platform_binding(storage, operation_id)` | Restore the sealed capability from its immutable journal event, never authorize resubmission. |
| `apply_platform_receipt(unit, binding, raw, receipt_id, expected_revision)` | Validate provenance, map the DTO, stage evidence, projections and deduplication in the existing transaction. Any failure poisons the UOW. |
| `apply_platform_receipt(storage, ...)` | Convenience transaction/commit wrapper; failed results expose no staged operation or settlement ID. |
| `platform_receipt_fact(storage, binding, receipt_id)` | Validate and return already stored numeric provenance and normalized result, without re-quantization. |
| `request_known_result(storage, binding, revision, api, endpoint_account, callback)` | Verify confirmed opening, numeric broker ID, claim and endpoint metadata; close UOW, then call the existing `fetch_trade_result()`. |

`detail::platform_receipts::Binding` has no public/default constructor or mutable
document access. Only the existing owner can capture/restore it; copies preserve
the same immutable event ID and document. Validation compares that exact durable
event, immutable account/platform/external identity/realm/canonical ID/risk scope,
request trade ID and operation/client/intent/group IDs. Account deactivation and
monotonic identity revision changes are allowed for recovery, not rebinding.
The underlying privileged `put_account()` API is not turned into an AccountRegistry:
if maintenance changes identity, receipt application fails closed.

This capability is **not** a cryptographic proof of the callback's source.
TradeResult has no AccountId. The host must capture the binding in the original
request/query callback and attest that its configured API endpoint belongs to
that exact account. Passing arbitrary raw DTOs or false endpoint metadata is
outside that trusted host boundary. Raw connection/table access remains
privileged maintenance, not a supported way to mint broker evidence.

## Journal And Compatibility Decision

Use the existing journal, journal-ID index and per-operation revision keys;
no new table, persistent index, enum ordinal or global schema migration is
necessary. Additional immutable journal facts have local envelope version 1:

| Event type | Existing journal idempotency key |
| --- | --- |
| `PLATFORM_BINDING` | `platform-binding:<operation_id>` |
| `PLATFORM_RECEIPT` | `platform-receipt:` followed by JSON array `[operation_id, receipt_id]` |
| `PLATFORM_BROKER_BINDING` | `platform-broker:` followed by JSON array `["INTRADE_BAR", realm, account_risk_scope_id, canonical_numeric_broker_id]` |

JSON arrays keep arbitrary separators inside IDs unambiguous. A claim belongs
to one physical operation/binding, not merely its logical group. ACCOUNT risk
scope is the shared financial identity, so aliases cannot bind one execution to
two roster members. Different financial accounts may legitimately use the same
broker number. Caller configuration must already give proven aliases the same
scope; this slice does not infer duplicate accounts from email or display names.

Binding payload stores the initial operation, account metadata, mapped request,
currency/precision, `BINARY64_FIXED_NEAREST_EVEN_V1` and binary64 request amount
bits. Receipt payload stores the raw DTO, original request/result amount and
profit bits as uint64, the same profile/rule/binding event ID, resulting operation,
canonical settlement ID and already normalized fixed-point profit (or null).
Binary64 bits preserve original financial inputs even when JSON cannot express
a nonfinite diagnostic value. Financial conversion itself rejects nonfinite data.
Native monetary facts remain authoritative; no implicit FX or base-currency
projection is introduced. Existing MoneyAmount JSON uses its existing currency
codec; the new profile explicitly stores the enum ordinal.

Operation events written by this path retain the existing operation codec and
add `platform_receipt_version: 1`, `platform_receipt_key` and
`platform_settlement_id`. These are linkage/provenance extensions, not a parallel
terminal ledger. Financial and failed-open evidence use the existing settlement
codec, including evidence version 2 for NOT_EXECUTED_PROVEN. Old operation/settlement
fixtures need no rewrite. Unknown local envelope/profile/normalization versions,
malformed numeric metadata and inconsistent canonical facts fail closed in the
new receipt reader; recovery rejects unsupported operation-extension versions.

Compatibility: the prior generic journal reader can read unknown event types and
ignores additional operation fields, but it cannot interpret this new adapter
protocol. This is not environment-wide feature negotiation or downgrade support.
Keep one compatible execution owner; mixed-version writers and downgrades remain
prohibited. Do not combine simulated/generic broker-state writes with the sealed
platform protocol for one operation: its acceptance must include the matching
broker ownership claim. No historical claims are guessed or backfilled.

## State, Terminality And Replay

Submission must already be durable. PREPARED alone cannot accept any callback.
The existing submission marker holds the operation boundary; the reservation
can remain RESERVED until a later owner projection changes it, and remains held.
This slice does not implement the external submission call.

| Observation | Durable action |
| --- | --- |
| OPEN_SUCCESS / IN_PROGRESS / WAITING_CLOSE | Preserve/establish acceptance; bind positive numeric broker ID when supplied. An earlier acceptance without ID can later acquire one. |
| CHECK_ERROR | Diagnostic receipt only. No new opening, demotion, settlement, risk release or broker claim. Prior accepted ID/state survives. |
| OPEN_ERROR | If no prior acceptance, REJECTED + NOT_EXECUTED_PROVEN, physical evidence and risk release in one UOW. Prior acceptance/financial evidence makes this conflicting. |
| WIN / LOSS / STANDOFF / REFUND | Mapper-validated final observation creates the existing financial physical settlement. A first final callback stages legal acceptance/identified opening followed by SETTLED in the same UOW. |
| UNKNOWN / WAITING_OPEN / cancellation notification | Diagnostic only; retain unresolved exposure. A cancellation notification is not proof of non-execution. |
| No callback / missing broker ID | Remain unresolved; no heuristic identity or automatic resend. |

IntradeBar OPEN_ERROR keeps its established semantics, including HTTP 200 with
an empty response. The Trading System does not parse HTTP again. CHECK_ERROR is
not reclassified as an unknown fact of opening or a new financial outcome.

Exact receipt identity + identical original facts returns DUPLICATE, even with
a stale supplied revision. This shortcut returns no operation snapshot, avoiding
rollback of newer in-memory owner state. Changed facts under that ID conflict.
For a new receipt, expected revision and absence of its successor are probed
inside the same write transaction; startup recovery must first audit contiguous
owner chains. Ordinary application never calls full `recover()`.

The first terminal settlement ID is canonical. A new delivery of equivalent
terminal evidence returns that ID without another settlement or MM update.
Changed normalized financial outcomes/profit conflict. A different receipt ID
with equivalent normalized evidence does not replace the original numeric facts.
Late diagnostics do not demote terminal state. A valid late observation proving
opening (OPEN_SUCCESS, IN_PROGRESS or WAITING_CLOSE) conflicts with REJECTED or
CANCELLED non-execution evidence; it cannot be silently classified as a duplicate.
The conflict changes no journal, physical, reservation, series or MM facts and
authorizes no broker action. Previously confirmed opening/progress after SETTLED
remains idempotent when identity matches. Terminal replay validates the persisted profile and source
receipt against the canonical settlement; it does not re-normalize old profit.
An entirely new callback is still mapped/validated under the captured V1 profile.

## Physical And Logical Completion

One receipt UOW commits receipt fact, broker claim, operation transitions,
physical projection and physical terminal settlement together; failed-open also
releases that member's reservation. Financial risk remains held until existing
logical completion. No callback can satisfy an additional roster member.

Group/series/MM completion reuses existing `apply_group_completion_atomic()`
or `apply_settlement_atomic()`. The staged overload can share a caller-owned UOW
with those validated operations. This slice's tests use the existing SyntheticHost
completion after durable physical facts: a crash between phases leaves replayable
physical evidence and pending logical completion, not double progression.
The fixture's MM is not new production policy code.

No TradeRecordDB mirror write is added. The bound raw DTO/provenance remains
available for a later idempotent projection; this does not claim atomicity with
TradeRecordDB's standalone transactions or with a broker. Notification delivery
and durable mirror replay are still integration work for a later gate.

## Known-ID Query And Lifetime

The real `BaseTradingApi` role is exercised with a programmable offline fake.
Only a confirmed opening and a canonical positive int64 numeric broker ID with
the matching claim can reach `fetch_trade_result()`. String-only/missing IDs,
wrong endpoint realm/identity, stale revision or scheduling rejection fail
closed. Existing `TradeResultQuery` retry defaults are unchanged. There is no
additional retry loop, `place_trade()` invocation, history heuristic or resend.

Validation's UOW ends before the API call, including synchronous callbacks.
The callback captures only the caller's callback and passes an owned DTO value;
it captures no storage, manager or platform pointer. Caller must keep the API
alive for the call and hand the observation to its existing owner thread.
Production callbacks must enqueue, not perform this test's synchronous DB work
on the platform loop. Queue capacity, shutdown drain and delivery acknowledgement
are deliberately deferred to 7D. Scheduling success is not durable acknowledgement.
Queued but uncommitted observations are not falsely reported as persisted.

## Crash And Cost Checks

| Boundary exercised offline | Surviving facts / restart action |
| --- | --- |
| Binding committed, submission not started | Original binding restored; callbacks refused. |
| Submission started, no callback | Stable operation/account/roster; held risk; no resend. |
| Acceptance without ID | Preserve acceptance; no result query until an ID arrives. |
| Acceptance and ID committed | Restore exact numeric claim; query the same existing API. |
| CHECK_ERROR after acceptance | Preserve opening/ID and held risk; later final observation can settle. |
| Terminal/physical writes staged, before commit | Dropped/poisoned UOW or allocator failure rolls all facts back, including broker claim and failed-open release. |
| Physical settlement committed, logical completion pending | Reopen, aggregate canonical member facts, apply existing group/series/MM UOW once. |
| Logical completion committed, late/repeated callback | Canonical settlement and MM revision remain unchanged. |
| Partial multi-account group | Release only conclusive failed-open member; hold other exposure until its evidence. |
| Account deactivated / identity changed | Recovery can use retained same identity; changed identity fails closed. |
| Deferred query callback after local owner destruction | Owned DTO can be queued; no captured owner pointer; apply later through restored binding. |

Focused [tests](../tests/trading_system_batch7c_b_test.cpp) include corrupt local
versions, narrowing metadata, original binary64 bits, canonical replay, aliases,
wrong IDs and finite-money validation. The [C++17 consumer](../tests/trading_system_batch7c_b_consumer_test.cpp)
uses only the supported aggregate. Windows/Ubuntu workflows run both targets
alongside earlier batches.

The cost fixture compares a diagnostic receipt before/after 4096 unrelated
journal facts. Diagnostic and exact-replay work uses only point reads, no global
history copies. Financial settlement retains the existing group-terminal index
visit bound: O(G + M log M) for G group facts and M matching member facts, not
O(total historical operations). The two counters report owner-issued journal
point reads and terminal-index entries visited, **not total MDBX I/O**, B-tree
page reads or wall-clock performance. Actual counts and compiler/CI results
belong in the PR report. Group completion and startup recovery retain their
previous separately documented history costs.

## Corrective Verification

The three rejected-then-opening regressions failed on reviewed HEAD
`9b6e38af6e11344d173f4432343a59eb432faa99` (DUPLICATE instead of CONFLICT).
After the terminal guard, all 32 receipt tests pass, including unchanged durable
facts across reopen and late SETTLED diagnostics/contradictions. Local MSVC Debug
and MinGW Release runs pass 283 Batch 1-7C-B tests and all six standalone consumers.

The earlier 132 versus 131 platform-test totals selected different executables,
not a removed test: 111 parser/response + 12 result-query + 9
`lifecycle_event_safety_test` versus the same first two + 8
`intrade_auth_lifecycle_test`. The corrective MinGW run explicitly executes all
four (140/140):

```powershell
cmake --build build-codex --target intrade_bar_api_response_test trade_result_query_test intrade_auth_lifecycle_test lifecycle_event_safety_test --parallel 2
foreach ($name in @('intrade_bar_api_response_test', 'trade_result_query_test', 'intrade_auth_lifecycle_test', 'lifecycle_event_safety_test')) {
    & ".\build-codex\$name.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

These are local offline results, not claims of MSVC platform-target success or
exact-head remote CI. The separately documented MSVC baseline remains unchanged.
