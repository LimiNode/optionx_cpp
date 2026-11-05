# Trading System Batch 5A: Architecture and Durable Boundaries

## Status

This document is the Batch 5A architecture decision record. It is based on
`main@e7dfc085e1cdf953552d359939658b5c7b9bc343`, where Batches 1 through 4 are
merged. It defines the contracts required before durable storage and recovery
implementation; it does not add a runtime, a database schema, or broker calls.

OptionX remains a C++17 library for binary and closely related fixed-outcome
contracts. It is not becoming a general Forex, futures, perpetuals, or
cryptocurrency exchange engine. MegaConnector and ADELIA may compose these
contracts, but they remain host dependencies rather than OptionX dependencies.

## Reconciliation With The Existing Repository

No Batch 5 implementation or uncommitted Batch 5 changes were present on the
verified base. The following components are reusable:

| Existing contract | Batch 5A decision |
| --- | --- |
| `data/trading/TradeRequest.hpp` | Preserve `account_id`, `trade_id`, signal and execution fields. Extend only through an explicit compatibility migration. |
| `data/account/AccountInfoUpdate.hpp` | Preserve `account_id` as the internal account correlation field. |
| `data/trading/enums.hpp` | Reuse `PlatformType`, `AccountType`, `CurrencyType`, `TradeState` and existing JSON names. |
| `platforms/common/BaseTradingPlatform.hpp` | Reuse platform ownership, `TaskManager`, `EventBus`, `post_task()` and staged shutdown. |
| `platforms/common/BaseTradingApi.hpp` | Keep broker execution and reconciliation behind the existing platform boundary. |
| `trading_system/TradeSeries.hpp` | Reuse series identity and progression state; persistence must restore it rather than create a second state machine. |
| `trading_system/ExecutionGroupIdentity.hpp` and `ExecutionGroupAggregator.hpp` | Reuse fixed group identity, roster and terminal aggregation rules. |
| `trading_system/RiskAdmission.hpp` | Reuse in-memory admission semantics; Batch 5 binds reservations to durable records and risk scopes. |
| `storages/SignalRecordDB.hpp` | Reuse signal and signal-to-trade facts. It is not the logical operation journal. |
| `storages/TradeRecordDB.hpp` | Reuse physical trade snapshots and persistent `trade_id` allocation. It is not the complete intent/group journal. |
| `external/mdbx-containers` | Reuse MDBX as the only storage engine. Its table methods accept an optional `MDBX_txn*`, but current DB facades own their connections and transaction boundaries. |

The old Batch 5 outline was too broad for one implementation step. It did not
separate account identity from risk ownership, did not define multi-account
rosters or valuation provenance, and could be read as promising atomicity by
wrapping standalone `TradeRecordDB` calls. Batch 5 is therefore split into:

1. **5A** architecture and contracts, this document;
2. **5B** shared MDBX ownership, Unit of Work and durable projections;
3. **5C** simulated execution, reconciliation and crash recovery.

Work must stop at the 5A review gate before 5B begins.

## ADR-1: Identity And Ownership

The following identities have different meanings and lifetimes:

| Identity | Meaning | Rule |
| --- | --- | --- |
| `PlatformType` | Implementation family such as Intrade Bar or a future Polymarket adapter. | Not an account identity. |
| `AccountId` | Stable internal identity of one configured trading account. | Persistent, independent of list position, never silently rebound or reused while referenced. |
| External account identity | Provider account ID, realm and DEMO/REAL identity when authoritative. | Credentials and tokens are never journal data. |
| `RiskScopeId` | Financial resource against which risk is reserved. | Separate from `AccountId`; aliases share this scope. |
| `SessionId` | Explicit trading or operational interval selected by the host. | A process restart does not create a new session implicitly; it does not replace a series or delete history. |
| `SeriesId` | One logical progression of outcomes. | May use different accounts over successive steps. |
| `ExecutionGroupId` | One logical operation containing one or more physical trades. | Roster is fixed before the first possible submission. |
| `trade_id` | Existing OptionX identity for one physical trade record. | Preserve the `TradeRecordDB` identity path. |
| Broker identity | Provider-side order/trade identity. | May be absent until acceptance and must not be fabricated during recovery. |

