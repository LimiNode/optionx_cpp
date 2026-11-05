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
| `ts_reservations` | Risk reservation state and revision |
| `ts_settlements` | Settlement receipt, native money and valuation evidence |
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
same identity returns `CONFLICT`. A successful transition writes the journal,
settlement, physical trade, series, reservation and policy projections in the
same local transaction.

Native cross-currency settlement is retained even without an FX quote, but the
stored record is marked `logical_outcome_ready = false` and the operation
returns `PENDING_VALUATION`. No logical WIN/LOSS/DRAW is fabricated.

## Hydration

`SeriesProgression::hydrate()` accepts a durable checkpoint, a reconstructed
`ExecutionGroupAggregator`, receipt history and used intent/idempotency sets.
It validates a candidate owner before publishing it, so a failed hydration
does not partially replace the current owner.

`RiskAdmission::hydrate()` validates account facts, scope ownership, duplicate
reservation identities and the next reservation ID before replacing its
in-memory maps. It preserves active risk and continues the durable sequence.

These methods are owner boundaries, not crash recovery orchestration. Batch 5C
will decide how unresolved external operations are reconciled and when a risk
scope becomes admissible again.

## Verification

`tests/trading_system_batch5b_test.cpp` uses real temporary MDBX environments
and covers atomic settlement, rollback, duplicate replay after reopen,
revision conflicts, immutable rosters, physical-trade persistence, pending
valuation, schema rejection, owner hydration and durable sequence rollback.

No test opens a broker connection or uses live credentials.
