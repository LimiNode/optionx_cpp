# Trading-System API Consistency Audit

This bounded audit records the Batch 6 review conclusions. It is an audit of
the current contracts, not a request for a repository-wide rename or a durable
schema migration.

## Boundary Classification

| Component | Classification | Contract |
| --- | --- | --- |
| `include/optionx_cpp/trading_system.hpp` | Public aggregate | The supported C++17 entry point for the trading-system domain. |
| `LossStepStake` | Public offline policy | Pure sizing policy; it owns no series, broker, or storage state. |
| `FakeExecutionClock` | Public offline capability | Deterministic clock for recovery simulations and tests. |
| `SimulatedExecutionAdapter` | Public offline capability | Explicit simulated broker boundary; it is not a live adapter. |
| `tests/support/trading_system/SyntheticHost.hpp` | Test support | Integration harness in `optionx::test_support`; not installed API and not an example. |

## Findings

### Monetary boundary

`MoneyAmount` is the authoritative fixed-point representation used by sizing,
risk reservations, settlements and durable records. The existing
`TradeRequest::amount` field is a legacy broker-compatible `double`. The
synthetic harness converts the validated decimal representation only when it
populates that request DTO. This is an explicit adapter boundary, not a change
to the domain money model. Future live adapters must define finite-value,
precision, range and broker-rounding rules before sending the request.

### Lifecycle boundaries

`ExecutionOperationState` is the typed state machine for journaled execution
operations. `RiskReservationState` describes reservation exposure. The
persisted `PhysicalTradeRecord::lifecycle_state` is a separate compatibility
projection used to query physical records. Similar labels such as `EXECUTED`
must not be treated as interchangeable transitions between these machines.
Operation transitions are validated by `ExecutionRecoveryManager`; reservation
release/settlement is derived from terminal settlement evidence.

### Risk-scope identity

Canonical reservation records persist both `RiskScopeKind` and `risk_scope_id`.
The admission projection is deliberately keyed by the numeric scope ID. Its
`@scope-kind:<id>` keys bind each ID to one `RiskScopeKind`, including after all
its reservations settle or release. Each reservation write checks this key in
the same MDBX writer transaction as its canonical record; a conflicting kind,
malformed claim or unsupported schema aborts the entire UnitOfWork. Rebuild
derives claims from all canonical reservations and likewise aborts on mixed
kinds. Reusing one numeric ID for different scope kinds is therefore invalid
configuration, not a supported aliasing mechanism. The kind remains part of the
canonical record and is checked when records are hydrated. The
in-memory `RiskAdmission` API still receives a caller-supplied typed scope, so
callers must preserve that global uniqueness invariant before any durable record
exists.

This is a bounded, rebuildable projection upgrade, not a canonical DTO/table
migration. The existing admission index marker now includes
`scope_identity_version: 1`. An older projection fails closed until the existing
explicit `rebuild_admission_scope_index()` commits; startup audit does not repair
it. All runtime writers of one environment must use this projection version;
mixed old/new writer binaries are unsupported. No new named table is introduced.

Ordinary writes perform one identity point lookup (MDBX tree lookup, not O(N)
history traversal) and at most one claim insert, independent of completed
history. The scaling test measures zero historical visits per fresh UnitOfWork
at both 32 and 2,048 completed reservations. Startup audit/rebuild remains a full
maintenance pass. Claims add O(distinct scope IDs) persistent entries and are not
copied into a per-transaction historical map.

Rejected alternatives: a per-UnitOfWork map reloads all history on every write;
a process-local cache cannot arbitrate independent MDBX writers. Durable claims
instead share MDBX's serialized writer boundary, so a later writer observes the
previous committed claim. Raw `connection()` maintenance writes require a
quiescent audit/rebuild or reopen before resuming runtime, as documented in the
[recovery guide](trading-system-batch5c.md). Arbitrary concurrent raw-table
mutation is not a supported integrity contract.

### Result and lifecycle APIs

`StorageResult`, `RiskAdmissionResult`, `SizingDecision`, `PolicyDecision`,
`ProgressionResult` and `ReconciliationResult` retain domain-specific result
semantics. They are not collapsed into one generic result. Their success and
duplicate/conflict states are interpreted at the owning boundary, while
exceptions remain reserved for malformed or impossible local state.

## Include and Consumer Verification

Integration tests may include test support through the test target's `tests`
include root. The standalone Batch 6 consumer test includes only
`<optionx_cpp/trading_system.hpp>` and connects public filter, sizing, series
and aggregation components. It deliberately does not include `examples/` or
`tests/support/`.

Canonical DTOs, named tables, SettlementId namespace rules and unrelated public
APIs are unchanged. The rebuildable admission projection version and two UOW
diagnostic counters are the bounded additions described above.
