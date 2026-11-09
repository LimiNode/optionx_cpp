# Trading System Batch 5C: Offline Execution Recovery

Batch 5C adds an offline execution journal, a simulated execution adapter, and
fail-closed reconciliation on top of the Batch 5B storage contract. It does not
submit to a live broker and does not introduce another storage engine.

## Boundary

`TradingSystemStorage::UnitOfWork` remains the only local transaction owner.
Execution recovery stores immutable `EXECUTION_OPERATION` journal events. The
latest operation state is rebuilt by operation ID and state revision; the event
ID is used as a deterministic tie-breaker.

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
occurred. An ambiguous or unavailable history remains unresolved and blocks
the affected risk scope. `UNKNOWN` is never rewritten to `CANCELLED` merely
because reconciliation did not find a record.

Repeated reconciliation of the same authoritative result is idempotent: it
returns the observation without advancing the operation revision a second
time. Repeated terminal settlement is also a duplicate, not a second physical
settlement.

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
| Proven pre-submit rejection | `REJECTED` | Release/advance risk through the owning runtime policy |
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
gate contracts; Batch 5C does not silently merge or duplicate them.

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
- idempotent reconciliation;
- operation plus physical settlement atomicity;
- multi-account roster identity preservation.

The simulated adapter is deliberately not a broker integration. Live platform
retry behavior, broker-specific history heuristics, external scheduling, and
manual reconciliation remain later runtime concerns.
