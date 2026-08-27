# Platform API Guide

Практический справочник по классам `optionx_cpp`: что какой класс умеет, где
он находится в стеке и какие ограничения важны для нового кода.

## Как Выбрать Класс

| Нужно сделать | Используй | Не начинай с |
|---|---|---|
| Подключить торговую платформу в приложении | `platforms::IntradeBarPlatform` или `BaseTradingPlatform` API | Platform-specific managers |
| Получать market data | `market_data::BaseMarketDataProvider` API | Прямой доступ к price managers |
| Добавить новый broker flow | Новый `BaseTradingPlatform` descendant + managers | Большой manager со всей логикой сразу |
| Отправить сделку | `BaseTradingPlatform::place_trade(std::unique_ptr<TradeRequest>)` | Прямое создание `TradeTransactionEvent` из application code |
| Подписаться на результат сделки | `on_trade_result()` callback или event flow | Polling internal queue |
| Передать auth/session | `configure_auth(std::unique_ptr<IAuthData>)` | Прямое изменение account info |
| Добавить фоновые действия | `TaskManager` или `BaseComponent::process()` | Новый ad hoc thread loop |
| Связать components | `EventBus`/`EventMediator` events | Жесткая ссылка component -> component |
| Сохранить session | `storage::ServiceSessionDB` | Новый local encrypted store |

## `platforms::BaseTradingPlatform`

Файл: `include/optionx_cpp/platforms/common/BaseTradingPlatform.hpp`.

Роль: публичный facade и владелец общего runtime stack: `BaseAccountInfoData`,
`AccountInfoProvider`, `EventBus`, `TaskManager`, `BaseAccountInfoHandler`,
список `BaseComponent*`.

Основные методы:

| Метод | Для чего | Ограничения |
|---|---|---|
| `configure_auth(std::unique_ptr<IAuthData>)` | Отправить auth data через `AuthDataEvent` | Возвращает `false` для `nullptr`; auth применяют subscribed managers |
| `place_trade(std::unique_ptr<TradeRequest>)` | User-facing вход для сделки | В base возвращает `false`; concrete platform должна override |
| `fetch_trade_result(TradeResultQuery, callback)` | Проверить результат одной сделки | В base возвращает `false`; используется для recovery по broker trade id |
| `fetch_trade_history(request, callback)` | Выгрузить историю закрытых сделок | Возвращает `TradeHistoryResult` с `TradeRecord` records |
| `fetch_trade_history(callback)` | Выгрузить всю доступную историю | Делегирует в `TradeHistoryRequest::all()` |
| `fetch_symbol_list(...)` | Список symbols | В base возвращает `false`; реализация platform-specific |
| `connect(callback)` | Публикует `ConnectRequestEvent` | Реальное подключение делает manager |
| `disconnect(callback)` | Публикует `DisconnectRequestEvent` | Реальное отключение делает manager |
| `is_connected()` | Читает `AccountInfoType::CONNECTION_STATUS` | Источник правды - account info provider |
| `get_info<T>(...)` | Typed read из account info | Нужен корректный ожидаемый тип |
| `on_trading_condition()` | Подписаться на изменения payout/лимитов/доступности торговли | Возвращает `TradingConditionUpdate` |
| `run(bool start_worker_thread = true)` | Добавляет initialize и loop tasks | `run(false)` требует ручного `process()` |
| `process()` | Один тик task manager | Используй при внешнем event loop |
| `shutdown()` | Остановить tasks/components и drain events | Idempotent; вызывается в destructor |
| `event_bus()` | Доступ к внутренней шине | Не используй для обхода готовых facade методов без причины |
| `register_component(BaseComponent*)` | Включить component в lifecycle | Следи, чтобы lifetime component был дольше регистрации |
| `platform_type()` | Platform identity | Pure virtual |

Особенности:

- `run()` не делает blocking loop сам, если `start_worker_thread=false`.
- Periodic task имеет период `1` ms и вызывает `m_event_bus.process()`, затем
  `component->process()`, затем `on_loop()`.
- `shutdown()` сначала останавливает `TaskManager`, потом вызывает
  `component->shutdown()`, потом `m_event_bus.drain()`.
