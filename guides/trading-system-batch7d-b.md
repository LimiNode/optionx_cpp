# Trading System Batch 7D-B: Offline Platform Owner

## Scope And Ownership

The supported aggregate `<optionx_cpp/trading_system_platform.hpp>` prepares
the platform and trading-system domains for `detail::OfflinePlatformOwner`.
It is a deliberately offline/manual integration adapter, not a production
TradingSystem facade or a new execution state machine. The programmable
`BaseTradingPlatform` fake in the tests never performs broker requests.

All lifecycle/storage methods belong to the thread calling `start()`. The
owner drives platforms with `run(false)` / `process()` and its own existing
`utils::TaskManager`; it does not add a scheduler implementation or a worker
thread. Platform callbacks can arrive on other threads, but they only enqueue
owned DTO values. The host supplies endpoint metadata and owns storage,
`ExecutionRecoveryManager`, and the idempotent group-completion policy for the
entire owner lifetime. A wrong endpoint identity/realm fails before dispatch.
Each endpoint requires a distinct platform object as well as a distinct account
ID: one BaseTradingPlatform represents one session. Reusing the same instance
for different accounts is rejected before lifecycle startup or any durable
change. Multi-session platform routing would require a separate explicit contract.

## Execution And Recovery

`submit()` requires a sealed prepared binding and commits submission-started
before calling `BaseTradingPlatform::post_task()`. The posted task invokes the
existing `BaseTradingApi::place_trade()` outside any MDBX transaction. Request
callbacks installed with `TradeRequest::add_callback()` can run synchronously
before `place_trade()` returns. They enqueue into the reviewed handoff; the
execution-owner task applies receipts only after platform processing returns.

Scheduler acceptance is not execution acknowledgement. Rejection, exception,
an API false return, or cancellation of accepted work blocks progression and
retains the durable uncertain boundary and its reservation. None of those
events generates an invented OPEN_ERROR, NOT_SUBMITTED, loss, or new opening.
The conservative cancellation choice can retain risk even when this adapter
knows its task never ran; reconciliation/policy may resolve that later.

Each owner allocates a durable monotonic generation. Callback closures retain
only a weak control state, a generation, and the weak handoff sink. Revoked or
destroyed generations cannot feed a new owner. Receipt IDs combine generation
and a per-generation sequence; the existing adapter provides semantic physical
deduplication even when a repeated callback has a new receipt ID.

`query()` uses the existing known-ID validation in ExecutionRecoveryManager.
A small BaseTradingApi scheduling adapter transfers the already validated
query to the platform TaskManager; no storage access runs on that loop.
Missing/unconfirmed IDs and stale revisions fail closed. It never resubmits.

The owner records recovered and newly submitted group IDs. After physical
receipt commits, it checks the immutable roster against terminal settlement
members, then automatically invokes the supplied idempotent completion owner.
The integration tests bind this to the existing `SyntheticHost::complete()`:
`ExecutionGroupAggregator`, `SeriesProgression`, and
`apply_group_completion_atomic()` update group/series/MM/reservations. The
runtime does not duplicate that policy or calculate MM payloads itself.
Startup rechecks terminal groups, covering crash after physical commit and
before logical completion. Repeated completion after restart returns the
existing durable DUPLICATE result. TradeRecordDB mirroring remains separate.

## Byte Bounds And Shutdown

Handoff limits: 1..4096 entries; configurable 1-byte..64-MiB budget (default
1 MiB); receipt ID <=256 bytes, broker hash <=256 bytes, diagnostic <=4096
bytes. The budget accounts for fixed Entry storage and string contents,
including admitted in-flight copies. Allocator overhead and deque bookkeeping
are additionally bounded by the entry limit. Sealed binding JSON is shared per
sink, not copied per observation; it is trusted durable configuration, outside
the callback-byte budget. A sink must not be made from unbounded host input.
The DTO is size-checked before copying, copied outside the mutex, and published
under the short queue lock. The consumer moves a shared immutable entry out
without copying its DTO under the lock. Drain has a finite entry-count budget
per owner iteration. Size/byte/count overflow or allocation failure blocks
automatic progression; no dropped observation is claimed as durable.

`shutdown()` first stops submissions, drains/cancels platform work while
observation intake remains alive, closes ingress, drains accepted receipts on
the owner thread, then revokes the generation and shuts down owner tasks.
Explicit shutdown is required for a durable final drain. Destructor cleanup
cancels/joins the owned platform lifecycle and closes ingress, but does no DB
work. In-memory observations lost at destruction/crash remain uncertain and
are reconciled on restart. Externally retained stale callbacks are harmless.

## Verification And Remaining Gates

Focused offline tests cover final-first and duplicate/late callbacks,
OPEN_SUCCESS, OPEN_ERROR, CHECK_ERROR, WIN/LOSS/STANDOFF, multi-account partial
completion, post acceptance followed by cancellation/rejection, lost queue,
known-ID restart query, physical-before-logical crash, foreign broker ID,
shutdown drain, callback on another thread, byte/size overflow, allocation
failure, and aggregate-only C++17 consumption. Earlier 7C tests retain the
account/realm, revision, and terminal-evidence corruption matrix.

No live traffic, HTTP retry changes, kurlyk updates, TradeState changes, or
new broker integration. This adapter does not provide a durable notification
outbox, bounded long-history recovery indexes, TradeRecordDB mirror replay,
account configuration management, or production scheduling. Those remain
separate reviewed work; SIM-3/4 are unrelated gates.

Local MSVC and MinGW verification: 21/21 focused tests, including duplicate
platform-instance/account rejection before run or journal changes. MSVC also
passed 10/10 7D-A tests and 283/283
Batch 1--7C-B regressions, and aggregate-only consumer build/run. Both full
platform targets pass: Intrade responses 111/111 and result queries 12/12.
The local prebuilt dependency bundle requires the matching static Debug CRT:

```powershell
cmake -S . -B build-codex-msvc -DOPTIONX_BUILD_TESTS=ON -DOPTIONX_BUILD_DEPS=OFF -DOPTIONX_DEPS_BUILD_DIR=E:/_repoz/optionx_cpp/build-codex-msvc '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>'
cmake --build build-codex-msvc --config Debug --target trading_system_batch7d_b_test trading_system_batch7d_b_consumer_test intrade_bar_api_response_test trade_result_query_test
$env:PATH = "E:\_repoz\optionx_cpp\build-codex-msvc\bin;$env:PATH"
.\build-codex-msvc\Debug\trading_system_batch7d_b_test.exe --gtest_brief=1
.\build-codex-msvc\Debug\intrade_bar_api_response_test.exe --gtest_brief=1
.\build-codex-msvc\Debug\trade_result_query_test.exe --gtest_brief=1
```
