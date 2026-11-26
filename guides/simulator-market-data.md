# Simulator Market Data (SIM-3A / SIM-3B)

Status: offline Draft. Public entry: `<optionx_cpp/simulator_market_data.hpp>`.
This composes the existing Router/provider API with SIM-2; it does not create
a `BaseTradingApi`, a production runtime, a scheduler or a second ledger.
The immutable fixture and `<optionx_cpp/simulator.hpp>` remain supported.

## Binding And Ownership

Use a shared-owned `SimulatorQuoteBook` and one scoped
`SimulatorMarketDataBinding` per simulator symbol. The host registers existing
`BaseMarketDataProvider` objects with `MarketDataRouter`; the binding verifies
the stable provider key against the actual provider instance. Symbols map
explicitly from provider to simulator. Several provider objects are supported,
but only one selected source per instrument is allowed; there is no implicit
fallback or cross-provider price comparison.

Example wiring with an already configured provider (no broker submission):

```cpp
#include <optionx_cpp/simulator_market_data.hpp>
namespace md = optionx::market_data;
using namespace optionx::platforms;

// provider: shared_ptr<BaseMarketDataProvider>, retained by the host.
md::MarketDataRouter router;
if (!router.register_provider(md::MarketDataProviderId{1}, *provider))
    throw std::runtime_error("provider already registered");
auto quotes = std::make_shared<SimulatorQuoteBook>();
std::int64_t owner_time_ms = 0; // Same epoch as SimulatorCore and source ticks.
{
    SimulatorMarketDataBinding binding(router, provider, quotes,
        {md::MarketDataProviderId{1}, "EURUSD.raw", "EURUSD",
         SimulatorPriceSide::REFERENCE, 5, 1000},
        [&] { return owner_time_ms; });
    // Pass quotes->selector() to the existing SimulatorCore constructor.
    // Dispatch provider completions/ticks/statuses on this same owner thread.
    // Advance owner_time_ms before observations, then advance the core.
} // Invalidate generation, then release the Router route.
router.shutdown(); // Provider remains alive through pending cleanup.
```

The executable fake-feed regression is
[simulator_market_data_test.cpp](../tests/simulator/simulator_market_data_test.cpp).
It drives actual Router subscribe/status/tick/unsubscribe paths without sleeps,
credentials, network or `BaseTradingPlatform`. The strict C++17 aggregate
consumer is [simulator_market_data_consumer_test.cpp](../tests/simulator/simulator_market_data_consumer_test.cpp).

Router stores non-owning provider references. A binding retains a strong
provider reference while alive, but the **host** must retain the provider and
Router through asynchronous subscribe/unsubscribe completion and Router
shutdown, even after destroying the binding. Provider callback slots belong
to Router; do not concurrently attach a Hub or replace them. Normal integration
uses existing Router owner-loop dispatch and platform `post_task()`, not direct
cross-thread callbacks into the book. A wrong-thread observation/select blocks
the book atomically without touching its owner-confined containers. Binding
creation, queries, shutdown and destruction are also owner-thread operations.

Callbacks capture weak subscriber/book ownership. The selector captures a weak
book, not the binding/provider. Binding teardown invalidates its generation
before resetting its scoped route; late old callbacks cannot feed a new
generation. Destroying the book makes the selector return no quote.

## Deterministic Quote Selection

`SimulatorQuoteBook` is the separate SIM-3B selector. Its first policy is strict
as-of, **zero waiting** and **no fallback**. Positive waiting windows or source
alternatives require a separately reviewed extension, not hidden defaults.

- `SUBSCRIBED` acknowledges a route; it does not imply `READY` or a price.
- A READY status does not synthesize a quote. A fresh delivered quote is needed.
- Disconnect/non-READY revokes cached prices from that observation time.
  Reconnect requires a new observation; earlier as-of queries retain their
  historical status/price ordering while the binding remains active.
- Every accepted quote records source timestamp and owner receive timestamp.
  Both must be nonnegative and at or before the execution event. A source
  timestamp later than owner receive time is rejected: there is no implicit
  clock-skew correction. Use one explicit epoch and aligned clock in the host.
- Provider `Tick::received_ms` is retained as metadata only. It cannot move an
  observation backwards in the simulator's decision clock.
- Choose the latest eligible source timestamp, then the last accepted distinct
  observation at that timestamp. Exact source-time/price duplicates within a
  READY epoch are suppressed. Older arrivals cannot regress a newer quote.
- Freshness is source age at the event, inclusive of the configured maximum.
  Both the binding and core age limits apply; the stricter wins. Defaults are
  zero age, requiring the exact timestamp.
- BID/ASK/MID/REFERENCE and fixed decimals (0..8) are explicit. MID requires a
  valid positive noncrossed bid/ask pair. Finite positive prices are converted
  with existing `utils::to_fixed_point` rounding, after int64 range checks.
  Tick batch precision must match the configuration; mismatch blocks the book.
