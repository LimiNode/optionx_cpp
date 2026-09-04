# API And Header Contracts

Этот документ фиксирует публичный контракт `optionx_cpp`: как подключаются
заголовки, где проходит граница public API, как устроены typed broker responses
и какие DTO использовать для проверки результата и выгрузки истории сделок.

Открывай его перед изменениями в `include/optionx_cpp/*.hpp`,
`platforms/common/BaseTradingPlatform.hpp`, `data/trading/*`,
`platforms/*/RequestManager.hpp` и broker history/result flows.

## Public Include Contract

Для внешнего пользователя публичный include contract остается классическим:

```cpp
#include <optionx_cpp/optionx.hpp>
#include <optionx_cpp/data.hpp>
#include <optionx_cpp/market_data.hpp>
#include <optionx_cpp/platforms.hpp>
#include <optionx_cpp/platforms/IntradeBarPlatform.hpp>
#include <optionx_cpp/bridges/bot_binary.hpp>
#include <optionx_cpp/bridges/protocol_v1.hpp>
#include <optionx_cpp/bridges/trading_view.hpp>
```

Внутри одной header family используется путь, реально относительный к
включающему файлу. Cross-domain dependency из вложенного header использует тот
же установленный префикс, что и внешний consumer:

```cpp
#include "telegram/TelegramRawMessage.hpp"
#include "detail/Parser.hpp"
#include <optionx_cpp/data/trading.hpp>
#include <optionx_cpp/utils/response_parse_utils.hpp>
```

Правила:

- Не используй `../` в `#include`.
- Не используй quoted `"optionx_cpp/..."`; cross-domain includes используют
  angle brackets и установленный префикс `<optionx_cpp/...>`.
- Не полагайся на `include/optionx_cpp` как на дополнительный include-root:
  consumer contract должен работать только с корнем `include`.
- Public aggregate headers (`optionx.hpp`, `data.hpp`, `market_data.hpp`,
  `platforms.hpp`, `storages.hpp`, `components.hpp`, `utils.hpp`,
  `bridges.hpp`) задают публичные точки подключения.
- Bridge families are public only through `bridges.hpp` or the family umbrella
  headers: `bridges/metatrader_file.hpp`, `bridges/legacy_trading.hpp`,
  `bridges/named_pipe.hpp` and `bridges/trading_view.hpp`.
  `bridges.hpp` also exposes `bridges::BridgeHost`, the application-side helper
  for pre-run/post-run/shutdown/reset hooks around any `BaseBridge`.
  `bridges/named_pipe.hpp` is a compatibility umbrella for the legacy trading
  named-pipe bridge. The BotBinary/BinaryBot compatibility bridge and adapter
  helpers are exposed through `bridges/bot_binary.hpp`, and the Bridge Protocol
  v1 HTTP/WebSocket server is exposed through `bridges/protocol_v1.hpp`. Headers under
  `bridges/<family>/` and `bridges/<family>/detail/` are not standalone public
  include entry points. See `guides/bridge-taxonomy.md` before adding a new
  bridge family or transport.
- Domain aggregates, например `data/trading.hpp`, задают include context для
  связанных leaf headers.
- Leaf DTO headers не должны вручную восстанавливать весь порядок зависимостей
  соседнего домена. Если зависимость общая для домена, держи ее в ближайшем
  aggregate.
- Direct leaf includes допустимы в white-box/internal tests for domains that
  explicitly keep self-contained leaf headers. Bridge family leaf/detail headers
  are an exception: tests and examples must include the bridge umbrella header
  first.

Текущая CMake-сборка tests/examples добавляет один project include-root:
`include`. Он совпадает с consumer contract `<optionx_cpp/...>` и не маскирует
неверные cross-domain quoted includes во вложенных headers.

## Header-Only Ownership

`optionx_cpp` - header-only C++17 библиотека. Большая часть публичной
поверхности живет в headers и компилируется в translation units пользователя.

Правила ownership:

- Новый public header добавляй в ближайший aggregate header.
- Internal helper не расширяет public surface без причины.
- Template-visible implementation должна быть видна из header.
- Свободные функции в headers должны быть `inline`, если header может
  включаться в несколько translation units.
