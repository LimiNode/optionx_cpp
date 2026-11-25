# Trading System Batch 7D-A: Observation Handoff

## Scope

`detail::PlatformObservationHandoff` is a bounded, in-memory callback inbox for
the existing durable platform-receipt adapter. It is an internal leaf reached
through `<optionx_cpp/trading_system.hpp>`, not a broker adapter, scheduler, or
production `TradingSystem` runtime. No broker API, callback, retry, or
`TradeState` contract changes in this slice. Tests use only offline fixtures.

## Owner Protocol

The host owns storage, `ExecutionRecoveryManager`, the handoff, and the platform
endpoint. Storage and manager outlive the handoff. The host captures a sealed
platform binding from the durable prepared operation, commits the
submission-started boundary, then calls `start()` on one execution-owner
thread. `start()` validates the full journal through `recover()` and accepts
callbacks only after successful recovery. For operations created later, the
owner calls `track_durable_operation()` after their journal write commits.

The callback retains a weak reference to the inbox and an owned copy of its
sealed binding. It copies/moves the `TradeResult` and host-supplied receipt ID
under a short queue lock. It never touches MDBX, the manager, or the platform
loop. `ACCEPTED` means **in-memory acceptance only**, not a durable broker or
storage acknowledgement. The host must supply a stable receipt ID and attest
that the callback came from the endpoint represented by the binding; the DTO
alone cannot prove its account origin.

After a broker call returns, the execution owner calls `drain()` from its own
TaskManager loop. The call applies each observation through the existing
transactional `apply_platform_receipt()` path. It processes FIFO entries and
updates its cached operation revision only after a successful durable result.
Duplicate receipts are harmless; conflicting facts remain queued and block
progression. A synchronous broker callback is safe because it only enqueues;
no storage transaction remains open during the external call.

Queue capacity is fixed at construction (1..4096). Overflow, invalid receipt
identity, or enqueue failure blocks the inbox. The dropped observation is not
reported as durable. The host must stop new progression and reconcile the
unresolved operation from authoritative evidence after restart; it must not
resend a possibly submitted trade. Existing reservations remain held unless
the durable receipt adapter receives conclusive non-execution or settlement
evidence. `ready()` is an ingress-health signal, **not** risk admission or a
claim that all outstanding operations are resolved.

Shutdown order is: stop new submissions, allow platform callbacks already in
flight to finish, close ingress, call `shutdown_and_drain()` on the owner
thread, then destroy the handoff after the platform has drained/joined.
Callback sinks invoked after handoff destruction return `CLOSED`. A failed
drain is not retried by the destructor. On restart, recover the durable
operation and binding, query/reconcile when supported, and never infer
non-execution from a missing callback. No queued-but-uncommitted callback is
assumed to survive process interruption.

## Boundary To 7D-B

This slice is manually pumped: it does not schedule the owner task or provide
a durable notification acknowledgement. Scheduling failure and cancelled
posted work remain fail-closed host responsibilities. It does not call logical
group completion automatically. The existing group/series/MM completion path
is exercised after durable physical settlement, including duplicate and late
callback cases, but integrating that trigger with the owner loop is a later
stage. It does not change account registry, platform lifetime, reset policy,
or `TradeRecordDB` mirror replay.

The focused offline tests cover callback-before-return, CHECK_ERROR after
confirmed opening, duplicate and conflicting terminal receipts, overflow,
shutdown/lifetime, lost callback and restart, wrong-thread drain, and
idempotent logical completion. They do not establish live broker behavior or
full production lifecycle integration.

Windows smoke also builds and runs `intrade_bar_api_response_test` and
`trade_result_query_test` instead of stopping after compilation. The bundled
curl snapshot provides `libcurl.dll.a`; the MSVC linker can consume that import
library when it is explicitly included in the network dependencies. Updating
the Kurlyk gitlink to v1.1.1 is unrelated to this curl linkage: that release
retains the same curl fallback and primarily updates WebSocket packaging.
