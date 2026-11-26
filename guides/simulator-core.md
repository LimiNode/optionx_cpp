# Offline Simulator Core (SIM-2)

Status: bounded, autonomous **offline** scenario core, not `SimulatorPlatform`
and not a `BaseTradingApi` implementation. Public entry:
`#include <optionx_cpp/simulator.hpp>`. The implementation leaves under
`platforms/SimulatorPlatform/` are not standalone include targets. C++17,
header-only; only the policy/enums and standard library are needed. There are
no live platforms, credentials, network calls, Router bindings or Trading
System ledger writes. The simulator models a broker's physical account, not a
second series/MM state machine.

## Configuration And Usage

Compiling example: [simulator_core_example.cpp](../examples/simulator_core_example.cpp).

```cpp
#include <optionx_cpp/simulator.hpp>
using namespace optionx;
using namespace optionx::platforms;

SimulatorPolicy policy;
policy.defaults.payout_ppm = 750000;
policy.defaults.min_stake_minor = 100;
policy.defaults.max_stake_minor = 10000;
policy.defaults.max_active_global = 2;
policy.defaults.max_active_symbol = 1;
policy.allowed_expiry = {60, 300};
policy.excluded_expiries = {120};
policy.symbols["EURUSD"] = {};
auto validated = SimulatorPolicySnapshot::create(policy);
if (!validated) throw std::invalid_argument("invalid policy");

SimulatorCore core(
    {"paper-example", CurrencyType::USD, 2, 100000},
    {150, 100, 0, 8}, // open delay, response delay, max quote age (ms), lifetime records
    *validated,
    SimulatorQuoteFixture({{"EURUSD", 150, 110000}, {"EURUSD", 60150, 110010}}));
auto command = core.enqueue({"EURUSD", OrderType::BUY, 1000, 60});
if (!command) throw std::runtime_error("queue full/stopped");
core.advance_to(60250);
auto result = core.query(*command); // authoritative net_profit_minor = 750
core.shutdown();
```

`SimulatorAccountConfig` requires a nonempty stable account ID, supported
`CurrencyType`, 0..8 decimal places and nonnegative initial integer balance.
There is no implicit currency conversion. Precision 2 means 100 units = 1
account currency unit, including for stake thresholds in policy. Hosts must
choose precision explicitly for their currency; the default of 2 is not a
claim about native precision of cryptocurrencies.

Execution defaults are zero open/response delay and **zero quote age** (exact
timestamp only). Nonzero freshness is explicitly opt-in. Negative timing,
invalid currency/precision, or invalid configuration throws `invalid_argument`
at construction. A fixture's price units must be positive and use one fixed
scale per symbol; the core compares integers and does not normalize prices.
Ties refund the stake (STANDOFF); no configurable alternative tie policy ships.

## Ownership And Bounds

Every API call is confined to one owner thread. Simultaneous commands mean
FIFO queue acceptance at the same manual timestamp, not concurrent calls into
the core. There is no mutex, worker, wall-clock sleep, infrastructure TaskManager
replacement, or production scheduler. The small event heap is a manual-time
simulation calendar; SIM-4 must integrate existing platform lifecycle/tasks.

`max_records` (default 4096, permitted 1..1000000) bounds **all accepted commands
over the scenario lifetime**, including failed, canceled and terminal history.
There is no eviction, wrapping counter, or silent loss of deduplication facts.
Record/event vectors reserve this capacity at construction. At most one heap
event exists per record. A fixture defaults to 100000 ticks and accepts an
explicit upper bound no larger than 1000000. Account IDs, symbols and tick
symbols are bounded to 64 bytes. Policy's existing count limits apply; its
configuration string bytes and caller-supplied callback captures are host
inputs, not an untrusted serialized ingress or a core-enforced byte budget.