- Не добавляй broad dependency в leaf header, если эта зависимость нужна только
  implementation detail соседнего manager.
- Публичные headers должны сохранять Doxygen `\file` и краткий `\brief`.

## Public Platform API Contract

`platforms::BaseTradingPlatform` - user-facing facade. Внешний код работает
через facade и DTO, а не через platform managers.

Основные public entry points:

- `configure_auth(std::unique_ptr<IAuthData>)`
- `connect(callback)` / `disconnect(callback)`
- `place_trade(std::unique_ptr<TradeRequest>)`
- `fetch_trade_result(TradeResultQuery, trade_result_callback_t)`
- `fetch_trade_history(const TradeHistoryRequest&, trade_history_callback_t)`
- `fetch_trade_history(trade_history_callback_t)`
- `fetch_symbol_list(...)`
- `on_trade_result()`
- `get_info<T>(AccountInfoType)`

Managers (`AuthManager`, `RequestManager`, `TradeManager`, `BalanceManager`,
etc.) являются implementation detail конкретной платформы. Новый user-facing
метод сначала должен появиться на facade/base contract, а затем делегироваться
в manager.

## Common Lifecycle Contract

`optionx_cpp/lifecycle.hpp` exposes the optional
`lifecycle::ILifecycleModule` and `lifecycle::LifecycleStack` API.

- A module implements `process()`, idempotent `shutdown() noexcept`, and the
  terminal predicate `is_stopped()`.
- The stack stores non-owning references. Registered modules must outlive the
  stack and its complete shutdown.
- Registration order is dependency order. Processing runs forward; shutdown is
  staged in reverse, one module at a time.
- Dependencies remain processable until the current dependent module stops.
- Stack calls are owner-loop confined. The stack does not create threads.
- Initialization and `run()` remain explicit because existing module startup
  contracts differ.
- `MarketDataRouter` and `BaseTradingPlatform` implement the common interface.
  Direct lifecycle calls remain supported.

See [lifecycle-stack.md](lifecycle-stack.md) for the complete contract and
[lifecycle-stack.ru.md](lifecycle-stack.ru.md) for the synchronized Russian
version.

## Account Info Subscriber Contract

`components::AccountInfoHub` is an optional fan-out adapter for the single
platform `on_account_info()` callback. It routes immutable `AccountInfoUpdate`
payloads to `IAccountInfoSubscriber` instances and may replay the latest cached
update to late subscribers. It stores subscribers as weak references, so caller
code must keep subscriber objects alive while they should receive callbacks.

Rules:

- The hub does not own account storage, authorization state, platform lifecycle
  or broker sessions.
- `bind_to(platform.on_account_info())` replaces the platform callback with the
  hub dispatcher. Call `unbind_from()` if the callback owner can outlive the
  hub object.
- Subscribers receive account lifecycle and account metadata updates such as
  `CONNECTING`, `CONNECTED`, `DISCONNECTED`, `BALANCE_UPDATED`,
  `ACCOUNT_TYPE_CHANGED`, `CURRENCY_CHANGED` and `OPEN_TRADES_CHANGED`.
- `AccountInfoUpdate::status` identifies the account aspect that changed.
  `AccountInfoUpdate::account_info` carries the current account snapshot, where
  subscribers can query the new value. The update is not an old/new diff object.
- `AccountInfoUpdate::account_id` is the internal OptionX account ID assigned
  by the host application or account registry. Broker/platform user IDs remain
  account snapshot data exposed through `AccountInfoType::USER_ID`.
- Broker-specific trading-condition streams, payout changes and symbol session
  changes should use their own DTO/subscriber contract instead of overloading
  `AccountInfoUpdate`.

## Market Data Contract

Market-data APIs are split into DTO/data types and a provider role:

- `data/bars/*`, `data/ticks/*`, `data/symbol/*` contain payload DTOs such as
  `Bar`, `BarSequence`, `Tick`, `TickSequence` and
  `BarHistoryRequest`.