`AccountId` is not a mandatory `BrokerInstanceId + AccountId` composite. A
platform instance may expose one configured account, but the trading system
stores the stable internal account identity and the authoritative external
identity separately. An identity-changing edit is rejected or requires a new
`AccountId`; it never changes the meaning of historical records.

### Account registry

The future `AccountRegistry` is a narrow owner of configured account metadata
and capabilities. It may hold `std::shared_ptr<BaseTradingPlatform>` or an
equivalent platform ownership handle, but policy modules receive read-only
capabilities rather than unrestricted account mutation.

It must support registration, lookup, enumeration, deactivation and canonical
alias resolution. Deactivation leaves a tombstone sufficient to recover old
operations. A duplicate is merged only when provider identity, realm and
DEMO/REAL status conclusively match. Email, display name or platform type alone
are insufficient. The deterministic tie-breaker for a proven duplicate is the
smallest persistent `AccountId`; other IDs remain aliases in history.

The canonical choice is an identity decision, not a financial migration. For
example, if account 20 already owns reservations in `RiskScopeId=5` and a new
account 10 is later proven to be the same external account, account 20's
existing trade references and reservations remain bound to their recorded
identities. Account 10 becomes a canonical alias for future lookups only after
the alias record and identity revision are durably committed.

If the two records carry different `RiskScopeId`s but are proven to represent
one physical balance, the registry enters a risk-scope conflict state. New
admission for both affected scopes is blocked until an atomic merge or an
explicit conflict resolution establishes one scope. Existing reservations are
not silently moved, duplicated or released. Alias history, tombstones and
identity revisions must survive restart.

## ADR-2: Multi-account Execution

An execution group may contain physical members targeting different accounts:

```text
TradeSeries -> ExecutionGroup
              -> PhysicalTrade(account A)
              -> PhysicalTrade(account B)
```

Before submission, persist an immutable roster containing the group identity,
expected member count, physical `trade_id`s, target `AccountId`s, risk scopes,
native stake facts, execution parameters and idempotency identities. Callback
count is never evidence that the roster is complete, and a duplicate callback
cannot satisfy another member.

Risk admission is prepared per actual `RiskScopeId`, with any desired group
limit applied in addition. Shared aliases must not reserve funds twice. Local
records can be committed atomically in one MDBX transaction, but external
broker submissions across accounts are never atomic. Partial submission,
partial execution and `EXECUTION_UNKNOWN` remain valid states.

## ADR-3: Currency And Deterministic Valuation

`TradingSystemConfig::base_currency` is a reporting and consolidation currency,
not a constraint on account, stake or execution currency. Native facts remain
first-class. For example, a 500 RUB stake is stored as 500 RUB even when the
system reports a USD projection.

Whenever conversion affects a persisted decision or aggregate, store an
immutable valuation fact containing:

- source and target currencies and quote direction;
- fixed-point rate, declared precision and rounding rule;
- provider/source identity;
- observation/effective timestamp;
- snapshot or version identity;
- availability and freshness state.

The global base currency cannot rewrite historical trades. A missing or stale
required quote blocks a new converted decision; it cannot erase an accepted
native settlement. Cross-currency group aggregation is a later extension that
preserves native P&L, converts with persisted valuation facts, and records the
valuation policy. No FX aggregation engine is part of Batch 5A or 5B.