`enqueue()` returning a handle acknowledges only queue acceptance. Capacity,
stopped state or malformed empty/overlong symbol rejects before recording.
Other invalid requests become queryable failed-open evidence during admission.
One record and callback closure are retained per accepted command; closures
are released on notification, shutdown, or destruction. No in-flight callbacks
or raw-this captures outlive the core. Destruction does not invoke user code;
call `shutdown()` explicitly to inspect/drain cancellation evidence first.

Callbacks may query, enqueue, replace policy, or request shutdown. Recursive
`process/advance_to` is rejected; callback-triggered shutdown is deferred until
the current callback returns. A callback exception is contained, counted by
`callback_errors()`, and never retried. Core allocation failure instead stops
the scenario using shutdown, then rethrows; opened exposure is preserved.
The owner must outlive every member call and callback invocation.

## Clock And Execution

Manual clock is nonnegative Unix UTC milliseconds. UTC time-window resolution
uses `(event_ms / 1000) % 86400`. `advance_to(target)` processes each due event
at **its own timestamp**, then reaches the target. Moving backwards is rejected.
Equal-time events use monotonically increasing heap insertion sequence numbers;
there is no hidden phase priority. Commands accepted together are admitted in
acceptance order. Events created by them follow the already queued equal-time
events. A large advance is equivalent to advancing over the same event times.

The transitions are deliberately separate:

1. Queue a command with `received_ms`; no stake/slot is held yet.
2. ADMIT resolves eligibility, balance and both caps at owner time. Reservation
   atomically holds stake and an active slot, records `admitted_ms` and a
   virtual trade ID. Pre-reservation failure has no virtual ID or release.
3. OPEN runs after `open_delay_ms`. Resolve the **current snapshot again**,
   checking symbol/direction/duration, availability, stake and both caps.
   Pending plus open/unresolved count against caps, including the caller's own
   reserved slot exactly once. A cap shrinking below the reserved count rejects
   earlier due opens until the remaining count fits; sequence order decides.
4. Select a fresh eligible opening quote and check money headroom. Only then
   convert pending reserve to exposure and freeze open time, expiry, quote,
   policy version, matching rule IDs, payout and rounded maximum net profit.
   Failed-open releases pending money/slot once and never creates open evidence.
5. SETTLE selects the expiry quote at `open_ms + duration_seconds * 1000`.
   Valid quotes realize net P&L and release exposure/slot at that exact time.
   Missing/stale eligible quotes instead become UNRESOLVED with no net P&L or
   settlement time and with open exposure/slot intact.
6. NOTIFY delivers one result snapshot after `response_delay_ms` for settlement
   or unresolved expiry. Failed-open notification is immediate. There is no
   opening callback in this SIM-2-only API. Query/history see the economic state
   before notification, not a separate callback-dependent financial state.

Every timestamp addition (open delay, duration, response delay) is checked
before reservation. Unsupported duration uses `INVALID_DURATION`; capacity
uses `LIMIT_OPEN_TRADES`, stake bounds use `AMOUNT_TOO_LOW/HIGH`, and insufficient
funds uses `INSUFFICIENT_BALANCE`. Missing opening quote uses `NO_CONNECTION`
plus `unresolved_reason`; unavailable policy uses `INVALID_REQUEST`. Arithmetic
overflow uses `INVALID_REQUEST`, not a wrapped value or a fabricated loss.

## Quote Fixtures

The fixture is fully owned and immutable once constructed. Input ticks may be
out of timestamp order; stable sorting indexes `(symbol, timestamp_ms)`.
Lookup chooses the latest timestamp **<= the event time**, then the last input
observation for equal timestamps. `observation_id` is its 1-based input position.
Conflicting equal-time prices therefore have an explicit deterministic winner,
not a hash/container iteration dependency. Freshness accepts age <= configured
maximum. Future ticks cannot open/close a trade even when already loaded or when
`advance_to` goes far past expiry. Fixture observation order is not live arrival
time. There is no post-construction insertion, source readiness, fallback or
automatic reconciliation; those timing contracts remain SIM-3 work.

## Money And Results