- Quote evidence retains stable provider key, runtime provider instance,
  binding generation, chosen side, precision and observation ID. Once open,
  a trade can settle only against identical source/generation/side/precision.
  Rebinding cannot silently introduce an incompatible fallback.

The route requests live-only ticks, not history prefill/recovery. Existing
Router semantics normalize a provider's **live callback** to LIVE_SOURCE and
clear HISTORICAL/BACKFILL flags. The adapter cannot authenticate the provider's
origin beyond that contract. History returned via a history API is not requested
or routed into this binding. Any delivered historical/backfill flags still
present are ignored; receive-time eligibility is always independently checked.

## Late Arrival And Exposure

No eligible opening quote means FAILED_OPEN, releasing only pending holdings.
No eligible expiry quote means UNRESOLVED, preserving opened exposure, active
slot and absent P&L/settlement time. It is not a loss, refund or cancellation.

Example: expiry=60000, no eligible quote at expiry, later delivery at
receive=60001 with source=59999. That tick was not observed by expiry and cannot
settle this trade retroactively. The zero-wait policy does not enqueue another
settlement attempt. Confirmed opening/settlement facts remain immutable under
duplicates, late conflicting ticks, reconnect and repeated `process()`.

Core's only new dependency is a selector function returning an optional owned
`SimulatorTick` snapshot at `(symbol, event_ms)`. No money calculation, frozen
payout, event-calendar ordering, response delay, checked arithmetic or
reservation lifecycle was replaced. Core rejects future/malformed snapshots
and incompatible close provenance. Custom selectors must obey the same
deterministic as-of and immutable-result contract. Fixtures remain offline
scenario input, not a substitute for a real-time observed feed.

## Bounds And Performance

The book defaults to 4096 total accepted status/quote events and 64 lifetime
bindings; hard maxima are 1000000 and 256. Generations are never reused, and
teardown does not silently evict evidence or reset budgets. Exhausting the
event budget, regressing the owner receive clock, allocation failure during
observation, or invalid precision blocks selection; existing exposure remains.
Each incoming batch is limited to 4096 items. Strings are bounded to 64 bytes.
Input configuration and provider implementations remain trusted host inputs.

Space is O(E+B), quote lookup/duplicate checking O(E), binding lookup O(log B),
with finite E/B above. This is a bounded scenario adapter, not unbounded tick
storage; rolling retention and live runtime clocks remain future work.

`simulator_execution_benchmark` measures the complete fixture-backed
enqueue/admission/open/settle/notification path for 128/512/2048 trades. It
checks callback count/profit, final balance and zero reserve/exposure. Run in
Release; there is no latency gate or new active-symbol index. SIM-2's O(N)
active-symbol count is intentionally retained. This benchmark does not measure
network latency or claim Router-feed throughput.

Local Windows verification: MSVC 19.44 Debug and MinGW GCC 15.2 Release pass
20 SIM-3, 22 existing core and 13 policy tests, plus both strict C++17
consumers. Both compilers' Release fixture examples and execution benchmarks
pass. One machine/load-specific Release measurement, including callbacks:

| Trades | MSVC us/trade | MinGW us/trade |
| ---: | ---: | ---: |
| 128 | 2.96953 | 2.72812 |
| 512 | 9.39434 | 9.29492 |
| 2048 | 52.8606 | 54.3129 |

The growth supports keeping active-count cost visible, not claiming constant
throughput. Ubuntu/GCC and exact-head workflow results belong to PR checks and
must not be inferred from these local Windows runs. Both workflows explicitly
build/run new tests, aggregate consumer and full execution benchmark.

## Before SIM-4

These are proposals, not implemented persistence or DTO adapters:

1. Allocate a persisted versioned simulator run/epoch identity before the
   first durable command; resume the same epoch/counter after recovery, and
   create a new epoch for an independent run. Use a collision-checked namespace
   such as `sim/v1/<run>/<account>/<command>`. Keep virtual string identity in
   full; `TradeResult` and `TradeRecord` have string `option_hash`, signed int64
   `option_id`, and uint32 `trade_id` (not a `broker_trade_id` field). Propose a
   source-qualified virtual `option_hash`, absent numeric broker identity and
   separately allocated checked persistent `trade_id`. Do not truncate/hash a
   virtual string or reuse a real broker identity. Review namespace separation,
   DTO/storage ID widths and migration before implementing this projection.
2. Preserve currency, money decimals, stake, integer net P&L, policy revision
   and half-up rounding as authoritative facts. Convert to `TradeResult` doubles
   once at the boundary under the persisted normalization profile; profit means
   net P&L, not returned stake. Reject unrepresentable values or carry exact
   sidecar evidence rather than round-trip ledger money through `double`.
3. Review any positive bounded waiting, source fallback and skew policy with
   receive-time evidence, compatible price semantics and explicit finalization.

No real-time scheduler, JSON config adapter, durable simulator history, live
trading, SIM-4 facade or Trading System integration is authorized by SIM-3.

Related: [SIM-2](simulator-core.md), [policy](simulator-platform-contract.md),
[Router](market-data-router.md).