- `market_data.hpp` exposes the provider role and subscription contract:
  `BaseMarketDataProvider`, `TickSubscriptionRequest`,
  `BarSubscriptionRequest`, `MarketDataSubscriptionBatch`,
  `MarketDataSubscriptionHandle`, `MarketDataSubscriptionResult`,
  `MarketDataBatch<T>`, `MarketDataHub`, `MarketDataRouter`,
  `MarketDataSubscriberBase`, `IMarketDataSubscriber`, and
  `MarketDataContinuityOptions`, `MarketDataContinuityUpdate`, and
  `MarketDataContinuityService`.

Contract rules:

- `BarTimeframe` is a signed 32-bit value in seconds. Values less than or equal
  to zero are invalid in requests.
- Live tick and live bar subscriptions use separate request types because the
  payloads and validation rules differ.
- `on_tick_data()` and `on_bar_data()` deliver `std::unique_ptr` batches.
  Shared stream metadata (`symbol`, `timeframe`, digits, subscription handle)
  lives on the batch; individual `Tick`/`Bar` payloads keep only price/time data
  plus compact `flags`.
- Intrade websocket parsers and polling managers exchange
  `events::TickUpdateBatch` directly. `SingleTick` remains only in the legacy
  `request_price()` and typed `PriceSnapshot` compatibility surface.
- Live data callbacks are flushed from the provider/platform lifecycle
  (`process()` or the worker loop started by `run()`), after queued price events
  are routed and coalesced. Calling `event_bus().drain()` alone is an internal
  event-bus operation and is not part of the public market-data delivery
  contract.
- Payload `flags` encode origin, delivery mode, bar lifecycle, and the compact
  price stream through `MarketDataFlags` and `MarketPriceType`.
- `LIVE_SOURCE` means that a payload originated in a live or polling pipeline;
  `HISTORICAL` means that it came from a history request. `BACKFILL` refines
  `HISTORICAL` for a request that repairs a stream gap.
- `REALTIME` means that a live-source payload is delivered from the current
  live edge. `CATCHUP` means that Router intentionally held that live-source
  payload in a continuity backlog and is now replaying it. They are mutually
  exclusive, and both may be combined independently with `INCOMPLETE` or
  `FINALIZED`.
- Providers and direct hubs should use `mark_live_payload()` for live data;
  history adapters should use `mark_historical_payload()`. These helpers
  preserve bar lifecycle and update origin/delivery flags consistently.
- `REALTIME` is pipeline state, not a wall-clock freshness guarantee. Use
  `received_ms` and `time_ms` when a strategy needs to measure transport or
  broker latency. A trading consumer should update its time series for
  `CATCHUP` payloads but gate current-edge actions on `REALTIME`.
- Live bar payloads with `INCOMPLETE` are mutable snapshots. Consumers that keep
  a local time series should upsert by `(provider_id, subscription_id, symbol,
  timeframe, time_ms)` until a `FINALIZED` payload for the same key arrives.
  Appending every incomplete snapshot as a new candle will create duplicate bars.
- Live bar aggregation finalizes a bar when the first tick from the next
  timeframe bucket arrives or when platform `process()` observes that the
  current bucket has elapsed. The process-time path does not require a later
  tick and emits the final snapshot through the normal `on_bar_data()` callback.
- `on_market_data_status()` is a separate stream-status callback. Data callbacks
  should carry data batches, not connection lifecycle sentinel payloads.
- `on_market_data_status()` is a stream-level event bus, not a per-subscription
  status API. Status updates are keyed by a valid subscription handle when one
  is present; otherwise they are keyed by `provider_id`, payload type, symbol,
  timeframe and transport. Providers are not required to replay cached `READY`
  status to subscriptions created after the source was already ready.
- `MarketDataStatusUpdate::subscription` carries the related subscription handle
  when a provider or router can identify a concrete subscription. If the handle
  is invalid, the update describes the underlying stream/source.
- `MarketDataHub` is the optional fan-out layer for applications that need many
  subscriber objects. It binds to the provider's single tick/bar/status
  callbacks, forwards batches to `IMarketDataSubscriber` instances, and replays
  cached stream statuses to late subscribers. It stores subscribers as weak
  references, so caller code must keep subscriber objects alive while they
  should receive callbacks.
