# Implementation Notes

Внутренние правила и инварианты `optionx_cpp`. Открывай этот документ перед
изменениями в lifecycle, async, HTTP/WebSocket, trade queue или storage.

## Header-Only И Include Model

- Основной код живет в `include/optionx_cpp`.
- Headers используют `#pragma once` и macro include guard.
- Aggregate headers (`optionx.hpp`, `utils.hpp`, `data.hpp`, `components.hpp`,
  `platforms.hpp`) формируют публичную поверхность.
- Public domains use an aggregate-first include model: users include the nearest
  aggregate header first, and that header prepares shared std/third-party/domain
  prerequisites in the intended order.
- Domain aggregates such as `data/trading.hpp` own common include context for
  their leaf DTO headers. Leaf DTO headers should not recreate sibling include
  order or pull broad dependencies unless that leaf truly owns the dependency.
- External consumers and nested cross-domain headers include through
  `<optionx_cpp/...>`. Headers within one family use paths genuinely relative
  to the including file.
- Do not use quoted `"optionx_cpp/..."` or `../` include paths. The local
  test/example CMake setup exposes only `include` as the project include root,
  matching the consumer contract and catching invalid cross-domain quotes.
- При добавлении public header обновляй ближайший aggregate header; при
  внутреннем helper не расширяй public surface.

Осторожно: non-`inline` свободные функции в header могут дать ODR-проблемы,
если header включается в несколько translation units. В существующем коде есть
старые места с такими функциями; не копируй этот стиль в новый код.

## Platform Lifecycle

Опорный файл: `platforms/common/BaseTradingPlatform.hpp`.

Lifecycle:

1. Constructor concrete platform создает account data и managers.
2. `run()` добавляет single task `initialize`.
3. Periodic task `loop` вызывает `m_event_bus.process()`,
   `component->process()`, `on_loop()`.
4. `shutdown()` останавливает `TaskManager`, вызывает `component->shutdown()`,
   затем `m_event_bus.drain()`.

Инварианты:

- `shutdown()` idempotent через атомики `m_stopping`/`m_stopped`.
- После shutdown `run()` логирует warning и не стартует lifecycle заново.
- Если используется `run(false)`, caller обязан регулярно вызывать
  `process()`.
- Registered components хранятся как raw pointers; concrete platform должна
  владеть ими как fields и гарантировать lifetime.

### Optional Application Lifecycle Stack

`lifecycle::LifecycleStack` composes top-level modules without replacing their
direct APIs. Register dependencies first. Normal processing runs forward;
shutdown is staged in reverse and does not stop a lower-level executor until
the current dependent module reports `is_stopped()`.

The stack is non-owning and owner-loop confined. It intentionally does not call
`initialize()` or `run()` because existing platform, component, Router, and bot
startup contracts are not equivalent. Keep startup explicit and use the stack
only when common processing and shutdown are useful. The canonical contract is
in [lifecycle-stack.md](lifecycle-stack.md).

### IntradeBar Delayed Retry Lifecycle Note

Do not report a use-after-free risk for the current IntradeBar settings-switch
delayed retry flow unless you find a new path that bypasses the shutdown chain
below or removes the task shutdown guard.

Current chain:

1. The delayed retry task is owned by `AuthManager::m_task_manager`.
2. `AuthManager::shutdown()` calls `m_task_manager.shutdown()`.
3. `TaskManager::shutdown()` marks tasks as shutdown and runs a final
   `process()`.
4. The delayed retry callback receives the `Task` and checks
   `task->is_shutdown()` before invoking the lambda that captures `this`.
5. `IntradeBarPlatform::~IntradeBarPlatform()` calls `shutdown()` while
   derived members still exist. The later `BaseTradingPlatform` destructor call
   is a no-op because platform shutdown is idempotent.

So the reviewed IntradeBar delayed settings-switch retry path does not
dereference a destroyed `AuthManager` during normal platform shutdown. For new
platforms or future lifecycle changes, verify that the concrete platform calls
`shutdown()` while its component fields are still alive, and that delayed callbacks
guard on `task->is_shutdown()` before using captured owners.

## Pub-Sub

