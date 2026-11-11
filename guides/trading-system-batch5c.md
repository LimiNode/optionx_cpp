# Trading System Batch 5C: Offline Execution Recovery

Batch 5C adds an offline execution journal, a simulated execution adapter, and
fail-closed reconciliation on top of the Batch 5B storage contract. It does not
submit to a live broker and does not introduce another storage engine.

## Boundary

`TradingSystemStorage::UnitOfWork` remains the only local transaction owner.
Execution recovery stores immutable `EXECUTION_OPERATION` journal events. The
latest operation state is rebuilt by operation ID only after validating the
complete revision chain; an invalid or unreadable chain is a recovery error,
not an empty recovery result.

The complete trading-system domain, including storage and recovery, is exposed
through one public aggregate:

```cpp
#include <optionx_cpp/trading_system.hpp>
```

`TradingSystemStorage.hpp` and `ExecutionRecovery.hpp` remain implementation
headers inside the `trading_system` family. The separate
`trading_system_storage.hpp` aggregate was removed to avoid two public entry
points for one domain. `storages.hpp` remains reserved for generic/session
storage and does not include this domain.

The recovery layer owns:

- durable operation identity and client idempotency identity;
- the pre-submission and submission-started boundaries;
- simulated broker outcomes;
- unknown-execution and reconciliation state;
- physical settlement commit when authoritative terminal evidence is available.

The recovery layer does not guess series, reservation, or money-management
projections. A caller that has those projections supplies the existing
`SettlementTransition`; the transition is then applied through the Batch 5B
atomic path in the same local transaction.

`ExecutionRecoveryManager::recover()` returns a typed result. It rejects
malformed payloads, unsupported schemas, revision gaps or overflow, immutable
identity changes, and invalid state transitions. Callers must block admission
when recovery is not `READY`; they must not interpret an empty operation map as
proof that no operations exist.

## Lifecycle

```text
PREPARED
    -> SUBMISSION_STARTED
        -> BROKER_ACCEPTED -> BROKER_ID_PERSISTED -> SETTLED
        -> EXECUTION_UNKNOWN -> RECONCILIATION_REQUIRED
                                      -> BROKER_ID_PERSISTED
                                      -> REJECTED
```

`TIMEOUT` and a lost response are `EXECUTION_UNKNOWN`, even when the adapter
cannot currently show a history record. They never authorize an automatic
resend. `submit()` accepts only `PREPARED`, and therefore a restarted runtime
must reconcile an unresolved operation before progression.

`REJECTED` is terminal only when the broker model proves that no side effect
occurred. That transition atomically appends the terminal operation event,
records `NOT_SUBMITTED` evidence, marks the physical trade rejected, and
releases its reservation. An ambiguous or unavailable history remains
unresolved and blocks the affected risk scope. `UNKNOWN` is never rewritten to
`CANCELLED` merely because reconciliation did not find a record.

Repeated reconciliation of the same authoritative result is idempotent: it
returns the observation without advancing the operation revision a second
time. Repeated terminal settlement with identical terminal evidence returns
`DUPLICATE`; conflicting terminal evidence returns `CONFLICT`. A physical
`SETTLED` state is not by itself an admission-ready signal: unresolved
operations, active reservations, durable risk-scope gates, incomplete group
completion, or an invalid journal continue to block admission.

`admission_readiness()` also requires the caller to state that series, risk,
and money-management owners have been hydrated after restart; the default is
fail closed. `blocked_risk_scopes()` is retained as a compatibility projection
of those durable facts, while new admission code should consume the typed
readiness result.

## Crash boundaries

| Boundary | Durable fact after restart | Recovery action |
| --- | --- | --- |
| Before preparation commit | Nothing authoritative | No broker query or resend |
| After roster/reservation commit | Immutable physical roster | Continue from `PREPARED` |
| After `SUBMISSION_STARTED` commit, before broker call | Submission may not have happened | Query/reconcile before retry |
| During broker call or after timeout | `EXECUTION_UNKNOWN` | Preserve risk and reconcile |
| Accepted but response lost | Unknown until history proves identity | Match by operation/client identity plus account and trade |
| Accepted without broker ID | Accepted evidence without local broker ID | Reconcile authoritative history |
| Ambiguous history | `RECONCILIATION_REQUIRED` | Keep scope blocked; no resend |
| History unavailable | `RECONCILIATION_REQUIRED` | Fail closed and retry reconciliation later |
| Proven pre-submit rejection | `REJECTED` | Release reservation and physical trade atomically |
| Physical settlement commit | Immutable settlement and physical projection | Apply logical projections exactly once |
| Logical completion commit | Existing Batch 5B completion event | Duplicate callbacks are ignored |

The journal and physical settlement can be committed together by
`settle_reconciled()`. The external broker is never part of the MDBX
transaction, so local commit must not be described as atomic broker execution.

## Identity and matching

Every operation retains `AccountId`, `RiskScopeId`, `trade_id`, operation ID,
and client operation ID. The simulated adapter requires the account and
physical trade identity to agree with a history candidate. Multiple candidates
remain ambiguous. This prevents a matching client key from binding one
physical roster member to another account.

A roster can contain multiple accounts and risk scopes. Account aliases and
shared risk scopes remain governed by the Batch 5B account registry and risk
gate contracts; Batch 5C does not silently merge or duplicate them. The
multi-account crash test covers three physical members, a WIN, proven
no-submission rejection, timeout, restart/reopen, independent broker history
reconciliation, physical settlement, shared-scope reservation state, and
exactly-once logical group completion.

## Valuation and logical projections

Native settlement facts remain authoritative. If a caller supplies a
cross-currency `SettlementTransition` without a fresh valid valuation, the
existing storage layer returns `PENDING_VALUATION` and the operation/settlement
transaction is not committed as logically complete. A later valuation
resolution uses the persisted native fact and valuation provenance. No live FX
quote is fetched by Batch 5C.

## Offline verification

`tests/trading_system_batch5c_test.cpp` covers:

- durable preparation and JSON round-trip after reopen;
- timeout and lost-response handling;
- no-resend behavior;
- ambiguous and unavailable history;
- proven pre-submit rejection;
- corrupted-journal fail-closed admission;
- public transition and revision-overflow guards;
- idempotent reconciliation;
- operation plus physical settlement atomicity;
- settlement replay conflict detection;
- multi-account roster identity preservation and restart/reopen completion.

The simulated broker state is explicitly owned outside the OptionX runtime
object. A new adapter can therefore reopen against the same broker state in a
crash/restart test; this models an external side effect without tying it to the
short-lived runtime instance.

The simulated adapter is deliberately not a broker integration. Live platform
retry behavior, broker-specific history heuristics, external scheduling, and
manual reconciliation remain later runtime concerns.