- `MarketDataHub` does not own provider subscriptions and does not replay tick
  or bar payloads. Its status replay happens when a subscriber object is added;
  it is not tied to creating a new provider subscription.
- `MarketDataHub` protects its containers and invokes callbacks outside its
  mutex, but strict replay/live ordering is guaranteed only when add/publish
  calls are marshalled through one owner loop, such as platform `process()`.
  A fully concurrent ordered dispatcher is a separate design.
- `apply_subscriptions()` applies subscription changes atomically. The old
  single-operation helpers (`subscribe_ticks`, `subscribe_bars`, `unsubscribe`)
  are wrappers around a one-operation batch.
- `SUBSCRIBED` means the provider accepted the desired subscription and
  returned a handle. Physical stream readiness is separate and is reported
  through `on_market_data_status()` with `READY`.
- `MarketDataSubscriptionResult::status` is the source of truth. There is no
  separate mutable `success` field; use `result.success()` or `if (result)`.
- Subscription handles are provider-bound. `provider_id` is a runtime identity
  of a concrete provider object, and `unsubscribe()` must reject handles from
  another provider with `WRONG_PROVIDER`.
- A provider object is intentionally non-copyable and non-movable, because
  copying it would duplicate runtime identity and make existing handles
  ambiguous.
- A broker implementation may maintain price polling or websocket connections
  even when no public subscription exists, because trading logic can also need
  current prices. Public subscriptions are the routing contract for external
  consumers, not necessarily the only source lifecycle.
- Public market-data subscriptions are independent from the trading account
  connection state. An account `DISCONNECTED` status must not tear down active
  quote streams that were started by public subscriptions, while an explicit
  platform disconnect or shutdown is still a full stop and should close
  physical streams.
- Historical bars use `BarHistoryResult` so callers can distinguish an empty
  successful range from transport, validation or parser failures.
- `MarketDataContinuityService` is the thin helper for routing recovered history
  into the same bar batch pipeline. It marks payload bars as
  `HISTORICAL` and, for gap recovery, `BACKFILL`.
- `BarSubscriptionRequest::continuity` enables Router-owned bar prefill and
  optional timestamp-gap recovery. Router buffers live batches until the
  corresponding history operation completes and reports route-scoped progress
  through `IMarketDataSubscriber::on_market_data_continuity()`.
- `prefill_bars` requests inclusive timeframe slots ending at the start of the
  current timeframe bucket. `max_buffered_batches` and `max_buffered_items`
  bound live batches held during history; zero disables each limit. On buffer
  overflow Router reports continuity `FAILED`, releases the held live data,
  disables continuity for that route, and resumes with live delivery in
  `DEGRADED`; continuity is not automatically re-enabled for that route.
- `MarketDataContinuityOptions::retry` bounds failed history requests with a
  capped exponential backoff. Retries are scheduled by the periodic Router
  `process()` call, so no continuity timer thread is created. Buffered live
  batches remain withheld while retrying.
- Each bounded gap-history response must cover every expected timeframe slot in
  the actual requested range. Router clips provider payloads to that range
  before validating it; a non-empty partial response is not delivered and does
  not advance the route's continuity watermark. Router reports `FAILED`,
  releases buffered live data, and ends in `DEGRADED` while live delivery
  continues.
- `max_backfill_bars` applies to each individual gap request. Continuity
  telemetry reports the actual bounded `from_time_ms`, `to_time_ms`, and
  `requested_items`, rather than the original unbounded gap.
- `last_bar_time_ms` tracks the latest delivered bar, not continuity trust.
  After any terminal unusable history operation, Router retains the earliest
  unresolved boundary. A later successful history range cannot clear that
  trust loss unless it covers the unresolved boundary; `LIVE` is emitted only
  when no unresolved boundary remains known.
- Successful empty history is terminal: an empty prefill proceeds to live
  delivery, while an empty gap backfill reports failed recovery and releases
  buffered live data without retrying the same range.