`balance = initial_balance + sum(realized net_profit_minor)`.
`available = balance - pending_reserved - opened_exposure`.
Opening does not debit balance; it moves one holding from reserve to exposure.
LOSS realizes `-stake` once; WIN realizes rounded `stake * payout_ppm / 1000000`
once; STANDOFF realizes zero. Removing exposure makes the stake available again
on WIN/STANDOFF; LOSS consumes it through the single net-P&L balance update.

Nonnegative win profit rounds **half up** to the nearest minor unit: 1 unit at
0.5 => 1, 3 at 0.5 => 2, 101 at 0.75 => 76. Checked quotient/remainder arithmetic
avoids both binary floating point and nonportable `__int128`. All amounts fit
signed int64. At open, all possible concurrent win profits also reserve balance
headroom up to INT64_MAX, so even simultaneous winners cannot overflow later.
This may conservatively reject an otherwise eligible near-INT64_MAX account.

`SimulatorTrade::net_profit_minor` is optional and authoritative. It stays empty
for failed, canceled and unresolved work, including shutdown of an open trade.
No `TradeResult` facade is introduced here. A future SIM-4 adapter may convert
this integer to `TradeResult::profit` at its boundary only, as **net profit**,
never stake-plus-return and never as an input back into the ledger.

Virtual IDs are `sim/<account_id>/<command_id>`, allocated once on reservation;
command IDs monotonically increase from 1 with no reuse. IDs, query and history
are **scenario-local**, not globally unique broker IDs or a durable restart
protocol. Independent/restarted cores using the same account can repeat IDs;
hosts must not combine their histories as one execution namespace. A durable
run identity/epoch is an explicit open decision before persistence/integration.

## Example Ledger

The runnable example uses 1000 minor-unit stake, 150 ms open delay, 100 ms
response delay, exact quotes and payout 0.75. Input prices are 110000 at 150,
110010 at 60150/60400, and 110020 at 120400. First command is BUY at 0,
second is SELL at 60250, both expire after 60 seconds.

| Time ms | Event | Balance | Available | Pending Reserve | Open Exposure |
| ---: | --- | ---: | ---: | ---: | ---: |
| 0 | BUY queued | 100000 | 100000 | 0 | 0 |
| 0 | BUY admitted | 100000 | 99000 | 1000 | 0 |
| 150 | BUY opened | 100000 | 99000 | 0 | 1000 |
| 60150 | WIN, net +750 | 100750 | 100750 | 0 | 0 |
| 60250 | WIN callback | 100750 | 100750 | 0 | 0 |
| 60250 | SELL admitted | 100750 | 99750 | 1000 | 0 |
| 60400 | SELL opened | 100750 | 99750 | 0 | 1000 |
| 120400 | LOSS, net -1000 | 99750 | 99750 | 0 | 0 |
| 120500 | LOSS callback / shutdown | 99750 | 99750 | 0 | 0 |

Shutdown rejects new commands, clears the calendar, cancels queued/pending work
without assigning any open/settlement facts, and releases only pending holdings.
OPEN becomes UNRESOLVED/SHUTDOWN; already unresolved risk stays unresolved.
Opened holdings remain exposure, not available funds, even after shutdown.
Already settled trades stay settled. Undelivered result callbacks are flushed
once at shutdown time, explicitly overriding notification delay but never
economic timestamps/prices or balance. Repeated process/advance/shutdown cannot
settle or notify twice. There is no externally injectable settlement API.

## Complexity And Benchmark

Snapshot details and conservative mutable behavior:
[SIM-0/SIM-1 policy contract](simulator-platform-contract.md). Validation is
`O(R^2 + S*T*E*R)` (two directions), bounded at 200000 cells; snapshot creation
adds a configuration copy and `O(R log R)` sort. Resolution is
`O(log S + log X + R)` for X excluded durations, with O(R) matching/output space
plus matching rule-ID bytes. There is no mutable cache.

