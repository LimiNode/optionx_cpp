# Trading System Batch 6: Synthetic Vertical Slice

Batch 6 is a bounded offline composition slice. It proves that the existing
filter, sizing, risk, durable-storage, settlement and recovery contracts can be
used by one ordinary C++17 caller. It is not a production `TradingSystem`
facade, scheduler, graph engine or broker adapter.

## Completion

Batches 1-6 are merged. Batch 6 was accepted on
`854d4d95a32e6e0ca7c419d738bee1372d9330f8` and squash-merged in
[PR #192](https://github.com/LimiNode/optionx_cpp/pull/192) as
`47ec37b825da9163bfa7ed3fe09f2d4dff0d1797`.
Post-merge [Ubuntu Smoke](https://github.com/LimiNode/optionx_cpp/actions/runs/44958086612)
and [Windows Smoke](https://github.com/LimiNode/optionx_cpp/actions/runs/44958086599)
passed on that merge SHA. Its tree matches the reviewed HEAD exactly.
Local MinGW/MSVC regression reruns passed 189/189 each, including 37 Batch 6
tests, plus three C++17 consumer executables per compiler.

These are offline guarantees. No live opening, server-side idempotency,
complete broker history or live recovery correctness was established by this
batch. The next gate is [documentary Batch 7A](trading-system-batch7a.md), not a
production promotion of SyntheticHost.

## Pipeline

The test-only `optionx::test_support::SyntheticHost` in
`tests/support/trading_system/SyntheticHost.hpp` composes the following stages:

```text
synthetic signal
  -> SymbolFilter / TradingWindowFilter
  -> FixedStake or LossStepStake
  -> RiskAdmission owners
  -> TradeIntent + immutable ExecutionRosterRecord
  -> ExecutionRecoveryManager::prepare
  -> SimulatedExecutionAdapter
  -> physical settlement evidence
  -> ExecutionGroupAggregator
  -> SeriesProgression
  -> atomic durable series, reservation and MM projections
```

The host owns no alternate execution or progression state machine. It calls the
existing Batch 1-5C contracts and explicitly separates side-effecting stages so
tests can destroy the host between boundaries.

## Durable boundaries

Preparation persists the intent, roster, per-member reservations, physical
trades and operation journal. When a series revision is supplied, the same Unit
of Work also applies the canonical `SeriesProgression::begin()` transition. A
crash after preparation therefore restores a waiting series instead of an idle
series with an outstanding operation.

Broker state is held by `SimulatedExecutionAdapter::State`, outside the host and
outside MDBX. A lost response is reconciled against that independent state; the
host never resends a possibly submitted operation. Proven no-submission
rejection is persisted as `REJECTED` and releases only that member's
reservation. `WIN`, `LOSS` and `DRAW` settle their member; an all-rejected group
completes without advancing the financial series step. Mixed groups preserve
the per-member terminal states. Physical settlement and logical group
completion remain separate durable transitions. Group completion updates the
series, MM payload and every member reservation in the existing atomic Batch 5B
path.

The harness is single-threaded. `RiskAdmission` owners are authoritative for
one risk scope and are recreated only after durable facts are read. Shared-scope
members are aggregated into one in-memory reservation token for admission while
their physical reservation records remain separate in the roster and storage.
Hydration uses a conservative group projection: any active member contributes
its risk, and the projection takes the strongest unresolved state. Released or
settled members contribute no active risk. Thus mixed physical states cannot
erase unresolved exposure or overwrite individual durable states.

Simulated broker settlement IDs use an independent high-bit namespace and a
broker-state high-water counter. They are never derived from `TradeId`; local
rejection evidence continues to use the storage settlement sequence.
The explicit settlement-sequence rebuild scans only the local namespace,
preserves a valid higher local counter and repairs a legacy counter that was
incorrectly advanced by external IDs. The external allocator stops without
advancing when its low 63-bit counter is exhausted.

## Scenarios and guarantees

`tests/trading_system_batch6_test.cpp` covers:

- accepted BUY, WIN, durable completion and exactly-once progression;
- symbol and time-window rejection without durable execution side effects;
- insufficient, unavailable and stale risk facts;
- multiple-scope admission rollback before preparation;
- one capped loss step with checked multiplier/overflow and durable restore;
- crash/reopen after preparation, possible broker side effect, physical
  settlement and logical commit;
- mixed-scope restart followed by broker-history reconciliation, logical
  completion and a second reopen;
- multi-account groups, shared scope aggregation, late member completion and
  immutable physical account identities;
- rejected conflicting simulated terminal evidence and settlement namespace
  boundary/rebuild cases;
- durable policy/account-binding changes failing closed.

The corrective regressions also cover roster order beyond single-digit IDs,
simulated broker identity binding, an indexed UNKNOWN before terminal evidence,
and terminal anchor/idempotency after reopen. Scope-kind conflict and malformed
claims forbid commit; reopen confirms no partial writes. Ordinary reservation
writes perform one persistent identity point lookup and zero historical visits
at both 32 and 2,048 completed records. The lookup has MDBX tree cost, not a
claim of constant-time complexity.

The integration test uses deterministic synthetic time and a real MDBX environment. No live
credentials, network broker or wall-clock sleep is involved.

The companion `trading_system_batch6_consumer_test` is intentionally separate:
it includes only `<optionx_cpp/trading_system.hpp>` and connects public filters,
sizing, series and aggregation contracts without test support or example paths.

## Loss-step policy

`LossStepStake` is deliberately minimal: it evaluates a fixed base stake and,
when the persisted `after_loss` flag is true, applies one configured integer
multiplier. It rejects multipliers not greater than one and checked arithmetic
overflow. The host persists the flag and validates the complete policy
configuration on restart. This is not a complete Martingale/Kelly/Labouchere
framework.

## Scope limits

The test harness does not provide live execution, a generic module graph, a
production scheduler, broker-specific reconciliation, FX conversion, GUI,
MegaConnector/ADELIA integration or a new storage engine. Those remain later
integration boundaries. Batch 7A first defines how existing platform contracts
can be connected without turning this harness into the production runtime.

## Admission Projection Upgrade

The rebuildable `ts_admission_scope_index` now stores `@scope-kind:<id>` claims
outside active `scope:<id>:` ranges and a `scope_identity_version: 1` marker.
Claims survive settlement/release so a numeric `ACCOUNT` scope cannot later be
reused as a `STRATEGY` scope. They are written in the same MDBX transaction as
reservations; ordinary writes do not reload all reservation history.

For an older projection:

1. Stop admission and quiesce **all** writers to the environment. Take a
   consistent backup with the host's normal MDBX maintenance procedure.
2. Open with the upgraded binary in maintenance mode. Startup audit compares a
   rebuild in a rolled-back transaction; it never silently repairs the index.
   A failed audit keeps new admission closed.
3. In one UOW, call `rebuild_admission_scope_index()` and check for `APPLIED`
   before `commit()`. Any failure, including mixed scope kinds in canonical
   history, must roll back the complete UOW. Do not delete or rewrite financial
   facts to make an invalid rebuild pass.
4. Check successful commit and rerun `audit_admission_scope_index()`. Before
   resuming execution, run recovery/readiness checks and hydrate authoritative
   risk/series/MM owners from durable facts; a projection audit is not complete
   execution-chain recovery.
5. Resume only compatible writer binaries. Do not mix old/new library writers
   against the same environment. Raw `connection()` maintenance writes require
   another quiescent audit/rebuild or reopen before runtime admission.

No canonical DTO, named-table or storage-schema migration is involved. This is
an explicit projection upgrade, not permission for an online rolling upgrade
with incompatible writers. See [recovery details](trading-system-batch5c.md)
and the [API audit](API_CONSISTENCY_AUDIT.md).