In 5B, native settlements are always durable even when a required valuation
fact is unavailable. A group may therefore have all physical members terminal
while its logical aggregate remains non-terminal with `AggregationStatus::PENDING`.
It must not receive a confirmed logical `WIN`, `LOSS` or `DRAW`, must not expose
a fabricated net P&L, and must not advance `SeriesProgression` until the
valuation dependency is resolved or an explicitly approved policy handles it.
This uses the existing pending/blocking mechanisms; it does not introduce a
fake `OutcomeKind`. A later versioned revaluation may change a reporting
projection, but it cannot rewrite a confirmed broker outcome implicitly. Risk
remains reserved by default while the valuation dependency blocks logical
progression; releasing it earlier requires an explicit durable policy and must
not be inferred from physical terminality alone.

## ADR-4: Series, Money Management And Composition

`TradeSeries` owns logical progression, `ExecutionManager` owns physical
submission and reconciliation, `RiskAdmission` owns reservations, and a money
management policy owns only its strategy state. Stateless policies do not need
an artificial historical payload. Stateful policies use a versioned state
record:

```text
module_instance_id, state_type, schema_version, series_id,
state_revision, last_settlement_id, validated_policy_payload
```

The payload must distinguish absent, uninitialized, valid, unsupported schema,
corrupt and reconciliation-required states. A settlement may advance this
state at most once.

The public direction is `TradingSystem`; the internal runtime is
`TradingSystemRuntime`; execution and recovery belong to `ExecutionManager`.
No separate `TradingCoordinator` is introduced. A first implementation may use
a typed sequential pipeline:

```text
signal -> normalization -> filters -> account selection -> sizing
       -> risk admission -> group roster -> execution -> settlement
       -> money-management progression
```

Modules declare small capabilities and the host-owned pipeline configuration
defines connections and ordering. OptionX does not become a graph editor or a
general unrestricted graph runtime.

## ADR-5: Durable Journal And MDBX Unit Of Work

The durable source of truth is a journal of logical operation facts and
idempotency identities, with projections for query convenience. It must retain
intent, group roster, physical links, reservations, submission state, broker
observations, settlements, series revisions and strategy-state revisions.

One confirmed settlement must be applied through one local Unit of Work that
coordinates the physical trade snapshot, settlement receipt, group state,
series progression, risk release/retention and money-management state. The
settlement identity and all revision checks are part of that same transition.

The current `TradeRecordDB` and `SignalRecordDB` public methods open their own
transactions through internally owned connections. Therefore this is **not** a
valid atomicity claim:

```text
external_transaction { trade_record_db.upsert(); save_series(); }
```

5B must introduce a shared environment/connection owner and transaction-aware
repository operations, or a narrowly scoped storage adapter that performs all
related writes with the same `MDBX_txn*`. Existing standalone public methods
remain compatible. If the implementation cannot share a transaction, it must
use an explicit recovery protocol and must not document the writes as atomic.

The external broker call is always outside the MDBX transaction. Local commit
can make a command ready and mark submission-started, but it cannot commit the
broker side effect.

### Owner hydration contract

`TradingSystemRuntime` owns startup recovery. It opens the storage, validates
schema/version and journal integrity, then creates or hydrates the in-memory
`SeriesProgression`, `ExecutionGroupAggregator` and `RiskAdmission` owners.
The journal is canonical; these objects are projections that may accept new
work only after hydration succeeds.

The required order is:

1. Open storage and validate schema/version.
2. Verify journal integrity, durable identities and idempotency indexes.
3. Restore immutable group rosters and physical links.
4. Restore active reservations and the reservation ID sequence for each risk
   scope.
5. Restore series and strategy-state projections with revision/receipt history.
6. Check owner projections against the canonical durable facts.
7. Mark unresolved operations and schedule their reconciliation.
8. Enable admission only for risk scopes whose required invariants are restored.

The runtime, not an individual policy module or broker callback, owns this
sequence. A scope that is not hydrated, has a journal conflict, or depends on
unresolved identity/valuation facts remains closed for new admission. Recovery
does not require all unrelated scopes to wait for one another.

If a durable commit succeeds but in-memory publication fails or is rejected,
the runtime does not roll back the database or guess a result. It marks the
projection not ready, reloads it from the journal, and keeps the affected
admission gate closed until the projection matches. After commit, all owner
updates are serialized through the existing owner/task loop. Batch 5 does not
claim thread/process safety beyond that owner contract.