- Router preserves ordinary provider bar revisions and leaves general timestamp
  upsert to the consumer. After a successful reconnect overlap, Router removes
  buffered live snapshots whose timestamps were already confirmed by returned
  historical bars, so the same recovery candle is not delivered twice. A chart
  or storage component should still upsert by stream and `time_ms` when it needs
  one current candle value.
- Continuity updates carry the concrete provider subscription handle and must
  not be confused with stream-level `MarketDataStatusUpdate`. History failure
  does not terminate the live route: Router reports operation-level `FAILED`,
  releases buffered live batches, and returns to `DEGRADED` after the retry
  budget is exhausted. `LIVE` means that the route's history-to-live continuity
  has no known unresolved history range; `DEGRADED` means that live delivery
  continues without proof of the complete range. `DEGRADED` remains sticky
  while any earlier unverified range exists; repairing a later independent gap
  cannot emit `LIVE`.
- For initialized `PREFILL_AND_RECOVER` bar routes, transport loss
  (`DISCONNECTED`, `RECONNECTING`, `FAILED`, or `STOPPED`) emits route-scoped
  `STALE`. History recovery cannot start before a later `READY`. Router then
  revalidates from its earliest unverified slot through the last closed candle,
  waiting for a dirty current candle to close when necessary. Gap and reconnect
  responses are clipped to their requested range and must cover every requested
  timeframe slot; a partial response finishes as `FAILED` then `DEGRADED`.
- A disconnect while initial prefill is pending invalidates its old completion
  and repeats the original prefill range after `READY`. In
  `PREFILL_AND_RECOVER`, Router then repairs closed outage slots after the
  original prefill boundary before emitting `LIVE`; plain `PREFILL` remains
  startup-only. A cached invalidating status applies the same transition before
  a newly accepted route may start prefill. Completed `PREFILL` routes do not
  acquire reconnect or timestamp-gap recovery implicitly.
- Generic history continuity is currently defined for bars only. Tick history
  remains a separate provider contract. Router does not apply a universal
  timestamp deduplication policy; consumers decide how to upsert revisions.

`MarketDataRouter` is the subscription-scoped alternative to `MarketDataHub`:

- `subscribe_ticks()` and `subscribe_bars()` bind one weak subscriber to one
  provider subscription and return a move-only `MarketDataRouterSubscription`.
- The RAII handle has a stable router-local strong ID. Use `router_id().valid()`
  or its explicit boolean conversion instead of comparing against an invalid
  numeric sentinel. Its
  `provider_subscription()` descriptor becomes valid after provider acceptance;
  destroying, resetting, or explicitly unsubscribing the handle releases the
  route.
- Routed tick/bar batches and statuses carry that concrete provider subscription
  descriptor, so one bot can distinguish its EURUSD and BTCUSDT routes without
  parsing symbols as logical IDs.
- Stream-level statuses are cached while a provider is bound. When a matching
  late route is accepted, the latest status is replayed with the new concrete
  subscription descriptor. Tick and bar payloads are never replayed.
- Router and hub both own the provider's single live-data callback slots and
  therefore must not be bound to the same provider at the same time.
- Subscribers remain weakly owned. Providers must outlive the router and any
  pending provider operations.
- Releasing a route stops event delivery immediately. If physical provider
  unsubscription is rejected or completes with failure, the Router retains the
  provider handle and callback binding for cleanup. Check
  `failed_unsubscribe_count()` and call `retry_failed_unsubscribes()` from the
  owner loop; new routes through that provider are rejected until cleanup
  succeeds.
- Router shutdown is two-phase. `shutdown()` stops new routes and user delivery,
  retains tombstones for pending provider operations, and performs one immediate
  `process()` pass. Keep the provider and owner loop alive and call Router
  `process()` until `is_shutdown_complete()` becomes true. Late successful
  subscribes are physically unsubscribed without user callbacks or replay;
  failed cleanup remains retryable and prevents shutdown completion.
- `register_provider()` adds a non-owning provider reference under a stable,
  application-assigned `MarketDataProviderId` and optional exact string aliases.
  Registration is a selection catalog only and does not bind live callbacks.
