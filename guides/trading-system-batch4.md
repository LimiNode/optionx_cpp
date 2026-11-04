# Trading System Batch 4

Batch 4 adds a broker-independent, in-memory risk-admission owner. It owns
reservations for exactly one explicit scope: an account, strategy, or group.
Separate `RiskAdmission` instances do not coordinate. A host that shares risk
must therefore share one owner for that scope; persistence and cross-process
coordination remain Batch 5 responsibilities.

## Admission Contract

`RiskRequest` carries the stable `IntentId`, intent idempotency key, execution
group identity, per-physical `stake`, and the complete logical operation's
`potential_risk`. For `SPLIT_GROUP`, `potential_risk` is the full group risk,
not one member's stake. Batch 4 does not infer or recalculate this value.

Admission requires a supported and fresh `RiskAccountSnapshot`. The caller
passes `now_ms` explicitly, so freshness is deterministic and does not read a
system clock. A snapshot is stale when it is from the future or older than the
configured maximum age. The owner rejects currency, precision, non-positive,
and overflowing monetary values without mutating reservations.

The balance check and reservation insertion happen under one owner mutex. Thus
two concurrent clients of the same owner cannot reserve the same remaining
risk. A newer account snapshot may reduce future capacity; it does not revoke
existing reservations.

## Lifecycle

```text
reserve -> RESERVED -> commit -> SUBMITTED
                         |              |
                         v              v
                      RELEASED       EXECUTION_UNKNOWN -> SETTLED
```

`release()` is only valid before submission and is idempotent after release.
`mark_unknown()` preserves the full reservation for an ambiguous submission;
it is not a release operation. `settle()` is the explicit reconciliation path
from `SUBMITTED` or `EXECUTION_UNKNOWN` and releases the reserved risk exactly
once. Repeated transitions return `DUPLICATE`; invalid transitions leave state
unchanged.

## Identity And Snapshot Scopes

The idempotency key and intent identity are unique for the lifetime of this
owner. Repeating the exact request returns the existing reservation. Reusing a
key with a different payload, or reserving the same intent under another key,
returns `CONFLICT`. Reservation IDs are owner-generated and are bound to the
scope, intent, key, group and monetary payload when transition tokens are
validated.

Snapshot revisions are monotonic. An older revision is rejected, and the same
revision with different balance facts is a conflict. Snapshot revision is
provider/account state, not a reservation revision. Account-wide uniqueness
across processes, restart-safe deduplication, and journal recovery belong to
the future host/admission and persistence layers.

## Non-Goals

This batch has no broker calls, scheduler, retry policy, MDBX journal, account
provider, execution strategy, or live trading. It does not release risk for a
timeout or cancellation unless the caller has proven that submission did not
occur. It does not change the existing DTO JSON contracts.

The focused tests cover stale and missing snapshots, full split-group risk,
balance and risk limits, duplicate/conflicting requests, release/unknown/
settlement lifecycle, snapshot revisions, currency/precision and numeric
boundaries, and concurrent admission attempts.