- Callback `on_trade_result` в trading base возвращает static null callback.
  Market-data callbacks (`on_tick_data`, `on_bar_data`, `on_market_data_status`)
  живут на `market_data::BaseMarketDataProvider`.

## `market_data::BaseMarketDataProvider`

Файл: `include/optionx_cpp/market_data/BaseMarketDataProvider.hpp`.

Роль: public role для endpoints, которые умеют отдавать live ticks, live bars
или historical bars. Торговая платформа может одновременно наследоваться от
`BaseTradingPlatform` и `BaseMarketDataProvider`, но market-data contract
остается отдельным: его можно реализовать и у источника котировок без торговых
операций.

Основные методы:

| Метод | Для чего | Ограничения |
|---|---|---|
| `on_tick_data()` | Callback для live tick batches | Получает `std::unique_ptr<TickDataBatch>` |
| `on_bar_data()` | Callback для live bar batches | Получает `std::unique_ptr<BarDataBatch>` |
| `on_market_data_status()` | Callback для stream lifecycle/status | Status события не смешиваются с data batches |
| `subscribe_ticks(request, callback)` | Запросить live tick stream | Request type отдельный от bars |
| `subscribe_bars(request, callback)` | Запросить live bar stream | `timeframe` в секундах и должен быть > 0 |
| `apply_subscriptions(batch, callback)` | Атомарно применить набор subscribe/unsubscribe изменений | Одиночные helpers являются wrappers над batch |
| `unsubscribe(handle, callback)` | Остановить live stream | Handle должен принадлежать этому provider instance |
| `fetch_bar_history(request, callback)` | Запросить исторические бары | Возвращает `BarHistoryResult`, а не пустой массив при ошибке |

Subscription rules:

- `ProviderInstanceId` and `SubscriptionId` are runtime IDs, not storage IDs.
- `MarketDataSubscriptionHandle` is valid only for the provider instance that
  created it. This prevents unsubscribing from a different provider object by
  accident.
- `MarketDataSubscriptionResult::status` is the source of truth. Use
  `result.success()` or `if (result)`.
- `SUBSCRIBED` confirms desired state and handle ownership. It does not mean
  the physical transport has already reached the source; stream readiness is
  reported separately as `READY` through `on_market_data_status()`.
- `on_market_data_status()` is a stream-level status event bus, not a
  per-subscription state snapshot. Late subscribers should not assume they will
  receive a replayed `READY` event if the underlying source was already ready.
- Data callbacks receive batches. Shared metadata (`symbol`, `timeframe`,
  digits and subscription handle) lives on the batch; compact payload flags live
  on `Tick` and `Bar`.
- Data callbacks are delivered by the provider/platform lifecycle. With the
  default worker loop this happens automatically after `run()`. If you use
  `run(false)` or an external event loop, call `platform.process()`; draining
  `event_bus()` directly is not enough to flush public market-data batches.
- Use `MarketDataFlags` for realtime/history/backfill markers and
  `MarketPriceType` for bid/ask/mid/last payload identity.
- Live bar streams can deliver several `INCOMPLETE` snapshots with the same
  `(provider_id, subscription_id, symbol, timeframe, time_ms)` key before the
  final `FINALIZED` snapshot. Treat them as upserts, not append-only candles.
- Tick-driven bar streams finalize the current bar only when a tick from the next
  timeframe bucket arrives. Timer/process-based finalization is tracked as
  future work.
- `MarketDataContinuityService` routes recovered historical bars into the same
  `BarDataBatch` pipeline and marks them as `HISTORICAL`/`BACKFILL`.
- `BaseMarketDataProvider` is non-copyable and non-movable so provider identity
  cannot be duplicated after handles were issued.
- Public subscriptions describe consumer routing. Internal platform polling or
  websocket feeds may still run for trade lifecycle needs even when there are
  no public subscribers.

## `market_data::MarketDataRouter`

Файл: `include/optionx_cpp/market_data/MarketDataRouter.hpp`.

`MarketDataRouter` owns public provider subscriptions and routes each batch or
status only to the subscriber associated with that concrete subscription. Use it
when a bot or chart needs RAII lifetime and per-subscription status replay; use
`MarketDataHub` when simple stream-level broadcast without subscription
ownership is sufficient.

