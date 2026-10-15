# Bridge Lifecycle Guide For Coding Agents

This guide captures the lifecycle patterns learned while hardening the
MetaTrader file bridge and Bridge Protocol v1 HTTP/WebSocket bridge. Use it
before adding or reviewing a bridge that owns external transports, callbacks,
worker threads, or idempotent command dispatch.

## Why Bridges Are Tricky

Platform code usually owns its event loop and component graph. A bridge sits on
the boundary between OptionX, third-party transport runtimes, and user
callbacks. That boundary creates failure modes that are easy to miss:

- HTTP/WebSocket libraries can create internal worker threads.
- User callbacks can call back into `run()`, `shutdown()`, update hooks, or
  destruct the bridge.
- A request handler may still need to write a response after a callback asks for
  shutdown.
- WebSocket clients can disconnect while broadcast is reserving or completing a
  send.
- Two lifecycle calls can race across external threads, transport threads, and
  status callbacks.
- Detached work must not depend on the lifetime of the bridge object.

Treat every bridge as a small concurrent system, not as a thin adapter.

## Core State Machine

Use an explicit runtime phase:

```text
Stopped -> Starting -> Running -> Stopping -> Stopped
```

Required invariants:

- `Stopped` means all transport server objects are stopped and all owned
  transport threads have completed or have been safely joined.
- `Starting` and `Stopping` belong to a specific lifecycle generation.
- Only the owner of a generation can publish its final state.
- `run()` must wait while `Stopping` is in progress, unless it is called from a
  bridge callback where waiting would deadlock.
- `shutdown()` must be idempotent.
- A failed startup must roll back only the generation that failed.

Keep a monotonic `lifecycle_generation`. Pass the expected generation into
startup rollback and generation-specific shutdown paths.

## Shutdown Ownership

Only one caller should own a shutdown transition.

Recommended behavior:

- The first caller that changes `Running` or `Starting` to `Stopping` becomes
  the shutdown owner.
- Concurrent external shutdown callers wait for `Stopped`.
- Reentrant shutdown from a callback returns quickly after recording intent.
- Only the owner for `stopping_generation` can publish `Stopped`.

Avoid this pattern:

```cpp
// Wrong: any shutdown caller can publish Stopped.
state->phase = RuntimePhase::Stopped;
state->lifecycle_cv.notify_all();
```

Use generation checks in finalization:

```cpp
if (state->phase == RuntimePhase::Stopping &&
    state->stopping_generation == stopping_generation) {
    state->phase = RuntimePhase::Stopped;
    state->lifecycle_cv.notify_all();
}
```

## Callback Reentrancy

Callbacks are part of the bridge lifecycle surface. Assume callbacks can call
`shutdown()`, `run()`, update hooks, and diagnostic hooks.

Rules:

- Do not hold the bridge I/O or lifecycle mutex while invoking user callbacks.
- Track callback execution with a guard.
- The guard identity must be instance-specific. A callback in one bridge must
  not affect lifecycle calls on another bridge.
- Prefer a stable runtime identity, such as `RuntimeState*` held by
  `std::shared_ptr<RuntimeState>`, over a raw bridge object pointer.
- `run()` or `shutdown()` called from a callback must not wait for the same
  callback path to unwind.

Do not use a process-wide or type-wide callback depth counter. It creates false
reentrancy across independent bridge instances.

## Callback-Initiated Shutdown

If a callback asks for shutdown while a request handler is still executing, do
not stop the transport immediately.

Problem sequence:

```text
HTTP worker
  -> handle request
    -> user callback
      -> bridge.shutdown()
        -> server.stop()
    -> write JSON response
```

The response can be lost if `server.stop()` wins the race.

Preferred pattern:

1. `shutdown()` inside a callback records a pending shutdown for the current
   generation and closes admission for new transport callbacks.
2. New HTTP/WS handlers that arrive after admission closes must not invoke user
   callbacks. They should return a documented stopping response or close.
3. Already admitted transport handlers write their responses or enqueue their
   WebSocket replies.
4. The last admitted transport scope drains the pending shutdown after all
   active transport scopes for that runtime have unwound.
5. Stop and join happen in a reaper thread or in the external shutdown caller.

Track active transport callback scopes in shared runtime state, not just in
`thread_local`, because HTTP servers can run multiple worker threads. The drain
must check both `active_transport_callbacks == 0` and closed admission while
holding the same mutex; otherwise a new handler can enter between "saw zero"
and the actual drain.