- Registered numeric IDs and aliases resolve to the same provider-reference
  overloads used by direct subscriptions. Unknown keys produce a typed failed
  subscription result, and an active or pending route prevents unregistering
  its provider.
- `ProviderInstanceId` remains the runtime identity carried by provider handles.
  `registered_provider_id()` maps it back to the stable application ID while the
  provider remains registered; aliases are not copied into live batches.
- Router synchronous subscribe/unsubscribe methods are owner-loop operations.
  A Router constructed with `owner_dispatcher_t` exposes `post_to_owner()` and
  marshals provider completions plus tick/bar/status delivery through that same
  FIFO dispatcher. The dispatcher must be thread-safe, must not execute foreign
  calls inline, and must remain available through Router shutdown. Once a
  configured dispatcher starts rejecting work, Router drops new provider
  delivery instead of invoking subscriber code in the foreign source thread.
- `BaseTradingPlatform::post_task()` is the standard adapter for using the
  platform TaskManager as that owner loop. Shutdown may cancel accepted tasks,
  so provider operation results are retained in Router state rather than owned
  only by posted tasks. Router and subscriber cleanup must be drained before
  stopping the platform.

`MarketDataSubscriberBase` is optional convenience sugar over Router. A bot can
derive from it, call protected `subscribe_ticks()`/`subscribe_bars()` from its
own methods, and let the base keep move-only handles alive. The base returns
strong Router IDs, supports direct provider references plus registered provider
IDs and aliases, explicit one/all unsubscribe, and releases all stored routes
during destruction. Default-constructed route IDs represent failure and expose
`valid()`/boolean checks instead of a public sentinel constant. Derived objects
must be created through `std::shared_ptr` before subscribing;
`IMarketDataSubscriber` remains the pure receiving interface for applications
that prefer explicit ownership elsewhere. Bots running on another thread use
`post_subscribe_ticks()`, `post_subscribe_bars()`, `post_unsubscribe()` and
`post_unsubscribe_all()`. These methods report command acceptance immediately;
route/provider callbacks arrive later from the configured owner loop. Subscriber
destruction moves remaining handles into one owner-loop cleanup task.

## Trading Condition Subscriber Contract

`TradingConditionUpdate` is the payload for broker trading conditions: payouts,
symbol tradability, market open/closed state, amount limits, duration limits,
refund limits and max open trades. Condition fields are optional because live
broker messages can update them independently.

`components::BaseTradingConditionHandler` bridges
`events::TradingConditionUpdateEvent` into the platform callback exposed as
`BaseTradingPlatform::on_trading_condition()`.

`components::TradingConditionHub` is an optional fan-out adapter for that
callback. Live subscribers receive incoming updates as-is, while the hub cache
merges optional fields into the current condition snapshot per
`(platform_type, account_type, currency, option_type, symbol)` scope. Late
subscribers may receive those merged snapshots immediately.

Rules:

- Use `AccountInfoUpdate` for account lifecycle and account metadata changes.
- Use `TradingConditionUpdate` for broker trading constraints and payout/session
  changes.
- `TradingConditionUpdate::merge_patch()` is snapshot merge logic, not event
  history. A patch with only `market_open=false` must not erase cached payout or
  expiration limits for the same scope.
- `TradingConditionHub::current_condition(scope)` is the direct read path for a
  concrete current condition state, for example the latest payout and expiration
  limits for one symbol.
- Do not encode trading-condition changes as fake ticks, bars or market-data
  status events. Market-data subscriptions report prices; condition subscribers
  report whether and how a trade can currently be opened.

Intrade Bar publishes condition snapshots for every supported symbol/option-type
scope after the account context becomes known. `TradingConditionManager` also
re-evaluates the time-dependent session, amount, open-trade and sprint-duration
limits from the same `AccountInfoData` model used to validate trade requests.
Only scopes whose values changed are emitted. During platform shutdown it emits
one final `tradable=false` patch for each cached scope before clearing the
manager, which prevents long-lived hubs from retaining stale availability.

