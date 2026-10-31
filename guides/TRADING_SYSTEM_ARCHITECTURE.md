# OptionX Trading System Architecture

## Status

This is the foundation decision for a future `optionx::trading_system` domain.
It is intentionally independent of ADELIA, MegaConnector, GUI code and broker
implementations. This document defines boundaries and invariants; it does not
authorize a production runtime implementation by itself.

## Responsibility Boundary

OptionX has two related but separate responsibilities:

| Area | Responsibility |
| --- | --- |
| Broker and data APIs | Authentication, account data, market data, trade requests, broker results and persistence primitives. |
| Trading system | Signals, decision context, filters, sizing, risk admission, intent lifecycle, series progression and recovery contracts. |

MegaConnector composes these primitives into an application. ADELIA may expose
the same primitives as graph nodes and typed edges. Neither dependency is
visible from OptionX headers. A normal C++ bot must be able to use the trading
system without either project.

The intended dependency direction is:

```text
broker/data APIs -> trading-system contracts -> host composition
                                      ^
                         ADELIA/MegaConnector adapter
```

The arrow is a boundary, not a required call sequence. A signal can originate
from a bridge, a market-data event, a schedule, or an active module.

## Core Identity Model

The system keeps these identities distinct:

| Identity | Meaning | Lifetime |
| --- | --- | --- |
| `signal_id` | Immutable source event and correlation identity. | Persistent source record. |
| `intent_id` | One candidate logical operation after policy decisions. | Persistent until terminal outcome. |
| `series_id` | Stateful progression such as martingale or anti-martingale. | Persistent across operations and restarts. |
| `execution_group_id` | One logical operation represented by one or more physical broker trades. | Persistent until all members settle or become unknown. |
| `trade_id` | OptionX persistent identity for one physical trade record. | Existing `TradeRecordDB` identity. |
| `broker_trade_id` | Broker-side numeric or string identity. | Available only after broker acceptance, or unresolved. |

`TradeSignal` remains the source event. It must not be mutated to represent a
later branch. A `TradeIntent` is the mutable, policy-approved candidate. A
`TradeRequest` is the broker-facing physical request. A `TradeResult` describes
the observed execution/outcome, and a `TradeRecord` is its durable snapshot.

## Lifecycle Invariants

The proposed logical intent lifecycle is:

```text
PROPOSED -> REJECTED
         -> ADMITTED -> SUBMITTING -> EXECUTED -> SETTLED
                                  \-> UNKNOWN
         -> CANCELLED
```

`UNKNOWN` is not `CANCELLED`. It covers a request that may have reached the
broker but whose acceptance is not proven. It requires reconciliation before a
retry or release of reserved risk. Terminal delivery is idempotent by
`intent_id`, `execution_group_id`, `trade_id` and a settlement/event identity;
duplicate callbacks must not advance a series twice.

The state source of truth is durable operation state. Event delivery is a
notification mechanism and cannot be required to reach every upstream module
for correctness. RAII manages subscriptions and runtime resources; it does
not replace persistence or recovery after a process crash.

## Branching and Execution Cardinality

Branching is allowed when the intent declares its cardinality:

| Policy | Meaning |
| --- | --- |
| `ZERO_OR_ONE` | One physical execution or an explicit rejection. |
| `EXACTLY_ONE` | Exactly one eligible alternative must be selected. |
| `MULTIPLE` | Several independent physical executions are allowed. |
| `SPLIT_GROUP` | Several physical trades form one logical execution group. |

Series progression consumes the aggregate logical outcome, not an arbitrary
branch callback. A split group remains non-terminal while any member is
unknown. For a settled group, aggregation is explicit, normally by total net
profit rather than by the count of wins.

## Contexts

Trading decisions read a single immutable decision snapshot composed of small
contexts:

- signal context;
- account context, including freshness and availability;
- market context, including ticks/bars and their provenance;
- calendar/news context;
- portfolio context;
- execution context, including reservations and active intents.

These contexts are not one universal datastore. Account values must distinguish
`SUPPORTED`, `UNAVAILABLE`, `STALE` and `UNKNOWN`; a zero default is not a safe
substitute. News is a calendar context, not a market tick. Historical/replay
consumers must not observe facts before their publication time.

For market data, prefer an immutable provider-owned snapshot held through a
shared lifetime when asynchronous modules need to retain it. A borrowed view is
valid only while that snapshot is alive. Do not expose a raw `span` whose owner
can disappear after a callback. Existing `TickSequence` and routed batches
remain the source of truth until a dedicated snapshot contract is justified.

## Module Model

The first design uses small capability interfaces rather than one mandatory
`ITradingModule` with every possible method:

- signal/event consumers;
- stateless filters;
- position sizing policies;
- series progression policies;
- risk admission;
- execution adapters;
- settlement/recovery participants.

Modules normally run on the existing owner loop and task system. A separate
worker is reserved for genuinely long or latency-sensitive computation. The
final risk admission and reservation still happen through the common owner of
account/risk state.

## ADELIA and MegaConnector Adapter Boundary

The host may map a graph node to one or more small OptionX capabilities and map
typed graph edges to `TradeIntent` or lifecycle events. The host owns graph
topology, configuration, UI and node scheduling. OptionX owns trading-domain
contracts and broker adapters. No OptionX type should mention a graph node,
graph editor, ADELIA, ImNodeFlow or MegaConnector.

The adapter must preserve identities, cardinality, cancellation semantics and
unknown execution state. It must not convert a rejected intent into a loss or
turn a missing broker response into a cancellation.