```cpp
market_data::MarketDataRouter router;
auto bot = std::make_shared<MyBot>();

auto eur = router.subscribe_ticks(
    platform,
    bot,
    market_data::TickSubscriptionRequest("EURUSD"));
```

The returned `MarketDataRouterSubscription` is move-only. Its destructor,
`reset()`, and `unsubscribe()` release the provider subscription. Subscriber
objects are weakly held, while the provider must outlive the router and pending
provider operations. Do not bind a `MarketDataHub` or assign the provider's
live-data callbacks while Router routes for that provider are active.

## Concrete Platforms

### `platforms::IntradeBarPlatform`

Файл: `include/optionx_cpp/platforms/IntradeBarPlatform.hpp`.

Собирает:

- `intrade_bar::HttpClientComponent`
- `intrade_bar::RequestManager`
- `intrade_bar::TradeExecutionComponent`
- `intrade_bar::AuthManager`
- `intrade_bar::BalanceManager`
- `intrade_bar::PriceManager`
- `intrade_bar::BtcPriceManager`
- `intrade_bar::TradeManager`

Методы:

- `place_trade()` делегирует в `m_trade_execution.place_trade(...)`.
- `fetch_trade_result()` делегирует в `m_trade_manager.fetch_trade_result(...)`.
- `fetch_trade_history()` делегирует в `m_trade_manager.fetch_trade_history(...)`.
- `platform_type()` возвращает `PlatformType::INTRADE_BAR`.

Market data:

- `subscribe_ticks()` and `unsubscribe()` delegate to
  `intrade_bar::MarketDataSubscriptionManager`.
- FX websocket subscriptions use `intrade_bar::FxPriceWebSocketManager` and
  the broker `/fxconnect` endpoint. Intrade accepts one FX symbol per websocket,
  so the manager keeps a refcounted desired stream per normalized symbol and
  opens physical sockets only when the platform is connected.
- BTCUSDT ticks continue to come from `intrade_bar::BtcPriceManager` through
  the broker `/bapi` stream.
- `intrade_bar::PriceManager` still owns HTTP polling snapshots. Polling is
  intentionally left in place because trade lifecycle code can need current
  prices even when no public market-data subscription exists.

Используй эту платформу как основной working example для новой реализации.

### `platforms::TradeUpPlatform`

Файл: `include/optionx_cpp/platforms/TradeUpPlatform.hpp`.

Собирает:

- `tradeup::HttpClientComponent`
- `tradeup::AuthManager`
- `tradeup::BalanceManager`

Ограничения:

- `place_trade()` сейчас явно возвращает `false`.
- `platforms.hpp` не подключает `TradeUpPlatform.hpp` по умолчанию.
- WebSocket manager существует в папке платформы, но не включен в facade
  composition в текущем header.

Считай TradeUp частичной реализацией, пока задача явно не требует завершить ее.

## Base Components

### `components::BaseComponent`

Файл: `include/optionx_cpp/components/BaseComponent.hpp`.

База для manager-компонентов. Наследуется от `utils::EventMediator`.

Методы:

- `initialize()` - подготовка перед loop.
- `process()` - periodic тик.
- `shutdown()` - cleanup.
- `on_event(const utils::Event* const)` - raw listener hook, по умолчанию no-op.

Для нового manager наследуйся от `BaseComponent`, если нет более точного base
class. Подписки на typed events обычно делай через `subscribe<EventType>(...)`,
а не через ручной `dynamic_cast` в `on_event`.

### `components::BaseHttpClientComponent`

Файл: `include/optionx_cpp/components/BaseHttpClientComponent.hpp`.

Роль: общий async HTTP executor поверх `kurlyk::HttpClient`.

Методы:

- `get_http_client()` - доступ к `kurlyk::HttpClient`.
- `set_max_pending_requests(size_t)` - глобальный pending request лимит kurlyk.
- `set_rate_limit_rpm/rps(rate_limit_id, value)` - protected setup limits.
- `get_rate_limit(rate_limit_id)` - получить id лимита.
- `add_http_request_task(future, callback)` - зарегистрировать future response.
- `process()` final - вызывает обработку готовых futures.
- `shutdown()` final - удаляет rate limits и отменяет requests.

