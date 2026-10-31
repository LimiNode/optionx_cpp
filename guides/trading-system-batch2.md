# Trading System Batch 2

Batch 2 adds deterministic, broker-independent policies to the
`optionx::trading_system` aggregate. It does not submit trades, read a live
account, persist state, or own a scheduler.

## Decision Context

`TradingDecisionContext` is a value snapshot. It copies the symbol and source
identity needed by filters, the explicit evaluation timestamp and
`TradeTimeZone`, and a `BalanceSnapshot` for sizing. Policy calls do not read
the system clock. Build the context once and pass it as a `const` value.

`TradeTimeZone` delegates named-zone and DST behavior to `time-shield`. Fixed
offset mode is available when the caller has an explicit broker offset. The
existing `TradeTimeZone` fallback behavior applies: an unsupported named-zone
conversion falls back to UTC/identity semantics.

## Filter Decisions

`SymbolFilter`, `SourceFilter` and `TradingWindowFilter` return a
`PolicyDecision` with one of:

- `ALLOW` when the input matches;
- `REJECT` when a configured policy denies it;
- `UNAVAILABLE` when required context is missing or a symbol/time configuration
  cannot be evaluated.

`SymbolFilter` uses generic separator/case normalization by default, so
`EUR/USD` and `eurusd` compare as `EURUSD`. A caller may inject an existing
broker normalizer, such as the Intrade Bar alias helper, and provide a known
symbol set. Unknown symbols in that explicit set are `UNAVAILABLE`, while an
explicitly disallowed known symbol is `REJECT`.

`TradingTimeWindow` uses half-open intervals `[start, end)`. An interval may
cross midnight, for example `{22 * 3600, 2 * 3600}`. `TradingWindowFilter`
evaluates the local time and weekday after applying the timezone from the
context. An empty allowlist means that no allow-window restriction is applied;
denied windows still take precedence over an allowed match.

## Fixed-Point Sizing

`MoneyAmount` stores integer minor units plus an explicit decimal precision.
`MoneyAmount::from_decimal()` rejects significant digits beyond the declared
precision instead of silently rounding them.

`FixedStake` returns its configured positive amount. `PercentOfBalance` accepts
a percentage in the range `(0, 100]`; the `double` constructor explicitly
converts it to basis points, while `from_basis_points()` is available when the
caller already has an exact value. The calculated amount uses integer
arithmetic. `MoneyRounding` explicitly selects `DOWN`, `HALF_UP`, or `UP` for
the percentage and output-precision boundaries.

The output precision may be equal to or finer than the balance precision. A
coarser output precision is rejected as `PRECISION_MISMATCH`; the policy does
not silently discard balance digits before calculating the percentage.

Stale, unavailable, unknown, or missing balances produce an `UNAVAILABLE`
sizing result. Zero and negative balances, invalid amounts, currency
mismatches, and zero calculated stakes are rejected. The policy only proposes
an amount; broker limits, payout, reservations, and final admission remain
later responsibilities.

## Sequential State Guards

`TradeIntent::advance_revision()` and `TradeSeries::advance_revision()` remain
local sequential guards for one state owner. They are not thread-safe CAS
operations. Their callers must serialize DTO access; persistent/runtime state
coordination belongs to a single owner or a storage-layer compare-and-swap.
Revision overflow is rejected rather than wrapped to zero.

## Example

```cpp
auto context = TradingDecisionContext::from_intent(
    intent, evaluation_time_ms, optionx::TradeTimeZone::utc());
context.account.availability = DataAvailability::SUPPORTED;
context.account.currency = optionx::CurrencyType::USD;
context.account.balance = MoneyAmount::from_decimal(
    "100.00", optionx::CurrencyType::USD, 2);

const SymbolFilter symbols({"EUR/USD"});
const SourceFilter source({7}, {}, {"alpha"});
const TradingWindowFilter window({{9 * 3600, 10 * 3600}});
const FixedStake stake(*MoneyAmount::from_decimal(
    "5.00", optionx::CurrencyType::USD, 2));

const auto symbol_result = symbols.evaluate(context);
const auto source_result = source.evaluate(context);
const auto window_result = window.evaluate(context);
const auto size_result = stake.evaluate(context);
```

The example is intentionally a pure decision chain. It does not imply that a
successful policy result is permission to submit a broker request.
