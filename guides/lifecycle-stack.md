# Lifecycle Stack Guide

This is the canonical guide for the optional common lifecycle API. Keep
[`lifecycle-stack.ru.md`](lifecycle-stack.ru.md) synchronized when this contract
changes.

## Purpose

`LifecycleStack` lets an application drive several modules through one
`process()` call and one staged `shutdown()` request. It is useful when a
platform owner loop, `MarketDataRouter`, bots, node systems, and similar modules
start and stop together.

The stack is optional. Every module keeps its direct lifecycle API and may be
managed without `LifecycleStack`.

Public include:

```cpp
#include <optionx_cpp/lifecycle.hpp>
```

## Module Contract

Modules implement `optionx::lifecycle::ILifecycleModule`:

```cpp
class ILifecycleModule {
public:
    virtual void process() = 0;
    virtual void shutdown() noexcept = 0;
    virtual bool is_stopped() const noexcept = 0;
};
```

The contract is deliberately small:

- `process()` advances normal work or an in-progress graceful shutdown;
- `shutdown()` is an idempotent request to stop accepting new work;
- `is_stopped()` becomes true only after module-owned work and cleanup finish.

`shutdown()` does not have to complete asynchronous cleanup before returning.
The owner loop continues calling `process()` until the terminal state is
reached.

`MarketDataRouter` implements this interface. Its common `is_stopped()` state is
the same as `is_shutdown_complete()`. `BaseTradingPlatform` also implements the
interface and reports its existing terminal lifecycle state.

## Registration And Ownership

Register dependencies first and dependents last:

```text
platform / owner executor
    -> provider-facing Router
        -> bots or node systems
```

```cpp
lifecycle::LifecycleStack application;
application.add_module(platform);
application.add_module(router);
application.add_module(bot);
```

The stack stores non-owning pointers. Every registered module must outlive the
stack and its complete shutdown. Duplicate registration, self-registration, and
registration after shutdown starts are rejected.

Registration order is a dependency declaration, not merely presentation order:

- normal `process()` runs forward;
- `shutdown()` runs in reverse;
- only one dependent shutdown stage is active at a time;
- lower-level modules keep processing until the current dependent reports
  `is_stopped()`.

Synchronous stages can collapse into one `shutdown()` call. An asynchronous
stage pauses the reverse walk until later `process()` calls complete it.

## Startup

`LifecycleStack` does not call `initialize()` or `run()`. Existing objects have
different startup contracts: a platform has `run(bool)`, a component has
`initialize()`, and a bot may require application-specific configuration. Keep
that work explicit:

```cpp
platform.configure_auth(...);
platform.run(false);
bot.start();

lifecycle::LifecycleStack application;
application.add_module(platform);
application.add_module(router);
application.add_module(bot);
```

This avoids inventing a lowest-common-denominator startup API. A separate
initialization capability can be added later if multiple real modules share the
same semantics.

## Owner Loop

Call `LifecycleStack::process()` and `shutdown()` from the same owner loop. The
stack itself does not create a thread and does not add synchronization around
module methods.

For a manually driven platform, the host loop becomes:

```cpp
while (running) {
    application.process();
}

application.shutdown();
while (!application.is_stopped()) {
    application.process();
}
```

With registration order `platform -> router -> bot`, each tick first lets the
platform execute queued provider callbacks, then lets Router consume retained
lifecycle completions, then processes the bot while it is still active.

Do not also drive a platform manually when it already owns a worker thread.
For that mode, schedule the common supervisor on the actual owner loop or keep
using the modules' direct lifecycle APIs.

## Staged Shutdown

Given this registration order:

```text
platform -> router -> bot
```

the shutdown sequence is:

```text
bot.shutdown()
while bot is not stopped:
    platform.process()
    router.process()
    bot.process()

router.shutdown()
while router is not stopped:
    platform.process()
    router.process()

platform.shutdown()
```

Application code sees only `application.shutdown()` and
`application.process()`. The stack keeps the platform/executor alive while
Router waits for late provider completions and physical unsubscribe results.

`LifecycleStack` is also an `ILifecycleModule`, so stacks may be nested when a
larger application has independently composed subsystems.

## Failures And Limits

The stack does not invent retry, timeout, or abandon policies. For example, a
failed Router unsubscribe keeps Router and therefore the whole stack in a
non-terminal state. The application may inspect
`failed_unsubscribe_count()` and call `retry_failed_unsubscribes()` with its own
backoff while the provider remains alive.

`process()` exceptions propagate to the caller. Module shutdown is `noexcept`
by contract. The stack does not own modules, destroy them, or call shutdown from
its destructor.

The runnable integration example is
[`examples/lifecycle_stack_example.cpp`](../examples/lifecycle_stack_example.cpp).
It combines an owner-loop executor, a deferred market-data provider, and
`MarketDataRouter`, then drains them through the common stack.