Ограничения:

- Derived class не переопределяет `process()`/`shutdown()`; добавляй поведение
  через собственные методы, events и HTTP task callbacks.
- Всегда добавляй future через `add_http_request_task`, иначе response не будет
  обработан в platform loop.
- Исключения из future переводятся в `kurlyk::HttpResponse` с error state и
  логируются.

### `components::BaseTradeExecutionComponent`

Файл: `include/optionx_cpp/components/BaseTradeExecutionComponent.hpp`.

Роль: вход для торговой очереди. Делегирует account read в
`AccountInfoProvider`, state transitions в `TradeStateManager`, очередь и
callbacks в `TradeQueueManager`.

Методы:

- `set_trade_result_callback(callback)` и `on_trade_result()`.
- `place_trade(std::unique_ptr<TradeRequest>)` - validate/preprocess/add queue.
- `process()` - вызывает `m_trade_queue.process()`.
- `shutdown()` - `finalize_all_trades()`.
- `preprocess_trade_request(...)` - protected hook для platform-specific
  подготовки/валидации.
- `platform_type()` - pure virtual.

Ограничение: не отправляй trade request в нижние managers в обход
`place_trade()`, иначе можно сломать callbacks, state machine и лимиты.

## Event Bus И События

Файлы: `include/optionx_cpp/utils/pubsub/*`, `include/optionx_cpp/data/events/*`.

Классы:

- `utils::Event` - base event с `type()` и `name()`.
- `utils::EventBus` - хранит subscriptions, callbacks и async queue.
- `utils::EventMediator` - удобный base для subscribe/notify/await_once.
- `utils::EventAwaiter` - single-shot ожидание событий с auto-unsubscribe.

Методы `EventMediator`:

- `subscribe<EventType>(std::function<void(const EventType&)>)`
- `subscribe<EventType>(std::function<void(const Event* const)>)`
- `subscribe<EventType>()`
- `unsubscribe<EventType>()`, `unsubscribe_all()`
- `notify(...)`
- `notify_async(std::unique_ptr<Event>)`
- `await_once<EventType>(predicate, callback)`

Правила:

- Для межмодульной коммуникации предпочитай `notify_async`.
- Для синхронного локального notify используй только если caller/lifetime
  понятны.
- Event object после `notify(const unique_ptr&)` не должен сохраняться как raw
  pointer у получателя.
- Новый event всегда подключай в `data/events.hpp`.

## Task Scheduling

Файлы: `include/optionx_cpp/utils/tasks/Task.hpp`,
`include/optionx_cpp/utils/tasks/TaskManager.hpp`.

`TaskManager` умеет:

- `add_single_task`
- `add_delayed_task`
- `add_periodic_task`
- `add_delayed_periodic_task`
- `add_on_date_task`
- `add_periodic_on_date_task`
- именованные overloads тех же методов
- `run()`, `process()`, `shutdown()`, `force_execute()`
- `active_task_count()`, `has_active_tasks()`, `get_current_time()`

Ограничения:

- После `shutdown()` manager останавливает worker thread и выполняет финальный
  `process()`.
- Для platform code обычно не нужен отдельный `TaskManager`: используй тот,
  который уже внутри `BaseTradingPlatform`.
- Callbacks получают `std::shared_ptr<Task>`; проверяй `task->is_shutdown()` в
  long-running callbacks, как делает `BaseTradingPlatform::run()`.

## Trading DTO

### `TradeRequest`

Файл: `include/optionx_cpp/data/trading/TradeRequest.hpp`.

Поля:

- identity/meta: `symbol`, `signal_name`, `user_data`, `comment`,
  `unique_hash`, `trade_id`, `unique_id`, `account_id`
- enums: `option_type`, `order_type`, `account_type`, `currency`
- money: `amount`, `refund`, `min_payout`
- timing: `duration`, `expiry_time`

Методы:

- `add_callback(callback_t)`
- `dispatch_callbacks(request, result)`
- `create_trade_result_unique/shared()`
- `clone_unique/shared()`
- JSON serialization через `NLOHMANN_DEFINE_TYPE_INTRUSIVE`

