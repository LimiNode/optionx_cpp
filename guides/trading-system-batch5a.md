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
| `SessionId` | Operational interval or host run boundary. | Does not replace a series and does not delete history. |
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

## 5A Delivery And Review Gate

This batch is complete when:

- the existing reuse matrix and architecture docs agree with this ADR;
- account, risk-scope, series, group, physical-trade and session lifetimes are
  explicit;
- native money and valuation provenance are replayable;
- the MDBX transaction ownership limitation is explicit;
- crash boundaries and `EXECUTION_UNKNOWN` behavior are documented;
- rejected alternatives are recorded here;
- no runtime, schema migration, broker adapter or live test is included.

After review, 5B may implement the shared transaction adapter in a separate
Draft PR. 5C remains a separate simulated recovery stage. No Batch 5 PR is to
be merged automatically from this architecture pass.