Опорные файлы: `utils/pubsub/EventBus.hpp`, `EventMediator.hpp`,
`EventAwaiter.hpp`, `data/events/*.hpp`.

Правила:

- Components общаются через typed events, не через прямые вызовы друг друга.
- `EventMediator` в destructor отменяет awaiters и отписывает listener.
- `await_once` создает self-owned awaiter и выполняет callback один раз.
- Async events должны проходить через `notify_async(std::unique_ptr<Event>)` и
  обрабатываются на `EventBus::process()`.

Осторожно:

- Не сохраняй raw pointer на event после callback.
- Predicate/callback в `await_once` исполняются на event loop thread.
- Если component может быть destroyed раньше event delivery, подписки должны
  принадлежать `EventMediator`, а не ручным raw callbacks без unsubscribe.

## Task Scheduling

Опорные файлы: `utils/tasks/Task.hpp`, `utils/tasks/TaskManager.hpp`.

`TaskManager` поддерживает single, delayed, periodic, on-date tasks и named
overloads. `run()` запускает worker thread, `process()` выполняет ready tasks в
текущем thread.

Инварианты:

- `add_*` возвращает `false`, если manager в shutdown state.
- `shutdown()` notify/join worker thread или делает финальный `process()` без
  worker.
- `force_execute()` выставляет atomic flag; callbacks должны быть готовы к
  forced execution.
- Task callback получает `std::shared_ptr<Task>` и может проверить
  `is_shutdown()`.

Не добавляй отдельный thread loop в manager, если можно вписаться в platform
`TaskManager` или `BaseComponent::process()`.

## HTTP Requests

Опорный файл: `components/BaseHttpClientComponent.hpp`.

Модель:

- Derived HTTP component запускает request через `kurlyk::HttpClient`.
- Возвращенный `std::future<kurlyk::HttpResponsePtr>` регистрируется через
  `add_http_request_task(future, callback)`.
- На каждом `process()` готовые futures читаются, callback получает response.
- В destructor/shutdown requests отменяются, rate limits удаляются.

Инварианты:

- `process()` и `shutdown()` final: derived class не должен перекрывать этот
  lifecycle.
- Exceptions из future ловятся и переводятся в error response.
- Rate limit ids хранятся в `m_rate_limits`; используй enum class
  `RateLimitType` внутри конкретной платформы.

## Broker HTTP Adapter Pattern

Platform `RequestManager` classes should keep broker-specific HTTP and parser
quirks inside the platform folder, but expose typed workflow results to higher
managers.

Current pattern:

- Low-level request methods keep the broker request/response flow close to the
  existing implementation.
- New `*_result` methods wrap the same parser path into `ApiResult<T>` payloads.
- `ApiResult<T>` separates `success`, `status_code`, `error_desc`, and typed
  payload. Do not represent a failed request as an empty successful payload.
- Parser literals and HTML markers are evidence-sensitive. Do not rewrite them
  just because a normalized API would look cleaner; first prove the broker
  response changed or add a fixture/live smoke case.
- Broker-independent raw-response helpers belong in `utils`, not in a concrete
  platform folder.

For user-facing broker features, add facade methods on
`BaseTradingPlatform`/concrete platform and delegate into managers. Do not make
application code call platform managers directly.

### IntradeBar Settings Switch Acknowledgement

`request_switch_account_type_result` and `request_switch_currency_result` treat
broker body `ok` as acknowledgement that the account setting changed. The auth
flow then updates local `AccountInfoData` without an immediate follow-up
`request_profile`.

Do not flag this as a missing verification unless there is live evidence that
Intrade Bar can return `ok` while leaving the setting unchanged. The extra
profile request would add latency, rate-limit pressure, and another broker
failure point to every successful switch. Existing balance/profile maintenance
can still detect a later mismatch and restart auth.

## WebSocket

Опорный файл: `platforms/TradeUpPlatform/WebSocketManager.hpp`.

Текущий WebSocket код platform-specific и не включен в публичную композицию
`TradeUpPlatform`. Перед расширением проверь, должен ли manager стать частью
facade lifecycle или остаться probe/internal component.

Правила:

- Auth/application events должны идти через `data/events`.
- Сетевой lifecycle должен иметь явный cancel/shutdown.
- Не смешивай WebSocket state с HTTP request queue без понятного owner.