Persistent storage:

- Use `trade_id` as the DB/callback identity and propagate it into `TradeResult::trade_id`.
- Reserve `trade_id` before broker execution through `TradeRecordDB::assign_trade_id()` or a platform
  `on_trade_id()` provider.
- Do not use `unique_id` as TradeRecordDB identity, and do not derive primary trade IDs from timestamps.

Ownership:

- Public API принимает `std::unique_ptr<TradeRequest>`.
- Events чаще держат `std::shared_ptr<TradeRequest>`/`TradeResult`.
- Для callbacks используй clone methods, если результат может уйти нескольким
  consumers.

### `TradeResult`

Смотри `include/optionx_cpp/data/trading/TradeResult.hpp`.
Используется вместе с `TradeRequest`, `TradeState`, `TradeErrorCode` и
callback/event flow.

`TradeResult` описывает результат одной сделки в lifecycle/callback API. Его
можно передать в `TradeResultQuery` как частично заполненный результат, если
бот восстановился после перезапуска и знает только broker id, amount,
account/currency и open timing/price.

### `TradeResultQuery`

Файл: `include/optionx_cpp/data/trading/TradeResultQuery.hpp`.

Используется `BaseTradingPlatform::fetch_trade_result(...)` для проверки одной
сделки по broker identity. У разных брокеров identity может быть числом,
строкой или hash; текущий Intrade Bar использует `broker_option_id`.

Это не замена `TradeRequest`: запрос на проверку результата не должен тащить
полный request, если для broker API достаточно id и минимального контекста
результата.

### `TradeHistoryRequest` / `TradeHistoryResult`

Файлы:

- `include/optionx_cpp/data/trading/TradeHistoryRequest.hpp`
- `include/optionx_cpp/data/trading/TradeHistoryResult.hpp`
- `include/optionx_cpp/data/trading/TradeRecordTimeRange.hpp`

`fetch_trade_history` возвращает историю аккаунта как `TradeRecord`, потому что
это storage/statistics model, а не callback-результат одной активной сделки.

`TradeHistoryRequest` задает:

- `start_ms`, `stop_ms`;
- `range_mode`;
- `time_field`, по умолчанию `CLOSE_DATE`;
- optional `comment`, который копируется в каждый returned `TradeRecord`.

`TradeHistoryRequest::all()` отключает client-side range filtering и означает
"выгрузить все доступное через broker source". Для broker endpoints, которые
требуют конечный date range, adapter может преобразовать это в практический
широкий диапазон.

`TradeHistoryResult` не маскирует ошибку пустым массивом: в нем отдельно есть
`success`, `status_code`, `error_desc` и `records`.

### `TradeRecord`

Файл: `include/optionx_cpp/data/trading/TradeRecord.hpp`.

`TradeRecord` - нормализованная запись сделки для storage, statistics и import
history. Он может быть создан из `TradeRequest` + `TradeResult`, а также из
broker history export.

`TradeRecord::close_date` означает planned or known option close timestamp. Для
classic options это фиксированное `TradeRequest::expiry_time`, известное до
открытия сделки. Для sprint options close time зависит от фактического
`open_date`, поэтому обычно становится известным после открытия как
`open_date + duration`.

## Account/Auth Data

Основные файлы:

- `data/account/IAuthData.hpp`
- `data/account/BaseAccountInfoData.hpp`
- `data/account/AccountInfoRequest.hpp`
- `data/account/AccountInfoUpdate.hpp`
- `platforms/IntradeBarPlatform/AuthData.hpp`
- `platforms/TradeUpPlatform/AuthData.hpp`
- `platforms/*/AccountInfoData.hpp`

Правила:

- Auth передавай через `configure_auth(std::unique_ptr<IAuthData>)`.
- Account info читай через `BaseTradingPlatform::get_info<T>()` или
  `AccountInfoProvider`.
- Account state обновляют managers через events/data structures; application
  code не должен менять platform account data напрямую.

## HTTP И Platform Helpers

Общие helpers:

- `utils/http_utils.hpp` - общая validation логика.
- `platforms/common/http_validation.hpp` - common platform validation.
- `platforms/IntradeBarPlatform/http_utils.hpp`,
  `platforms/TradeUpPlatform/http_utils.hpp` - platform aliases/helpers.
