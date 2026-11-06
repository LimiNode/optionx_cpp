# Trading System Batch 5B: Durable Storage

## Status

Batch 5B is the durable local-storage stage after the approved Batch 5A
architecture. It provides a narrow shared MDBX environment and a transaction
adapter for local facts. It does not submit broker commands, reconcile unknown
execution, or provide a `TradingSystem` runtime.

The storage API is exposed through
`<optionx_cpp/trading_system_storage.hpp>`. The existing
`<optionx_cpp/trading_system.hpp>` remains a lightweight aggregate for the
pure Batch 1-4 contracts and does not impose an MDBX link requirement on those
consumers.

## Transaction ownership

`TradingSystemStorage` owns one shared `mdbxc::Connection` and named tables.
`TradingSystemStorage::UnitOfWork` owns one writable `mdbxc::Transaction`.
Every repository operation receives that transaction through the
`KeyValueTable` transaction overloads. The standalone `TradeRecordDB` and
`SignalRecordDB` APIs are unchanged and are not silently included in this
Unit of Work.

An operation failure marks the Unit of Work aborted. `commit()` then refuses to
commit and rolls the transaction back. Destruction also rolls back an open
Unit of Work. The external broker call remains outside this local transaction.

## Named tables

| Table | Canonical fact or projection |
| --- | --- |
| `ts_journal` | Immutable journal events keyed by idempotency key |
| `ts_journal_ids` | Event-ID uniqueness index |
| `ts_intents` | Immutable logical intent payloads |
| `ts_rosters` | Immutable execution-group rosters and account/risk-scope members |
| `ts_physical_trades` | Physical trade identity and local lifecycle projection |
| `ts_accounts` | Account identity bindings, aliases and deactivated tombstones |
| `ts_risk_scope_gates` | Durable fail-closed gates for account/RiskScope conflicts |
| `ts_reservations` | Risk reservation state and revision |
| `ts_settlements` | Settlement receipt, native money and valuation evidence |
| `ts_valuation_resolutions` | Immutable second-stage FX valuation facts |
| `ts_series` | Series checkpoint, including receipt and idempotency history |
| `ts_mm_state` | Versioned policy-specific money-management state |
| `ts_sequences` | Durable next-value counters |

Records carry `schema_version = 1`. Unsupported versions are rejected before a
write and reported as `UNSUPPORTED_SCHEMA`; malformed durable data is reported
as a storage error on read.

## Typed outcomes and atomic settlement

Writes return `StorageResultKind` rather than a boolean. The relevant states
are `APPLIED`, `DUPLICATE`, `CONFLICT`, `REVISION_CONFLICT`, `NOT_FOUND`,
`INVALID_STATE`, `UNSUPPORTED_SCHEMA`, `PENDING_VALUATION` and
`STORAGE_ERROR`.

`apply_settlement_atomic()` checks the settlement identity before changing any
projection. An identical receipt returns `DUPLICATE` and does not advance the
series, reservation or money-management state. A different payload using the
same identity returns `CONFLICT`. A physical transition writes the journal,
settlement and physical-trade projection in the same local transaction. A
physical receipt does not by itself complete its execution group. Series
progression, logical outcome, MM state and risk release are written only when
the authoritative aggregate for the complete immutable roster is terminal.
Multi-scope groups may update all of their terminal reservations in the same
Unit of Work.

The transition has two deliberate forms:

| Input state | Durable effect | Logical progression |
| --- | --- | --- |
| Physical settlement, group still pending | Journal, settlement and physical trade | Forbidden; existing reservations remain active |
| Physical settlement with missing/stale FX | Same physical facts, marked pending valuation | Forbidden until valuation and group completion are proven |
| Complete terminal group with matching projections | Physical facts plus series, all group reservations and MM state | Exactly one logical advance |
| Repeated physical or logical event | No additional writes | `DUPLICATE` |

`UNKNOWN`, pending and non-terminal group aggregates never release the full
group risk. A logical transition must include the authoritative normalized
`GroupOutcome`; an arbitrary checkpoint outcome is rejected.