Settlement receipts are inserted with a durable uniqueness key before the
series, group, risk and policy projections are published. Replaying the same
receipt is a no-op; a conflicting payload is a recovery error. No new
settlement is accepted from an in-memory owner whose durable receipt cannot be
confirmed.

## ADR-6: Recovery, Unknown Execution And Reset

`RECONCILE` is an action, `PAUSE` is a progression gate, and `RESET_SERIES` is
an explicit new progression. They are not alternatives in one enum.

The durable lifecycle distinguishes:

```text
not_submitted -> ready/submission_started -> accepted/identified
              -> executed -> settled
                         \-> execution_unknown -> reconcile
```

A timeout after a possible broker side effect is `EXECUTION_UNKNOWN`. It keeps
risk reserved and cannot trigger an automatic resend. Reconciliation uses
proven broker/client identity first, then authoritative history, while
ambiguous heuristic matches remain unresolved. A reset never converts unknown
exposure into cancellation or releases its risk.

The recovery owner must restore unresolved operations before accepting new
progression for the affected risk scopes. A configurable failed-reconciliation
policy may pause or create a new series, but the old series, physical records
and unresolved risk remain durable.

## ADR-7: Bounded Future Polymarket Compatibility

The future adapter may support a bounded binary outcome subset. It must preserve
order identity, fill identity, cumulative and remaining quantities, execution
mode/fill policy and expiration where applicable. `execution_status` remains
orthogonal to `TradeState`: partial execution is not a financial outcome.

Batch 5 stores extension-compatible facts only. It does not implement a live
Polymarket adapter, order book, position engine, or general exchange runtime.

## Crash Boundary Matrix

| Boundary | Durable interpretation after restart | Safe action |
| --- | --- | --- |
| Before intent persistence | No accepted operation exists. | No recovery action. |
| After reservation, before roster commit | Local admission is incomplete. | Recover or roll back only through the same local transaction. |
| After roster/request commit, before submission | Definitely not submitted. | Resume through the owner, without creating new identities. |
| Submission-started before broker response | Side effect may exist. | Mark/retain `EXECUTION_UNKNOWN`; no resend. |
| Broker accepted before broker ID persistence | Accepted identity is incomplete. | Reconcile by durable command identity/history. |
| Result before settlement commit | Physical fact exists; logical state may lag. | Apply settlement idempotently in one Unit of Work. |
| During series/MM/risk update | Transition may be partially visible only if atomicity is broken. | Use transaction recovery or explicit journal replay; never guess a result. |
| After local commit before notification | Facts are authoritative. | Replay notification from durable state. |
| Multi-account partial submission | Some roster members may have broker effects; the group is not complete. | Keep submitted/unknown members reserved, reconcile each member, and do not resend ambiguous members. |
| Crash after one physical execution in a group | One member is durable or observed while other members may be absent. | Restore the fixed roster and keep group progression pending until every member is known terminal or explicitly unresolved. |
| Account identity change or deactivation | Historical account binding remains valid; new capability may be unavailable. | Keep a tombstone, block unsafe new admission, and reconcile through the recorded identity. |
| Journal restored before owner hydration | Durable facts exist but in-memory projections are not authoritative yet. | Keep admission closed for that scope until owner hydration and consistency checks pass. |
| Reset requested with a pending group | Reset is a new progression request, not cancellation of exposure. | Pause or create a new series while the old group and risk remain visible. |
| Broker-side pending order during downtime | External order may remain active even without a local result. | Reconcile by broker/client identity; no automatic resend or risk release. |
| Callback accepted in memory before durable commit | Observation is not yet canonical. | Do not publish terminal progression; retain/requeue the journal write, or leave the operation unresolved for reconciliation if the observation is lost. |
| New canonical alias with active reservations | Account lookup identity changed while financial state is live. | Preserve old references and block conflicting risk scopes until an atomic alias/scope resolution. |

