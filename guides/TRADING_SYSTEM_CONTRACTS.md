# Trading System Contracts

## Status

These are proposed C++17 contracts for review. They are intentionally small
and header-only in spirit. Names and field types may change in the first
implementation PR; the invariants are the important part.

The original sketches below are not the runtime API. See
[Batch 2](trading-system-batch2.md) for implemented decision/sizing contracts and
[Batch 3](trading-system-batch3.md) for fixed-point outcomes, series transitions
and settlement idempotency. The Batch 3 owner is in-memory only, not the durable
state/recovery adapter proposed here. See [Batch 5A](trading-system-batch5a.md)
for the approved identity, valuation, transaction ownership and recovery
boundaries before implementing durable storage.

## Stable IDs and States

```cpp
using IntentId = std::uint64_t;
using SeriesId = std::uint64_t;
using ExecutionGroupId = std::uint64_t;
using SettlementId = std::uint64_t;

enum class IntentStatus {
    PROPOSED,
    REJECTED,
    ADMITTED,
    SUBMITTING,
    EXECUTION_UNKNOWN,
    EXECUTED,
    SETTLED,
    CANCELLED
};

enum class ExecutionCardinality {
    ZERO_OR_ONE,
    EXACTLY_ONE,
    MULTIPLE,
    SPLIT_GROUP
};
```

State transitions must be validated against the current revision and an
idempotency key. A repeated terminal event is a no-op. An out-of-date revision
cannot silently overwrite a newer series state.

## Signal, Intent and Execution

```cpp
struct TradeIntent {
    IntentId intent_id = 0;
    SignalId origin_signal_id = 0;
    SeriesId series_id = 0;
    std::int32_t series_step = -1;
    ExecutionGroupId execution_group_id = 0;
    ExecutionCardinality cardinality = ExecutionCardinality::ZERO_OR_ONE;
    IntentStatus status = IntentStatus::PROPOSED;
    TradeRequest request;
    std::uint64_t state_revision = 0;
};

struct TradeOutcome {
    IntentId intent_id = 0;
    ExecutionGroupId execution_group_id = 0;
    SettlementId settlement_id = 0;
    TradeState state = TradeState::UNKNOWN;
    double net_profit = 0.0;
    bool final = false;
};
```

This is a contract sketch, not a request to add these exact structs before the
first vertical slice is tested. `TradeSignal` remains immutable. A physical
`TradeRequest` still flows through the existing platform API and keeps the
existing persistent `trade_id`.

## Decision Context

```cpp
enum class DataAvailability { SUPPORTED, UNAVAILABLE, STALE, UNKNOWN };

struct AccountContext {
    DataAvailability availability = DataAvailability::UNKNOWN;
    std::uint64_t captured_at_ms = 0;
    std::uint64_t revision = 0;
    // Immutable account/balance/payout values.
};

struct TradingDecisionContext {
    const TradeSignal* signal = nullptr;
    const AccountContext* account = nullptr;
    const MarketSnapshot* market = nullptr;
    const CalendarSnapshot* calendar = nullptr;
    const PortfolioSnapshot* portfolio = nullptr;
    const ExecutionSnapshot* execution = nullptr;
};
```

The final implementation may replace raw pointers with owned immutable
snapshot handles. A pointer in this sketch is never valid beyond the lifetime
of the context call. Asynchronous modules must retain a shared snapshot, not a
borrowed pointer or a `span` over mutable storage.

## Small Module Interfaces

The intended capabilities are separate:

```cpp
struct IntentDecision {
    bool accepted = false;
    std::string reason;
    std::optional<TradeIntent> intent;
};

class IIntentFilter {
public:
    virtual ~IIntentFilter() = default;
    virtual IntentDecision evaluate(const TradingDecisionContext&) = 0;
};

class IPositionSizer {
public:
    virtual ~IPositionSizer() = default;
    virtual std::optional<double> size(const TradingDecisionContext&) = 0;
};

class ISeriesProgression {
public:
    virtual ~ISeriesProgression() = default;
    virtual void apply_outcome(const TradeOutcome&, std::uint64_t expected_revision) = 0;
};
```

Active/event-driven modules may consume ticks, bars, calendar events or
settlement events and emit intents. They do not need a private thread by
default. The existing task manager and lifecycle stack remain the execution
mechanism.

## Risk Admission

Risk admission is the final gate immediately before submission:

The concrete Batch 4 in-memory owner and its typed lifecycle are documented in
[Trading System Batch 4](trading-system-batch4.md). The interface below is the
architectural boundary; it does not provide freshness, idempotency, or
concurrency semantics by itself.

```cpp
struct RiskReservation {
    std::uint64_t reservation_id = 0;
    bool valid = false;
};

class IRiskAdmission {
public:
    virtual ~IRiskAdmission() = default;
    virtual std::optional<RiskReservation> reserve(
        const TradeIntent&, const TradingDecisionContext&) = 0;
    virtual bool commit(const RiskReservation&, const TradeRequest&) = 0;
    virtual void release(const RiskReservation&) = 0;
    virtual void mark_unknown(const RiskReservation&) = 0;
};
```

Two strategies must not both reserve against the same stale balance snapshot.
`mark_unknown` retains the reservation until reconciliation; it is not an
alias for `release`.

## Execution Groups

For `SPLIT_GROUP`, all physical members share an `execution_group_id`. The
group is final only when every required member is settled or the group policy
explicitly records a terminal failure. Aggregate outcome is defined by policy,
normally total confirmed net P&L. One unknown member keeps the group pending.

## Persistence and Recovery

The future persistence adapter must support:

- create/update intent with monotonic revision;
- create/update series state;
- create execution group and physical trade links;
- persist the immutable physical roster, account IDs, risk scopes and native
  monetary facts before the first possible broker side effect;
- persist versioned state for stateful money-management modules;
- append or idempotently apply settlement identity;
- find unresolved `SUBMITTING`/`EXECUTION_UNKNOWN` operations;
- reconcile broker identity before retry;
- restore state before accepting new series progression.

`SignalRecordDB` and `TradeRecordDB` remain adapters and sources of existing
signal/trade facts. They should not be silently treated as a complete journal.
Their current public writes own their connection/transaction boundaries. A
future Unit of Work must therefore use a shared MDBX environment and explicit
transaction-aware repository operations, or document a compensating recovery
protocol. Wrapping the current standalone methods is not atomicity. The first
implementation can use dedicated named tables or a carefully scoped extension,
but it must not hide logical intent state in free-form comments.

The local transaction ends before the external broker call. It can make a
command ready and record submission-started, but it cannot atomically commit a
broker side effect. A timeout after a possible side effect remains
`EXECUTION_UNKNOWN`; it retains risk and cannot trigger an automatic resend.

For multi-currency outcomes, preserve native values and persist the selected
valuation fact whenever conversion affects a decision or aggregate. A global
base currency is reporting policy, not a requirement that every account or
stake use that currency.

## Cancellation Rules

- Before submission: transition to `CANCELLED` or `REJECTED`, release the
  reservation, and emit one terminal event.
- During submission: record the operation as `SUBMITTING`; cancellation cannot
  claim that the broker did not receive it.
- After transport ambiguity: transition to `EXECUTION_UNKNOWN`, retain risk,
  reconcile by broker identity/history, and only then settle or cancel.