Status and lifecycle callbacks are not always transport callbacks. If
`shutdown()` is called from a non-transport callback while the runtime is already
`Running`, recording `pending_shutdown` is not enough: there may be no transport
scope destructor left to drain it. After recording the pending stop, immediately
attempt the same generation-safe drain. If transport callbacks are still active,
the drain should return and the last transport scope will retry it; if none are
active, the async reaper can stop and join the transport threads.

## Starting Callbacks

`SERVER_STARTED` is often emitted from the transport thread that runs
`server->start()`. Shutdown requested from this callback is special:

- Do not stop the server from inside the `server->start()` ready callback.
- Do not detach the server thread and publish `Stopped` before that thread has
  really returned.
- Record pending shutdown while phase is `Starting`.
- Let `run()` observe the pending shutdown after the ready callback returns and
  perform generation-safe shutdown from the caller thread.

Regression tests should restart immediately on the same fixed port after
shutdown from `SERVER_STARTED`. This catches old-generation threads that were
detached instead of joined.

If `server->start()` reports ready and later throws from its transport thread,
handle rollback like a generation-specific async shutdown. The failing
transport thread must not publish `Stopped` through a self-detach fallback.
Move the thread handle to a separate reaper and let that reaper stop/join before
publishing final state.

## Thread Joining

Never self-join. But also do not detach a live transport thread just because the
shutdown request originated from that thread.

Use two paths:

- Synchronous shutdown path: if finalization runs on the current thread, avoid
  self-join and detach only when there is no safe owner that can join.
- Async reaper path: finalization runs on a separate thread, so move all
  transport thread handles into the reaper and join them there.

`Stopped` must not be published until the reaper has stopped server objects and
joined the moved thread handles.

## Detached Work And Object Lifetime

Detached lambdas must not capture raw `this`.

Bad:

```cpp
std::thread([this] {
    finalize_shutdown();
}).detach();
```

Good:

```cpp
std::thread([state = m_state, handles = std::move(handles)]() mutable {
    finalize_shutdown(state, std::move(handles));
}).detach();
```

The finalizer should be static or otherwise independent of the bridge object.
Callbacks needed by finalization should be copied from `RuntimeState` under
lock, and callback guards should use the stable runtime identity.

## Recursive Async Callback Ownership

Recursive asynchronous handlers need an explicit ownership graph. Do not let a
`std::function` or lambda own, through `std::shared_ptr`, the object that
contains that same callback. The resulting cycle is permanent:

```text
shared_ptr handler
  -> std::function
    -> stored lambda
      -> same shared_ptr handler
```

The cycle leaks the handler and everything captured by it. It is separate from
the lifetime of a raw `this` pointer: a leaked cycle does not keep the pointed-to
object alive, and an in-flight callback can still use a raw `this` after that
object has been destroyed unless shutdown fencing proves otherwise.

Use weak ownership in the stored recursive handler and acquire strong ownership
only for the active asynchronous operation:

```cpp
auto handler = std::make_shared<Handler>();
const std::weak_ptr<Handler> weak_handler = handler;

*handler = [weak_handler](Args... args) {
    const auto strong_handler = weak_handler.lock();
    if (!strong_handler) {
        return;
    }

    start_async([
        strong_handler
    ](Response response) {
        // The next recursive call is valid while this operation owns the handler.
        (*strong_handler)(make_next_args(response));
    });
};
```

For every recursive async flow, review all terminal paths: success, failure,
cancel, timeout, and shutdown. The last strong owner must be released when no
next operation is scheduled. Also verify the lifetime of every captured `this`:
use shutdown cancel/drain/join or a stable lifetime guard, rather than assuming
that an unrelated `std::shared_ptr` capture makes a raw pointer safe.

## Status Notifications

`SERVER_STOPPED` is part of lifecycle finalization.

Preferred rule:

- Stop and join transport objects first.
- Under the lifecycle mutex, publish `Stopped`, clear generation-owned stop
  flags, and wake `lifecycle_cv`.
- Release the mutex.
- Then invoke external `SERVER_STOPPED` callbacks.

This ordering lets a `SERVER_STOPPED` callback call `run()` without waiting on
the finalizer that is currently invoking the callback. The callback must still
be associated with the runtime generation that emitted it. Do not let a stale
old-generation callback stop or corrupt a newer generation.

