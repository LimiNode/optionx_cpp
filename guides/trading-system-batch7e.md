# Trading System 7E-0: Durable Delivery And Bounded Recovery

Status: proposed contract for independent review, not a production runtime.
7E-A (outbox) and 7E-B (recovery indexes) require separate code PRs. No new
message bus, DB engine, execution manager or scheduler is proposed.

## Existing Atomicity Audit

Paths are relative to `include/optionx_cpp/` unless stated otherwise.

| Owner / path | Actual guarantee |
| --- | --- |
| `trading_system/TradingSystemStorage.hpp`, `UnitOfWork` | All `ts_*` tables share one `mdbxc::Connection` and writable transaction. Commit validates touched roster/reservation bindings; failure aborts the UOW. |
| `ExecutionRecoveryManager::prepare()` in `trading_system/ExecutionRecovery.hpp` | Precommits intent, immutable roster, reservation bindings, physical IDs and PREPARED facts before submission. |
| `ExecutionRecoveryManager::mark_submission_started()` | Commits possible submission before scheduling; external calls are outside transactions. |
| `UnitOfWork::apply_physical_settlement_atomic()` | Physical settlement evidence, deduplication, group-settlement index and physical projection. Missing valuation preserves native facts without inventing logical profit. |
| `UnitOfWork::apply_group_completion_atomic()` | Validates terminal roster evidence/revisions; commits GROUP_COMPLETION, series, MM and all group reservation changes together. Physical/logical keys are distinct. |
| `trading_system/detail/OfflinePlatformOwner.hpp` | Existing platform tasks and bounded handoff; callback DTOs never perform MDBX work. Existing completion owner is invoked after physical commits. |
| `tests/support/trading_system/SyntheticHost.hpp::complete()` | Offline reference composition of aggregator, progression and UOW, not a second production completion engine. |
| `storages/TradeRecordDB/TradeRecordDB.ipp` | TradeRecordDB owns a connection; `write()`/`upsert()` reach methods committing their own transactions. There is no shared TS transaction or durable TS consumer ACK. |

TS schema is version 1. Tables: `ts_journal`, `ts_journal_ids`, `ts_intents`,
`ts_rosters`, `ts_reservations`, `ts_physical_trades`, `ts_accounts`,
`ts_risk_scope_gates`, `ts_admission_scope_index`, `ts_settlements`,
`ts_group_settlement_index`, `ts_valuation_resolutions`, `ts_series`,
`ts_mm_state`, `ts_sequences`. The transaction spans this environment, not
arbitrary TradeRecordDB calls placed inside an external guard.

No durable notification outbox, consumer ACK ledger or delivery cursor exists.
Callback acceptance is only in-memory acceptance. Lost observations before
receipt commit need reconciliation; an outbox cannot recreate broker evidence
never persisted. Commit followed by crash before completion/notification must
not reopen the trade. Destruction without explicit owner drain may lose queued
observations while leaving canonical uncertain risk intact.

## Consumers And ACKs

| Consumer | Delivery contract |
| --- | --- |
| Series/MM/risk | Canonical local UOW, not notification-driven financial progression. |
| Required history projection/export | At-least-once with idempotent durable target application and durable per-consumer ACK. |
| UI refresh | Best-effort hint; reread snapshots on reconnect. UI ACK never releases risk or determines correctness. |
| Broker submission | Not an outbox consumer in 7E-A. Notification replay never calls `place_trade()`. |

ACK means a named consumer durably applied an immutable event version, not that
all observers saw it. Projection lag has a typed health diagnostic. Whether it
blocks admission is explicit host policy; canonical evidence and uncertain risk
remain intact regardless. No ACK authorizes freeing an unresolved reservation.

## Proposed 7E-A Contract

Add minimal outbox tables in the existing TS environment. Eligible physical
and logical transitions append their notifications inside their own UOW or
append neither. An optional enqueue after commit is not transactional outbox.
An arbitrary journal label alone is not authority to publish a financial fact.

C++17 sketch, not a shipped API:

```cpp
struct DurableNotification {
    std::string notification_id;       // deterministic transition identity
    std::string consumer_id;           // stable allow-listed consumer instance
    std::uint32_t envelope_version;
    std::string canonical_event_key;
    std::uint64_t canonical_event_id;
    std::uint64_t aggregate_revision;
    std::string payload_type;
    std::uint32_t payload_version;
    nlohmann::json payload;            // bounded, validated, immutable snapshot
};
enum class ProjectionApply { APPLIED, DUPLICATE, RETRY, CONFLICT, UNSUPPORTED };
```

