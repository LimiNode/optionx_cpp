# Trading System Batch 6: Synthetic Vertical Slice

Batch 6 is a bounded offline composition example. It proves that the existing
filter, sizing, risk, durable-storage, settlement and recovery contracts can be
used by one ordinary C++17 caller. It is not a production `TradingSystem`
facade, scheduler, graph engine or broker adapter.

## Pipeline

`optionx_examples::SyntheticHost` in `examples/trading_system/SyntheticHost.hpp`
composes the following stages:

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
host never resends a possibly submitted operation. Physical settlement and
logical group completion remain separate durable transitions. Group completion
updates the series, MM payload and every member reservation in the existing
atomic Batch 5B path.

The example is single-threaded. `RiskAdmission` owners are authoritative for
one risk scope and are recreated only after durable facts are read. Shared-scope
members are aggregated into one in-memory reservation token for admission while
their physical reservation records remain separate in the roster and storage.

## Scenarios and guarantees

`tests/trading_system_batch6_test.cpp` covers:

- accepted BUY, WIN, durable completion and exactly-once progression;
- symbol and time-window rejection without durable execution side effects;
- insufficient, unavailable and stale risk facts;
- multiple-scope admission rollback before preparation;
- one capped loss step with checked multiplier/overflow and durable restore;
- crash/reopen after preparation, possible broker side effect, physical
  settlement and logical commit;
- multi-account groups, shared scope aggregation, late member completion and
  immutable physical account identities;
- durable policy/account-binding changes failing closed.

The test uses deterministic synthetic time and a real MDBX environment. No live
credentials, network broker or wall-clock sleep is involved.

## Loss-step policy

`LossStepStake` is deliberately minimal: it evaluates a fixed base stake and,
when the persisted `after_loss` flag is true, applies one configured integer
multiplier. It rejects multipliers not greater than one and checked arithmetic
overflow. The host persists the flag and validates the complete policy
configuration on restart. This is not a complete Martingale/Kelly/Labouchere
framework.

## Scope limits

The example does not provide live execution, a generic module graph, a
production scheduler, broker-specific reconciliation, FX conversion, GUI,
MegaConnector/ADELIA integration or a new storage engine. Those remain later
integration boundaries. Batch 7 must connect such hosts to platform adapters
only after this offline composition remains accepted.