Avoid this pattern:

```text
finalizer publishes SERVER_STOPPED
  -> callback calls run()
    -> run() waits for phase Stopped
finalizer cannot publish Stopped until callback returns
```

If a bridge chooses a different ordering, tests must prove that
`SERVER_STOPPED -> run()` and `SERVER_STOPPED -> shutdown()` cannot deadlock and
cannot affect the wrong generation.

## Dispatch And Idempotency During Shutdown

If a command is already in dispatch when shutdown begins:

- Complete it with a durable or cached terminal result such as
  `server_stopped`.
- Do not leave it permanently `dispatching`.
- If another request retries the same idempotent operation while the first is
  still running, return a stable `operation_in_progress` response instead of
  blocking the transport thread.

Do not call user trade callbacks under lifecycle or I/O locks.

## WebSocket Backpressure

For WebSocket bridges:

- Track pending message count and pending bytes per connection.
- Reserve capacity before sending.
- Complete or release capacity in send callbacks.
- On overflow, close or drop the connection according to the documented
  contract.
- Remove connection state on close, error, and shutdown.

Broadcast must tolerate disconnect racing with send completion.

## Test Checklist

Add tests for these scenarios when implementing a bridge transport:

- `run()` while already running returns quickly.
- Startup failure leaves no running transport and allows a later successful
  `run()`.
- External `run()` waits while `shutdown()` owns `Stopping`.
- Two concurrent external `shutdown()` calls do not publish `Stopped` early.
- `shutdown()` from an HTTP request callback returns without deadlock and still
  lets the HTTP response reach the client.
- New HTTP/WS command handlers cannot enter user callbacks after callback
  shutdown has closed transport admission.
- `run()` and `shutdown()` from a request callback do not deadlock.
- `shutdown()` from `SERVER_STARTED` joins the old generation before immediate
  restart on the same fixed port.
- `shutdown()` from a `SERVER_STOPPED` callback is a safe no-op for the
  completed generation.
- `run()` from a `SERVER_STOPPED` callback can start a new generation without
  deadlock or interference from the completed generation.
- A delayed startup failure callback cannot stop a newer generation.
- WebSocket broadcast handles disconnect, slow clients, and queue overflow.
- Oversized, malformed, partial, and unauthorized requests return documented
  errors without crashing the server loop.

Run the lifecycle tests on both Windows and Linux. Thread scheduling differences
regularly expose transport races on only one platform.

Prefer deterministic synchronization over sleep-based assertions. A short
`sleep_for(100ms)` can be useful as a smoke guard, but it should not be the only
proof that a callback, stop notification, or lifecycle transition did not happen
too early. Use latches, condition variables, atomic status flags, unique
per-test endpoints, and real restart/connect attempts to prove the state
machine invariant.

## Review Checklist For Agents

When reviewing bridge code, ask:

- Who owns each transition in the runtime phase machine?
- Which generation is being stopped or rolled back?
- Can a callback call `shutdown()`, `run()`, or destroy the bridge?
- Are callbacks invoked without holding locks?
- Can a callback-initiated shutdown cut off the current HTTP/WebSocket response?
- Does detached work capture `this`?
- Does `Stopped` really mean transport threads have ended?
- Can an old callback affect a new generation?
- Are in-flight operations completed or failed closed during shutdown?
- Do tests prove the lifecycle behavior, or do they only prove that CI was lucky?

## Reviewing Stacked Or Stale PRs

Bridge fixes often arrive as a stack: dependency PR, bridge PR, and follow-up
tests. Review reports must be bound to exact commits.

Before accepting or acting on review feedback:

- Record the expected PR number, head SHA and base SHA.
- Re-fetch PR metadata before reviewing. If the head changed, do not review the
  old diff silently.
- For submodules, record both the parent gitlink SHA and the dependency PR head.
- Distinguish production defects from CI or test-stub defects. A missing
  Windows stub constant can break dependency CI without changing production
  semantics, but it still blocks merge when the required check is red.
- After a lower/base PR is merged, rebase or retarget each remaining upper PR
  onto the updated base, verify its new base/head SHAs, and re-run its checks
  before merging the next layer.
- If an old report names stale heads, map each finding to the current code
  before reopening work. Many lifecycle findings are valid for one generation
  of the patch and already closed in the next.
