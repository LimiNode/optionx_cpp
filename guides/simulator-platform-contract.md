# SimulatorPlatform contract (SIM-0)

Status: design contract, not an implemented API. `SimulatorPlatform` is an
independent paper-trading platform for binary/fixed-outcome contracts. It may
consume real or replayed quotes, but it never delegates trades to another
platform. Integration with Trading System is a later, separate test target.

## Existing boundaries and compatibility

- Use `PlatformType::SIMULATOR`; do not add another platform kind.
- The public facade will be `optionx::platforms::SimulatorPlatform`, through
  `BaseTradingPlatform` and `BaseTradingApi`. The latter already defines
  `place_trade`, result/history queries, and callback ownership. The former
  supplies `run(false)`, `process`, `shutdown`, `post_task`, connection events,
  account information, and trading-condition callbacks.
- `BaseTradingPlatform::configure(unique_ptr<IEndpointConfig>)` currently
  requires an `IAuthData` cast. A simulated account has no credentials. SIM-4
  must provide a narrow compatible configuration route (for example, a
  simulator-specific overload), without fake login/password fields or a
  general change to existing broker configuration semantics.
- `BaseMarketDataProvider` supplies ticks/status; `MarketDataRouter` owns
  provider subscriptions. The simulator consumes subscribed observations. It
  does not introduce `setCurrentPrice()` or own mutable provider internals.
- `SimulatedExecutionAdapter` belongs to Trading System recovery tests and is
  not this public platform. `TradeRecord` and existing storage DTOs must be
  evaluated before adding a persistence format. No second financial ledger
  should be added without a documented missing field or lifecycle contract.
- `TradeStateManager` treats BUY as an upward bet and SELL as a downward bet.
  The `OrderType` CALL/PUT string converter currently maps CALL to SELL and
  PUT to BUY. SIM-1 must test this discrepancy and use enum semantics for
  settlement; do not silently change the global converter in SIM work.

## Configuration and ownership

`SimulatorPlatformConfig` is a value configuration with schema version,
account, execution, policy, and quote-source bindings. The names below are
provisional. A serialized configuration stores provider *keys*, never C++
pointers or credentials. At runtime the host binds those keys to
`shared_ptr<BaseMarketDataProvider>` instances and the Router. Binding fails
closed for a required but absent source. Providers remain externally owned;
the platform releases subscriptions before its callback targets are destroyed.

The account config contains a stable simulated account ID, `AccountType`,
`CurrencyType`, initial balance in native monetary units, and the policy
version. Currency describes the balance/stake, not the traded symbol: EURUSD
is an instrument; USD is an account currency. Balance is never fabricated
from a missing quote. Both manual and real-time clocks are supported. Manual
time advances by explicit millisecond steps and requires no sleeping or worker
thread. Real-time mode uses the platform lifecycle. A single owner loop
serializes admission, quote selection, open/settle events, account projections,
and callbacks. Equal-time events have a stable sequence number.

The execution config defines open delay, close-response delay, tie/refund
policy, price precision, stake precision, and rounding policy. Defaults for
both delays are zero. The simulation uses no actual broker request.
`place_trade() == true` only acknowledges local queue admission. Processing
that command later either fails before reservation or atomically reserves the
stake and active slot for the pending open. A delayed open must therefore
count against active-trade caps. At the actual open time, revalidate any
time-dependent condition; if opening cannot proceed, release both reservations
exactly once and emit failed-open evidence. A queued command not yet reserved
has nothing to release.

## Rule model

A policy contains defaults, per-symbol defaults, and ordered rules. A rule
has a selector and independent optional field overrides, not a precomputed
row for every combination. Selectors may constrain symbol, BUY/SELL, UTC
seconds-of-day window, and expiry interval/threshold. Overrides may set
availability, payout, minimum/maximum stake, global/per-symbol active limits,
and other explicitly supported trading conditions. An absent field inherits
the current value; it does not clear a value set by a less specific rule.
`max_active_global` applies to the whole virtual account and may depend only
on UTC time, never on symbol, direction, or expiry. It must not be set in
per-symbol defaults. `max_active_symbol` applies to one symbol and may vary by
time and symbol, but not direction or expiry. Both limits apply together; a
future direction-specific cap would need its own counter and field.

Resolution for each requested field:

1. Begin with policy defaults, then per-symbol defaults.
2. Match all rules against the **execution/open** instant and requested
   direction and duration, using UTC unless a future schema explicitly adds
   a timezone. Windows are half-open `[start,end)` in seconds of day. A
   wrap-midnight window matches `t >= start || t < end`; equal endpoints are
   rejected, not interpreted as a full day.