Identity derives from canonical event, consumer and envelope version, not a
new allocation per delivery attempt. Duplicate transitions verify exact
envelope equivalence; conflicting payload fails closed. Bind original account,
realm, operation/trade/group IDs, proven broker identity, persisted normalization
and native money/valuation facts as appropriate. Do not recalculate history
from current FX, account settings or normalization defaults. Credentials,
cookies, proxy URIs and tokens never belong in payloads.

Propose versioned immutable envelopes, pending delivery index ordered by
consumer/stable sequence, and immutable ACK receipts in the same connection.
Set explicit identifier, payload, page-item and page-byte limits. ACKs identify
event/version, consumer and exact target application receipt. Unknown versions,
foreign bindings and conflicting ACKs are errors. Exhausting a delivery retry
budget does not delete canonical evidence or silently acknowledge work.

Delivery reads a bounded page, closes the read transaction, calls the consumer
outside any writable TS transaction, then ACKs APPLIED/DUPLICATE in a new UOW.
RETRY remains pending; CONFLICT/UNSUPPORTED remain pending with typed diagnostics.
One owner serializes each consumer; distributed leases are out of scope.
Exceptions/shutdown before ACK leave the same identity eligible after restart.
Drive finite pages through existing owner-loop/TaskManager contracts.

| Crash boundary | Restart requirement |
| --- | --- |
| Before append / abort while appending | Neither canonical transition nor notification is visible. |
| Append done, before UOW commit | Both roll back. |
| Commit succeeded, before delivery | Recover same pending identity; never resubmit broker command. |
| Consumer failed before target commit | No ACK; retry identical payload. |
| Target commit succeeded, before TS ACK | Redeliver; target returns durable DUPLICATE without another financial effect. |
| During ACK commit | ACK is wholly absent or present. |
| ACK committed, before process notification | No pending redelivery for that consumer; status remains queryable. |
| Shutdown during delivery | Drain within owner lifetime or leave unacknowledged; never invent ACK. |

Tests must inject failures at every boundary plus envelope conflict, unsupported
version, stale revision, account/risk isolation, page/byte limits, independent
consumer ACKs and restart after target commit. Retain Batch 1--7D and C++17
consumer regressions. Do not call this exactly-once delivery.

## TradeRecordDB Projection

Journal remains canonical; TradeRecordDB is independently committed and
rebuildable. Plain unconditional `upsert()` is insufficient: stale deliveries
could overwrite newer data, or a trade ID could belong to another identity.
The real consumer needs a target-side persistent projection receipt containing
source namespace, canonical trade identity, revision, codec version and exact
payload identity. Target record and receipt/revision must commit together in
the TradeRecordDB environment. Equal replay returns DUPLICATE; stale replay
does not regress history; identity/revision conflict returns CONFLICT.

Preserve standalone TradeRecordDB public calls. Its target receipt extension,
trade-ID width/namespace mapping and legacy-record migration need a separate
compatibility review. 7E-A may test with a small durable idempotent fake consumer
without claiming that the real mirror is complete. An in-memory dedup set or a
TS ACK before target commit is not a substitute. Deleting a projection requires
an explicit rebuild generation; old ACKs cannot silently hide missing history.

## Restart Order

1. Validate schemas and migration/index generation; keep admission closed for
   unknown versions or incomplete generations.
2. Restore immutable account/realm/alias bindings, normalization profiles,
   reservations and risk gates. Inactive identities cannot be silently rebound.
3. Restore operation owners from verified durable heads/terminal evidence;
   SUBMISSION_STARTED and EXECUTION_UNKNOWN never imply resend.
4. Finish terminal groups missing logical completion via the existing
   idempotent owner; hydrate exact series/MM revisions.
5. Restore consumer ACKs and deliver pending projections after canonical
   owners are ready. Target DUPLICATE may then be durably ACKed.
6. Attach distinct scoped platform instances and schedule read-only known-ID
   reconciliation through `post_task()` where supported.
7. Admit new work only for consistent account/risk scopes. Reset/ACK cannot
   spend unresolved exposure. Scheduling acceptance is not execution evidence.