- `platforms/*/http_parsers.hpp` - parsing конкретного broker API.

Правило выбора:

- Универсальное и broker-independent - в `utils`.
- Общее для всех platforms - в `platforms/common`.
- Привязано к JSON/HTML/API конкретного broker - в папку этой платформы.

## Storage And Bridge

### `storage::ServiceSessionDB`

Файл: `include/optionx_cpp/storages/ServiceSessionDB.hpp`.

Методы:

- `get_instance()`
- `set_key(key)`
- `uses_default_key()`
- `has_custom_key()`
- `get_session_value(platform, email)`
- `set_session_value(platform, email, value)`
- `remove_session(platform, email)`
- `clear()`
- `shutdown()`

Ограничения:

- Singleton, copy запрещен.
- Данные шифруются AES и кодируются Base64.
- Built-in default key предназначен для разработки/tests. Production code
  должен вызвать `set_key(key)` и может проверить состояние через
  `has_custom_key()`.
- IV, generated AES keys and `SecureKey` masks use direct operating-system
  randomness (`BCryptGenRandom` on Windows, `/dev/urandom` on Unix-like
  systems) instead of `std::random_device`; the default key is still a shared
  fallback and does not provide production-grade secret isolation by itself.
- Path задается macros `OPTIONX_DATA_PATH`, `OPTIONX_DB_PATH`,
  `OPTIONX_SESSION_DB_FILE`.
- Методы защищены mutex; не обходи сервис прямым доступом к mdbx table.

### `bridges::BaseBridge`

Файл: `include/optionx_cpp/bridges/BaseBridge.hpp`.

Контракт:

- `configure(std::unique_ptr<IBridgeConfig>)`
- callbacks: `on_status_update()`, `on_place_trade()`, `on_trade_result()`
- `update_account_info(const AccountInfoUpdate&)`
- `run()`
- `shutdown()`

Bridge должен быть adapter между внешней системой и DTO/callback API, а не
второй реализацией platform internals.

## Enum Stack

Файл-пример: `include/optionx_cpp/data/trading/enums.hpp`.

Важные enums:

- `PlatformType`
- `BridgeType`
- `AccountType`
- `OptionType`
- `OrderType`
- `CurrencyType`
- `TradeState`
- `TradeErrorCode`
- `MmSystemType`

Стиль:

- `enum class`.
- `to_str(value, mode)` где нужны разные форматы.
- `to_enum(str, value) noexcept` + throwing specialization
  `to_enum<T>(str)`.
- `to_json`/`from_json`.
- `operator<<`.

Не меняй существующие строковые значения без проверки backward compatibility.

## Компактные Примеры

### Новый Event

```cpp
namespace optionx::events {

class MyEvent : public utils::Event {
public:
    int value = 0;

    explicit MyEvent(int value) : value(value) {}

    std::type_index type() const override { return typeid(MyEvent); }
    const char* name() const override { return "MyEvent"; }
};

} // namespace optionx::events
```

### Новый Component

```cpp
class MyManager final : public components::BaseComponent {
public:
    explicit MyManager(utils::EventBus& bus) : BaseComponent(bus) {
        subscribe<events::MyEvent>([this](const events::MyEvent& event) {
            m_last_value = event.value;
        });
    }

    void process() override {
        // periodic work from platform loop
    }

private:
    int m_last_value = 0;
};
```

### Расширение Trade Execution

```cpp
class MyTradeExecution final : public components::BaseTradeExecutionComponent {
public:
    MyTradeExecution(utils::EventBus& bus,
                     std::shared_ptr<BaseAccountInfoData> account_info)
        : BaseTradeExecutionComponent(bus, std::move(account_info)) {}

private:
    bool preprocess_trade_request(
            std::unique_ptr<TradeRequest>& request,
            std::unique_ptr<TradeResult>& result) override {
        if (!request || request->symbol.empty()) {
            result->error_code = TradeErrorCode::INVALID_SYMBOL;
            return false;
        }
        return true;
    }

    PlatformType platform_type() const override {
        return PlatformType::SIMULATOR;
    }
};
```
