# Trading System Reuse Audit

## Scope

This audit was performed before adding a new trading-system runtime. Batches 1
through 4 now provide the tested in-memory identity, policy, progression and
risk contracts. The goal remains to reuse existing OptionX contracts and
identify only the missing durable pieces. The result is an implementation map,
not a claim that a durable runtime already exists. See [Batch 5A](trading-system-batch5a.md)
for the architecture gate.

## Inventory

| Existing component | Decision | Findings and boundary |
| --- | --- | --- |
| `data/trading/TradeSignal.hpp` | EXTEND | Already carries source identity, symbol, trade parameters, MM type/step/group and decision/MM parameter objects. Keep it immutable as the source event; do not overload it with branch state. |
| `data/trading/TradeRequest.hpp` | REUSE + EXTEND | Correct broker-facing DTO and callback snapshot. It already carries `trade_id`, `signal_id`, account and execution fields. Add intent/group correlation only when the contract is approved. |
| `data/trading/TradeResult.hpp` | REUSE + EXTEND | Contains broker identities, prices, P&L, balances, timestamps and `TradeState`. It does not by itself model an unresolved submission or idempotent settlement event. |
| `data/trading/TradeRecord.hpp` | REUSE | Durable trade snapshot, broker identity matching and conversion from signal/request/result already exist. It is not a journal of every intent transition. |
| `data/trading/SignalRecord.hpp` | REUSE + EXTEND | Durable signal-to-trade linkage exists and can support source recovery. It has no general series/intent journal or revision protocol. |
| `data/trading/enums.hpp` and MM parameter interfaces | REUSE | `MmSystemType` already includes fixed, percent, Kelly, martingale, anti-martingale, Labouchere and related modes. Do not create parallel money-management enums. Pure policy adapters can be added around these types. |
| `platforms/common/BaseTradingApi.hpp` | REUSE | Existing callbacks cover place, trade ID, result and history operations. It is the broker boundary, not the trading-system policy boundary. |
| `platforms/common/BaseTradingPlatform.hpp` | REUSE | Owns platform lifecycle, owner-loop dispatch and public callbacks. Trading-system modules should schedule through existing task/lifecycle contracts. |
| `components/.../AccountInfoProvider.hpp` | EXTEND | Provides account information by account, symbol, option and order. A decision layer still needs an immutable, freshness-aware account snapshot. |
| `market_data/` and Router | REUSE | Provider selection, subscription ownership, history continuity and source metadata already exist. The trading system should consume snapshots/batches and not reimplement routing. |
| `utils/tasks`, `utils/pubsub`, `lifecycle.hpp` | REUSE | TaskManager, EventBus/EventMediator and LifecycleStack already define scheduling and staged shutdown. No new scheduler or bus is needed. |
| `storages/SignalRecordDB.hpp` | REUSE + ADAPTER | Async/synchronous queueing, signal ID, trade ID and timestamp lookup are available. It remains a source adapter, not the operation journal. |
| `storages/TradeRecordDB.hpp` | REUSE + ADAPTER | Persistent linear trade IDs, UID/broker identity indices, upsert and recovery queries exist. It stores physical snapshots, not all logical intent/group transitions. |
| MDBX dependency | REUSE WITH SHARED OWNERSHIP | Existing storage is the only persistence engine. A Batch 5 Unit of Work must share one environment/transaction context; current standalone DB facades do not yet provide that contract. |
| ADELIA/MegaConnector concepts | NOT_NEEDED | They belong to the host/adapter layer and must not become OptionX dependencies. |

## Gaps That Are Real

1. A durable `intent_id`, `series_id`, `execution_group_id` and revision model is
   missing from the broker-facing DTOs.
2. There is no common typed lifecycle for rejected, cancelled, admitted,
   submitted, unknown, executed and settled logical intents.
3. There is no atomic risk admission/reservation owner shared by concurrent
   strategies.
4. `TradeRecordDB` and `SignalRecordDB` do not constitute an append-only
   lifecycle journal or an idempotent settlement ledger, and their current
   standalone writes cannot be made atomic by an external wrapper.
5. Account data is available through providers, but a decision snapshot needs
   freshness/availability metadata and a coherent capture boundary.
6. There is no generic split-group aggregator or explicit execution-cardinality
   contract.
7. Account identity, financial risk scope, durable physical roster and valuation
   provenance still need one recovery-owned binding.

## Existing Recovery Strengths

The current code already supports useful parts of recovery:

- a persistent `trade_id` can be allocated before broker submission;
- `TradeRequest -> TradeResult -> TradeRecord` can preserve that identity;
- trade records can be found by persistent ID, user UID and broker identity;
- signal records can link a signal to produced trade IDs;
- queued storage work has explicit `process`, `flush` and `shutdown` behavior.

The dangerous gap is the crash after submission and before broker ID/result.
The correct state is `UNKNOWN`, not a fresh retry. A future adapter must keep
that record visible until broker reconciliation proves the outcome.

## Reuse Rules

- Keep policy code independent of platform-specific HTTP and broker response
  formats.
- Keep one owner for mutable series and reservation state.
- Use immutable snapshots for cross-module reads.
- Carry existing `trade_id` through execution; do not make `unique_id` a
  replacement for the persistent database identity.
- Use existing lifecycle/task/pub-sub facilities.
- Add persistence only after the state machine and idempotency keys are
  specified.
- Do not claim cross-table atomicity until repository methods accept a shared
  transaction context and tests prove rollback/commit behavior.
- Keep native money facts and valuation provenance; do not recompute historical
  decisions from current exchange rates.