## Batch 5A API Sketches

These sketches are documentation contracts, not code to add in this PR:

```cpp
struct AccountDescriptor {
    AccountId account_id;
    PlatformType platform;
    AccountType realm;
    std::optional<ExternalAccountIdentity> external_identity;
    RiskScopeId risk_scope_id;
};

struct ValuationFact {
    CurrencyType source;
    CurrencyType target;
    FixedPointRate rate;
    std::string provider;
    std::uint64_t observed_at_ms;
    std::string snapshot_id;
    std::uint8_t precision;
};

struct OperationJournal {
    JournalResult apply_settlement(const SettlementTransition&, UnitOfWork&);
    std::vector<UnresolvedOperation> find_unresolved() const;
};
```

The eventual API must preserve existing serialized field types and aggregate
include ownership. It must not expose credentials, cookies, proxy secrets or
tokens in journal records.

### Conceptual 5B Unit of Work

This is a bounded interface sketch, not a storage framework or implementation
for this PR. It shows the required commit boundary and hydration gate:

```cpp
auto transaction = storage.begin_unit_of_work();
transaction.physical_trades().upsert(trade_snapshot);
transaction.settlements().insert_if_absent(settlement_receipt);
transaction.series().update_if_revision(series_snapshot, expected_revision);
transaction.reservations().apply_settlement(reservation_id, settlement_id);
transaction.strategy_state().advance_once(module_id, settlement_id, next_state);

if (!transaction.commit()) {
    // No projection is published; durable facts are rolled back.
    return StorageResult::ROLLED_BACK;
}

runtime.reload_projection_from_journal();
runtime.enable_admission_after_hydration(risk_scope_id);
```

The conceptual repositories receive one transaction context and share one
MDBX environment. A failed commit publishes no partial local transition. A
successful commit followed by projection failure leaves the journal canonical,
keeps the admission gate closed, and schedules owner rehydration; it never
releases risk based only on an in-memory snapshot.

## Rejected Alternatives / Rationale

- **Two independent progress databases:** rejected because series, risk,
  roster and settlement receipts could commit at different revisions. One
  shared journal/UoW makes the local atomicity boundary explicit.
- **Money-management policy calls the broker directly:** rejected because it
  couples strategy state to transport retries and prevents one owner from
  coordinating execution, risk and recovery. `TradingSystemRuntime` and
  `ExecutionManager` keep those responsibilities separate.
- **Delete duplicate accounts:** rejected because historical `AccountId`s and
  trade references would disappear. Canonical aliases preserve history and
  make identity revisions recoverable.
- **Hard-coded module links:** rejected because replacing a filter or sizing
  policy would require changing unrelated modules. The host owns a bounded,
  typed pipeline configuration.
- **Automatic reset after unknown exposure:** rejected because it can release
  or double-spend risk while a broker-side operation remains live. Pause and
  reconciliation preserve the old exposure; reset is explicit and idempotent.
- **A universal Forex/crypto runtime for Polymarket compatibility:** rejected
  because the supported future subset needs fill-aware binary execution facts,
  not an unrelated order book, position engine or FX aggregation framework.

## 5A Delivery And Review Gate

This batch is complete when:

- the existing reuse matrix and architecture docs agree with this ADR;
- account, risk-scope, series, group, physical-trade and session lifetimes are
  explicit;
- native money and valuation provenance are replayable;
- cross-currency terminal physical facts cannot advance logical progression
  without valuation evidence;
- owner hydration and per-risk-scope admission gates are explicit;
- the MDBX transaction ownership limitation is explicit;
- crash boundaries and `EXECUTION_UNKNOWN` behavior are documented;
- rejected alternatives are recorded here;
- no runtime, schema migration, broker adapter or live test is included.

After review, 5B may implement the shared transaction adapter in a separate
Draft PR. 5C remains a separate simulated recovery stage. No Batch 5 PR is to
be merged automatically from this architecture pass.