3. Apply matching rules by ascending `(priority, specificity, stable_rule_id)`.
   Higher priority wins last. Specificity is the count of constrained
   selector dimensions; it only breaks ties within one priority. A field not
   supplied by a matching rule is unchanged.
4. Reject configuration with duplicate stable rule IDs, invalid bounds,
   contradictory limits, or two overlapping rules that could write the same
   field at the same priority and specificity. An explicit priority or a
   non-overlapping selector resolves such conflict. Do not rely on JSON order
   or `unordered_map` iteration.

Payout is the **net profit fraction on stake**: `0.75` means a winning bet
returns stake plus 75% profit. Freeze payout and effective rule/profile ID at
open. Later policy edits affect only later opens. Stake and active limits are
checked and reserved as one owner-loop transition after queue admission.
Active-trade limits count reserved pending opens and opened but not yet
terminal operations;
global and per-symbol caps both apply. They are concurrency limits, not
rate limits over a wall-clock interval. No interval rate limiter is implied.

SIM-1's pure resolver stores stake thresholds as native-currency minor units
and payout as parts per million (`750000` means `0.75`). The decimal strings
in the JSON example require explicit conversion by a future configuration
adapter; they are not binary floating-point inputs to the resolver.

Before trading, SIM-1 validates every reachable effective combination of
symbol, BUY/SELL, UTC window, and expiry. It partitions time and duration at
rule boundaries and excluded expiries, then checks one representative per
half-open cell; selector membership is constant inside a cell. Validation is
`O(R^2 + S * T * E * R)`, where `R` is rules, `S` symbols, and `T/E` are the
numbers of distinct time/expiry intervals. To bound hostile configurations,
the implementation rejects more than 128 rules, 128 symbols, 256 excluded
expiries, or 200000 reachable cells. `resolve()` remains fail-closed if a
mutable policy object becomes invalid after prior validation.

An expiry policy accepts a bounded duration interval in seconds, excludes
specified durations, and may override fields at duration thresholds. Duration
resolution is one-second precision. A trade with unsupported expiry fails
with the existing `TradeErrorCode::INVALID_DURATION` or
`INVALID_EXPIRY_TIME`, as appropriate. Missing symbol/unsupported account,
currency, option, stake, payout, balance, connection, and count limit use the
existing matching `TradeErrorCode`; no new error enum is required by SIM-0.

### Example resolution table

Assume defaults: payout 0.70, stake 1..100 USD, active cap 10. EURUSD sets
payout 0.75 and active cap 3; GBPUSD sets payout 0.80. Rule `close-window`
matches EURUSD BUY at `[64500,65100)` UTC, sets payout 0.50 and max stake
20 USD. A separate EURUSD duration rule excludes 120 seconds and permits
60..30000 seconds. Unmentioned fields inherit.

| Request at open (UTC) | Effective payout | Max stake | Active caps | Result |
| --- | ---: | ---: | --- | --- |
| EURUSD BUY, 17:54:59, 60 s | 0.75 | 100 USD | 10 global, 3 symbol | eligible |
| EURUSD BUY, 17:55:00, 60 s | 0.50 | 20 USD | 10 global, 3 symbol | eligible |
| EURUSD SELL, 18:00:00, 60 s | 0.75 | 100 USD | 10 global, 3 symbol | eligible |
| EURUSD BUY, 18:04:59, 300 s | 0.50 | 20 USD | 10 global, 3 symbol | eligible |
| EURUSD BUY, 18:05:00, 300 s | 0.75 | 100 USD | 10 global, 3 symbol | eligible |
| EURUSD BUY, 18:00:00, 120 s | n/a | n/a | n/a | invalid duration |
| GBPUSD BUY, 18:00:00, 60 s | 0.80 | 100 USD | 10 global | eligible |

The example intentionally distinguishes a symbol from the account currency;
all stakes and balances above are USD. Multiple rules may each override only
one field. A payout-only rule never erases an inherited count or stake cap.

### Proposed JSON shape

This is a versioned design sample, not a parser currently shipped by OptionX.
SIM-1 should validate equivalent in-memory DTOs first; JSON support may be a
separate adapter rather than a dependency of the core policy engine.