Intrade Bar intentionally leaves `TradingConditionUpdate::payout` empty. Its
payout model depends on the concrete trade amount and duration, but those values
are not part of the current condition scope. Publishing one payout per symbol
would therefore be ambiguous. Use `AccountInfoRequest` for the exact prospective
trade until the condition API gains an amount/duration-aware scope.

## Typed Broker Result Pattern

Broker HTTP adapters используют typed result wrappers, чтобы не смешивать
транспортную ошибку, parser failure и успешный payload.

Общая форма: `platforms::ApiResult<T>`.

Инварианты:

- `success` говорит о результате операции.
- `status_code` хранит HTTP status или один из sentinel values:
  `NO_HTTP_STATUS`, `NO_RESPONSE_STATUS`.
- `error_desc` хранит диагностический текст failure.
- `value` хранит typed payload только для успешной операции.
- Ошибка не должна маскироваться пустым payload, если caller должен отличать
  "пустой успешный ответ" от "запрос не удался".

Для Intrade Bar новые `RequestManager::*_result` methods оборачивают старые
request/parser flows в typed results. Они не являются поводом менять parser
literals или HTML constants без live evidence: broker API нестабилен, поэтому
сохраняй то, что уже проверено.

## Trade Result Query Contract

`fetch_trade_result` проверяет результат одной сделки. Он предназначен для
recovery-сценария: бот мог открыть сделку, сохранить промежуточное состояние,
перезапуститься и позже восстановить финальный результат.

DTO:

- `TradeResultQuery` - входной запрос.
- `TradeResult` - заполняемый результат одной сделки.

Контракт:

- Broker trade identity может быть числом, строкой или hash у разных брокеров.
  Текущий Intrade Bar использует `broker_option_id`.
- Не передавай весь `TradeRequest`, если для проверки результата достаточно
  broker id и минимального контекста результата.
- `TradeResult` сохраняет local `trade_id`, account/currency, amount и open
  timing/price, если caller их знает.
- Если платформе не хватает входных данных для корректной классификации
  результата, она должна вернуть failure/diagnostic, а не молча угадать.

## Trade History Contract

`fetch_trade_history` выгружает историю закрытых сделок аккаунта как массив
`TradeRecord`.

DTO:

- `TradeHistoryRequest` - time range, selected time field, range mode, optional
  comment.
- `TradeHistoryResult` - `success`, `status_code`, `error_desc`, `records`.
- `TradeRecord` - нормализованная запись сделки для storage/statistics.

Почему `TradeRecord`, а не `TradeResult`:

- История аккаунта является экспортом/статистикой, а не callback-результатом
  одной активной сделки.
- История должна совпадать с моделью storage и аналитики.
- `TradeRecord.comment` можно использовать, чтобы пометить происхождение
  экспортированных сделок, например `account-history-export`.

Range rules:

- `TradeHistoryRequest::all()` отключает client-side filtering и означает
  "выгрузить все доступное через broker source".
- Ranged request по умолчанию использует `TradeRecordTimeField::CLOSE_DATE`,
  потому что это ближе всего к closed-trade statistics.
- Caller может выбрать другой `TradeRecordTimeField`, если нужна другая
  временная ось.
- Записи без выбранного timestamp исключаются из ranged results.
- `TimeRangeMode::CLOSED` включает обе границы.
- `TimeRangeMode::HALF_OPEN` включает start и исключает stop.

`TradeRecord::close_date` хранит planned or known option close timestamp. Для
classic options это фиксированное `TradeRequest::expiry_time`, известное до
открытия сделки. Для sprint options close time зависит от фактического
`open_date`, поэтому обычно становится известным после открытия как
`open_date + duration`.

Trade statistics ordering:

- Realized monetary curves are event-based. Synthetic equity/profit aggregates
  `profit` by result timestamp, and sweep-line free-funds aggregates all
  open/close deltas with the same timestamp before drawdown is updated.
- `TradeSeriesStats` uses a separate outcome event stream. Series are ordered
  by result timestamp (`close_date`, then `open_date`, then automatic fallback).
  If several outcomes have the same result timestamp, their tie breaker is the
  decision timeline (`place_date`, then `send_date`, then `open_date`), followed
  by `trade_id` and `unique_id`.