## Trade Queue And State

Опорные файлы:

- `components/BaseTradeExecutionComponent.hpp`
- `components/BaseTradeExecutionComponent/TradeQueueManager.hpp`
- `components/BaseTradeExecutionComponent/TradeStateManager.hpp`

Модель:

- Public `place_trade()` принимает unique request.
- Queue manager создает/ведет transaction event/result.
- State manager отвечает за допустимые переходы и проверки.
- Trade result callbacks вызываются через queue/result flow.
- Persistent trade storage uses TradeRequest::trade_id, not unique_id.
- `TradeQueueManager::add_trade()` is the only queue entry point intended for
  external callers. It synchronizes only final insertion into the pending
  queue; the caller must ensure the trade ID provider, account info access, and
  preprocess callback are safe from that calling thread.

Инварианты:

- Не создавай trade result callback вручную в обход queue manager.
- Для сохраняемых сделок резервируй trade_id до отправки брокеру.
- Не используй TradeRequest::unique_id как DB identity.
- Не строй primary ID сделки из даты или timestamp bucket.
- Сохраняй propagation TradeRequest::trade_id -> TradeResult::trade_id.
- Не меняй active/pending trades напрямую из platform manager.
- Do not call `TradeQueueManager::process()`, `finalize_all_trades()`, or event
  handlers concurrently with the platform loop. Local open-trade counters,
  broker snapshot counters, and active transaction lists are single-loop state.
- Preprocess hook должен вернуть `false` и заполнить result error, если request
  невалиден.
- On shutdown все pending/active trades должны финализироваться.

## Account Info

Опорные файлы:

- `data/account/BaseAccountInfoData.hpp`
- `components/BaseAccountInfoHandler.hpp`
- `components/BaseTradeExecutionComponent/AccountInfoProvider.hpp`

Модель:

- `BaseTradingPlatform` хранит shared `BaseAccountInfoData`.
- `AccountInfoProvider` дает typed read API.
- `BaseAccountInfoHandler` прокидывает account info callback.
- Updates идут через account/events/managers.

Не меняй account info напрямую из application code. Для user-facing чтения
используй `BaseTradingPlatform::get_info<T>()`.

## Session Storage

Опорный файл: `storages/ServiceSessionDB.hpp`.

Модель:

- Singleton `ServiceSessionDB::get_instance()`.
- Key = `platform + ":" + email`, затем Base64.
- Value шифруется `crypto::AESCrypt` и хранится как Base64.
- Backend: `mdbxc::KeyValueTable<std::string, std::string>`.
- Методы protected mutex.

Инварианты:

- Перед production use задай key через `set_key`; default key в constructor
  удобен для разработки, но не должен считаться безопасной политикой.
- `uses_default_key()` / `has_custom_key()` позволяют проверить, что session
  storage работает с caller-provided key; сохранение session с default key
  логирует warning.
- IV, generated AES keys and in-memory `SecureKey` masks come from the
  operating-system random source (`BCryptGenRandom` on Windows, `/dev/urandom`
  on Unix-like systems). The implementation deliberately avoids
  `std::random_device` because older MinGW/libstdc++ builds could return
  deterministic `random_device` output.
- Не обходи сервис прямым доступом к mdbx table.
- `shutdown()` disconnects DB и clears AES key.
- Path управляется macros: `OPTIONX_DATA_PATH`, `OPTIONX_DB_PATH`,
  `OPTIONX_SESSION_DB_FILE`.

## Backward Compatibility

Сохраняй совместимость в:

- string values enums и JSON conversion;
- public aggregate includes;
- DTO field names в `NLOHMANN_DEFINE_TYPE_INTRUSIVE`;
- callback signatures с `std::unique_ptr<TradeRequest>` /
  `std::unique_ptr<TradeResult>`;
- event names/type identity.

Если compatibility нужно нарушить, документируй это в PR/commit и добавляй
минимальный migration note.

## Старые Или Временные Решения

- В некоторых headers есть mojibake в комментариях и старые закомментированные
  include blocks. Не копируй это в новые файлы.
- `TradeUpPlatform` выглядит частичной реализацией: не используй ее как полный
  образец trade execution.