The fixture costs `O(Q log Q)` setup, `O(log Q)` lookup and O(Q) storage.
For N accepted commands, the event calendar costs O(log N) per transition and
O(N) space. Core admission/open also scan bounded history to count symbol-active
slots, so cost is O(N + R + log Q + log N), not an O(1) account fast path.
Command-ID query is O(1), virtual-ID query/history O(N) plus owned DTO copying.
Lifetime N and Q bounds make a scenario finite; large backtests must partition
scenarios, not silently evict identities/history. Active-symbol indexing can be
considered later if measured core throughput, not just policy resolution, needs it.

Run `simulator_policy_benchmark` in Release. It checks reference/snapshot
checksums on the same 100 varying requests, then times 10000 snapshot requests.
The fixture has 16 symbols, 64 rules, 65 time cells, 4 expiry cells, 8320 reachable
cells including BUY/SELL. Timing includes request construction and result IDs;
it is informational, not a flaky performance gate. One local Windows x64 run:

| Compiler / Build | Create Snapshot us | Mutable us/resolve | Snapshot us/resolve |
| --- | ---: | ---: | ---: |
| MSVC 19.44 / Release | 1466.3 | 1492.8 | 0.46262 |
| MinGW GCC 15.2 / Release | 1928.1 | 1856.4 | 0.30788 |

Reference checksum and first 100 snapshot checksum: `61016000`; full snapshot
benchmark checksum: `6871886000`. Measurements are machine/load dependent, not
a universal speedup claim or a benchmark of the complete simulator.

## Verification And Remaining Boundaries

Targets: `simulator_policy_test`, `simulator_core_test`,
`simulator_core_consumer_test` (two-TU strict C++17), `simulator_core_example`,
`simulator_policy_benchmark`. Both Windows/Ubuntu workflows build/run all of
them. Local tests use no sleeps, credentials, network or platform owner.

Local verification: MSVC 19.44 x64 Debug and MinGW GCC 15.2 x64 Release both
passed 13/13 policy and 22/22 core tests (the latter includes a 162-scenario
direction/outcome/delay/expiry matrix), the strict two-TU C++17 consumer, and
the runnable ledger example. Both Release benchmarks matched checksums.
Ubuntu and exact-head workflow status are reported by the Draft PR checks,
not inferred from these Windows runs.

For a worktree with uninitialized submodules, reuse a dependency bundle:

```powershell
cmake -S . -B build-sim2-msvc -G "Visual Studio 17 2022" -A x64 `
  -DOPTIONX_BUILD_DEPS=OFF -DOPTIONX_BUILD_TESTS=ON `
  -DOPTIONX_BUILD_SIMULATOR_EXAMPLES=ON `
  -DOPTIONX_DEPS_BUILD_DIR=E:/_repoz/optionx_cpp/build-codex-msvc `
  '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>' `
  '-DCMAKE_EXE_LINKER_FLAGS=/LIBPATH:E:/_repoz/optionx_cpp/build-codex-msvc/lib/Debug'
cmake --build build-sim2-msvc --config Debug --target simulator_policy_test simulator_core_test simulator_core_consumer_test
cmake --build build-sim2-msvc --config Release --target simulator_core_example simulator_policy_benchmark
```

`OPTIONX_BUILD_SIMULATOR_EXAMPLES=ON` builds only these two dependency-light
examples without initializing the other example families. The existing
`OPTIONX_BUILD_EXAMPLES=ON` also includes them. MinGW uses its matching bundle
and `-DCMAKE_CXX_FLAGS=-Wa,-mbig-obj`; do not mix ABI/runtime bundles.

Not implemented: real-time clocks, Router/online ticks, durable history/restart,
unresolved reconciliation/retry, multi-source ready/fallback policy, serialized
config/money parsing, BaseTradingApi overrides, opening callback compatibility,
or Trading System integration. `set_policy()` rejects equal/older versions and
requires strictly increasing versions within one core. The host must map versions
to configuration contents consistently across independent/restarted cores;
there is no durable hash registry. SIM-3 and SIM-4 must settle these contracts explicitly;
paper results are not claims about a live broker's execution quality.