This is proposed orchestration, not an implemented production facade. 7E-A
does not change callbacks, TradeState, broker APIs or retry ownership.

## 7E-B Index Plan

Current startup is not bounded by outstanding work:

- `ExecutionRecoveryManager::recover()` calls
  `journal_events("EXECUTION_OPERATION")`, which filters after a full table
  load, then allocates/sorts all historical operation revision chains.
- Storage construction invokes `audit_admission_scope_index()`: a full
  integrity comparison, not a cheap trusted manifest check. Roster audits and
  collection helpers such as `reservations_for_scope()` scan historical data.
- Exact `journal_by_key()`/`ts_journal_ids` and ranged
  `ts_group_settlement_index` are reusable. The admission scope index has
  narrower ownership; it is not a complete unfinished-operation index.
- OfflinePlatformOwner considers every recovered operation's group, including
  completed history. Avoiding only journal scanning would not fix that cost.
- `SeriesCheckpoint` in TradingSystemStorage retains receipts, used intent IDs
  and used idempotency keys. `SeriesProgression::hydrate()` validates/rebuilds
  those historical maps even when only the current group can still progress.
- `SyntheticHost::reload_risk()` (test-only composition) loads all reservations,
  groups by scope/intent and looks up historical intent keys; `RiskAdmission::hydrate()`
  retains terminal as well as active reservation keys. These costs must not be
  silently copied into a production bootstrap path.

Propose operation-head index, outstanding work keyed by
`risk_scope/account/operation`, terminal groups awaiting completion, pending
valuation, and consumer-pending notification ranges. Entries reference exact
canonical keys/revisions. Updates/removal share the canonical UOW; remove a
group only with logical completion/reservation commit, not its last callback.
An index never independently authorizes resend or risk release.

With H historical facts and U outstanding items, target operation-discovery
`O(log H + U log H)` point/range reads, bounded pages and O(page + active owner
state) memory. Scope isolation belongs in keys, not post-load filters. This is
not yet an end-to-end startup bound: loading/validating C checkpoint bytes and
retained historical reservation/series keys adds history-dependent cost.
Full audits remain explicit maintenance. Define a trusted generation/checkpoint
rule: an isolated latest-head record does not prove a historical revision chain.

An end-to-end bounded-startup claim additionally requires a reviewed versioned
compact owner checkpoint and durable point-lookup deduplication contract for
old settlements/intents/reservations. Do not simply omit old keys from hydrate,
reuse reservation IDs, or discard terminal receipts to make the benchmark
faster. Keep existing pure owner APIs and legacy recovery compatible until that
migration is approved. If initial 7E-B only bounds work discovery, report that
limit explicitly and measure owner hydration/serialized checkpoint bytes
separately. Historical-copy-on-settlement cost is also separate from indexes.

Legacy conversion explicitly visits the full history (H facts; index writes,
lookups and per-operation chain validation add their own costs). Block mutations during
migration; either rebuild transactionally at supported small sizes, or
checkpoint bounded pages into a new generation and atomically publish a
verified manifest. Crash cannot expose half a generation. Resume/rebuild
deterministically; unsupported/corrupt state fails closed. Do not delete a
disagreeing index and trade on guessed facts.

Before implementation, compare 10^3/10^4/10^5 completed operations with fixed
unresolved account/group counts against full canonical recovery. Measure
point reads, index visits, latency and memory; inject crashes at head/index
updates, logical completion, migration checkpoints and manifest publication.
Keep 7E-B separate from 7E-A.

## Decisions And Stop Gates

- Approve consumer identities, required/UI distinction and payload budgets
  before 7E-A. No exactly-once claim across independent environments.
- Review real TradeRecordDB receipt compatibility separately; a fake consumer
  does not settle this decision.
- Approve index trust/checkpoint and legacy migration policy before 7E-B.
- Approve compact owner checkpoints/durable dedup lookup before claiming that
  complete startup or settlement cost is independent of completed history.
- Before SIM-4, review versioned run/epoch identity and one-way integer-to-DTO
  money conversion. Do not hash virtual string IDs into supposedly collision-
  free numeric broker IDs or reconstruct authoritative money from `double`.
- No live/demo execution, production runtime, SIM-4 or release publication.

Related: [7D-B](trading-system-batch7d-b.md),
[5B](trading-system-batch5b.md), [5C](trading-system-batch5c.md),
[SIM-2](simulator-core.md).