```json
{
  "schema_version": 1,
  "account": {"id": "paper-1", "type": "DEMO", "currency": "USD", "initial_balance": "1000.00"},
  "clock": "MANUAL",
  "execution": {"open_delay_ms": 150, "close_response_delay_ms": 0},
  "sources": [{"key": "offline-eurusd", "symbols": ["EURUSD"]}, {"key": "offline-gbpusd", "symbols": ["GBPUSD"]}],
  "policy": {
    "version": 1,
    "defaults": {"min_stake": "1.00", "max_stake": "100.00", "max_active": 10},
    "symbols": {"EURUSD": {"payout": "0.75", "max_active": 3}, "GBPUSD": {"payout": "0.80"}},
    "expiry": {"min_seconds": 60, "max_seconds": 30000, "exclude_seconds": [120]},
    "rules": [{"id": "close-window", "priority": 10,
      "selector": {"symbol": "EURUSD", "direction": "BUY", "utc_window": [64500, 65100]},
      "overrides": {"payout": "0.50", "max_stake": "20.00"}}]
  }
}
```

## Quote and execution semantics

A source binding maps a symbol to one or more provider keys with explicit
priority/fallback policy. The platform subscribes through Router, waits for
stream READY, and stores owned tick observations with source identity and
timestamp. Subscription acceptance alone is not READY. A stale, missing,
out-of-order, duplicate, or post-unsubscribe tick must not become a new
executable price. Fallback is allowed only to an explicitly configured ready
source with a quote within freshness bounds. Selection at open and expiry
uses the latest eligible observation at or before the respective event time;
never use a later quote (no look-ahead). A missing eligible quote leaves the
operation unresolved or rejects opening, as appropriate; it never invents a
price. Define bounded waiting and terminal handling before SIM-3.

The lifecycle has four distinct transitions: local queue admission,
pre-open stake/slot reservation, actual open after any delay, and terminal
settlement. Allocate a stable virtual trade ID at reservation. At actual open,
freeze requested parameters, policy version, matching rule IDs, payout,
selected opening quote/source/time, and account currency. Shutdown, cancel,
failed quote selection, or any rejected open releases a reservation exactly
once; a duplicate callback cannot release it twice. At expiry,
select an eligible closing quote. BUY wins on price rise, SELL on price fall;
equal price follows the configured draw/refund rule. A win credits stake plus
rounded net profit; loss consumes the stake; refund returns the stake. Close
response delay affects notification, not the economic close timestamp. A
duplicate ID or duplicate terminal observation cannot settle twice. Result
query and history use the same immutable virtual identity as callbacks.

Monetary arithmetic uses a fixed precision/rounding rule specified by policy;
binary floating point is not a money ledger. Replay data must retain policy
version, effective rule/profile IDs, stake/expiry, open/close quote identities
and timestamps, frozen payout, virtual ID, and final native-currency net P&L.
Policy changes do not retroactively recalculate accepted trades.

## Public usage sketch and staged verification

The following is illustrative C++17 API intent, **not yet compilable**:

```cpp
SimulatorPlatformConfig config;
config.account("paper-1", AccountType::DEMO, CurrencyType::USD, "1000.00");
config.policy().symbol("EURUSD").payout("0.75").max_active(3);
config.policy().symbol("GBPUSD").payout("0.80");
config.policy().rule("close-window", 10)
    .symbol("EURUSD").direction(OrderType::BUY)
    .utc_window(17 * 3600 + 55 * 60, 18 * 3600 + 5 * 60)
    .payout("0.50").max_stake("20.00");
config.policy().expiry(60, 30000).exclude(120);
config.execution().open_delay_ms(150).close_response_delay_ms(0);
config.source("EURUSD", "offline-eurusd");

auto platform = std::make_shared<SimulatorPlatform>();
platform->configure(std::move(config));
platform->bind_source("offline-eurusd", offline_provider, router);
platform->run(false);
platform->process();
```

- SIM-1: pure policy DTOs, validation, deterministic per-field resolution,
  expiry/window boundaries, overlap errors, and the BUY/SELL converter audit.
- SIM-2: account ledger, trade lifecycle, manual clock, admission atomicity,
  payout freezing, query/history, deterministic event ordering.
- SIM-3: Router/provider binding, ready/stale/fallback/unsubscribe behavior,
  timestamped quote selection without look-ahead.
- SIM-4: public facade, callback/lifecycle compatibility, C++17 **compiling**
  consumer example, and offline integration tests. No live/demo broker call.

Paper-trading results model specified latency, payouts, freshness, and limits.
They are not evidence of the execution quality of any real broker.