- `TradeStatsInputOrder` is a legacy hint and must not be used to promise input
  order semantics for realized curves or win/loss series.

Trade ID and result merge contract:

- `trade_id` is a 32-bit linear persistent identity. `0` means "not assigned".
  `TradeRecordDB` stores this value in the low 32 bits of its composite key;
  the high 32 bits store biased unix minutes. Do not convert the DB key itself
  to a plain 64-bit trade sequence without a separate storage redesign.
- `TradeRecord::apply_result_snapshot()` applies a normal TradeResult snapshot.
  It preserves request/account identity when the corresponding result fields
  are unspecified, but result-state fields are otherwise copied as-is.
- `TradeRecord::merge_result_patch()` is the recovery/status-fixer path. It
  updates only fields that can be distinguished from their default sentinel
  values. `TradeResult` still does not provide presence flags for every scalar,
  so patch semantics must stay conservative near valid zero values.

## Storage Result Contract

Storage services share common operation result DTOs:

- `StorageStatus` - success/failure code for storage operations.
- `StorageWriteResult<Record>` - write/upsert result with the written or
  attempted record.
- `StorageReadResult<Record>` - single-record lookup result with `found`.
- `StorageListResult<Record>` - multi-record query result.

Domain storage APIs keep readable aliases, for example
`TradeRecordDBStatus`, `TradeRecordDBWriteResult`,
`TradeRecordDBReadResult` and `TradeRecordDBListResult`. New storage services
should reuse the common result templates and expose domain-specific aliases
rather than duplicating equivalent result structs.

## Intrade Bar History Sources

Source выбирается через `platforms::intrade_bar::TradeHistorySource` в
`AuthData::trade_history_source`.

Режимы:

- `CSV` - использует `/stat_trade_export.php`; обычно дает лучшее финансовое
  покрытие и более дальнюю историю.
- `HTML` - читает authenticated main page `trade_close`, затем пагинацию через
  `/trade_load_more2.php`; ближе к UI и содержит broker row identity, но
  покрытие ограничено доступной HTML-пагинацией.
- `HTML_CSV` - требует успеха обоих источников и возвращает только записи,
  найденные в обоих. Это осознанный strict режим, а не "максимальная история".

Особенности Intrade Bar:

- HTML load-more endpoint следует текущему account type в broker session; он
  не принимает независимый `account_type` в request body.
- Перед HTML/HTML_CSV history платформа должна быть подключена к нужному
  account type.
- Intrade Bar closed-history sources provide a close timestamp; the adapter
  stores it in `TradeRecord::close_date`.

## New HTTP Broker Checklist

Когда добавляется брокер с похожей HTTP-механикой:

1. Добавь concrete platform facade на базе `BaseTradingPlatform`.
2. Раздели auth/request/trade/balance/history managers по ответственности,
   ориентируясь на Intrade Bar, но не копируя broker-specific quirks.
3. Parser literals держи в platform-specific parser файле.
4. Broker-independent raw-response helpers выноси в `utils`.
5. RequestManager должен возвращать typed results для новых workflows.
6. Историю аккаунта возвращай как `TradeHistoryResult` с `TradeRecord`.
7. Проверку одной сделки возвращай через `fetch_trade_result`.
8. Для online smoke сделай отдельную подпапку в `tests/<broker>_api`.
9. Credentials/proxy держи только в untracked `*.local.env`.
10. Negative-auth tests оставляй manual, если broker может заблокировать
    аккаунт после failed login attempts.

## Documentation And Evidence Rules

Для broker behavior отделяй факты от предположений:

- Факт: подтвержден кодом, fixture-тестом, live smoke или broker response.
- Предположение: разумная гипотеза, но без подтверждения.
- Не меняй parser constants только потому, что HTML/API "должен" быть другим.
- Если reviewer сообщает потенциальный edge case, сначала классифицируй:
  regression, real bug, low-risk hardening, или false positive.
- Документируй intentional trade-offs рядом с кодом и в guide, если они могут
  выглядеть как ошибка при следующем review.