Native cross-currency settlement is retained even without an FX quote, or when
the supplied quote is stale, but the stored record is marked
`logical_outcome_ready = false` and the operation returns
`PENDING_VALUATION`. No logical WIN/LOSS/DRAW is fabricated. The pending
settlement still has a journal event, so recovery can distinguish a durable
native fact from a transition that was never recorded.

Pending valuation is completed by `resolve_pending_valuation_atomic()`. It does
not rewrite the native settlement: the selected FX fact and converted logical
profit are stored as an immutable valuation-resolution record, while the
logical projections are applied in the same Unit of Work. A resolved native
settlement is therefore excluded from `pending_valuations()` even though its
original native record remains unchanged.

Immediate and deferred valuation use the same fixed-point conversion and
aggregate validation. A fresh quote is not sufficient by itself: the converted
group result must match the supplied logical outcome. `ROUND_HALF_UP` rounds an
exact half away from zero and conversion rejects integer overflow.

An execution roster rejects duplicate physical trade identities before it can
be committed. This prevents repeated callback data from being mistaken for a
second member of a split group.

## Hydration

`SeriesProgression::hydrate()` accepts a durable checkpoint, a reconstructed
`ExecutionGroupAggregator`, receipt history and used intent/idempotency sets.
It validates a candidate owner before publishing it, so a failed hydration
does not partially replace the current owner.

The checkpoint stores the current `GroupOutcome`. Receipts from completed
groups remain idempotency history, but only receipts matching the current
group roster are replayed into the current aggregator. A terminal current
aggregator cannot be restored as `WAITING_OUTCOME`.

`RiskAdmission::hydrate()` validates account facts, scope ownership, duplicate
reservation identities and the next reservation ID before replacing its
in-memory maps. It preserves active risk and continues the durable sequence.
After a restart, the host reads `risk_scope_gate()` and applies its typed
blocked/ready decision through `RiskAdmission::restore_admission_gate()`.
Hydration and reconciliation remain available; only new admission is blocked
while a durable conflict gate is active.

These methods are owner boundaries, not crash recovery orchestration. Batch 5C
will decide how unresolved external operations are reconciled and when a risk
scope becomes admissible again.

The storage facade also exposes read-only recovery queries for journal event
types, intents, group receipts, pending valuations, reservations by risk scope
physical trades by lifecycle state and account bindings by RiskScope. Account
records contain only stable identity metadata, canonical/alias mapping,
realm, active status and RiskScope; credentials, cookies and proxy secrets are
never stored. They are projections over the same MDBX environment; they do not
perform broker history matching.

### Physical-trade boundary

`ts_physical_trades` is the Batch 5B physical-trade projection owned by
`TradingSystemStorage`. It is not a second claim that the standalone
`TradeRecordDB` has been updated. `TradeRecordDB` and `SignalRecordDB` retain
their existing connection and transaction ownership, so their public writes
must not be called as if they participated in this Unit of Work. Until a
transaction-aware repository bridge is introduced, hosts must treat
`ts_physical_trades` plus the logical journal as the durable Batch 5B source
for this transition and reconcile any external `TradeRecordDB` snapshot through
an explicit later protocol. There is no atomic commit spanning these separate
repository APIs or an external broker.

## Verification

`tests/trading_system_batch5b_test.cpp` uses real temporary MDBX environments
and covers atomic settlement, rollback, duplicate replay after reopen,
revision conflicts, immutable rosters including duplicate physical IDs,
physical-trade persistence, pending and stale valuation, recovery queries,
account tombstones and RiskScope conflicts, schema rejection, owner hydration
and durable sequence rollback. It also covers multi-step series hydration,
late-stage rollback after earlier projection writes, multi-member physical vs
logical settlement, immediate and deferred valuation consistency, exact-half
rounding, durable risk gates and malformed read rejection. The focused suite
contains 22 tests.

No test opens a broker connection or uses live credentials.
