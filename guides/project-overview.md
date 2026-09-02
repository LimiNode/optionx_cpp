# Project Overview

Документ дает короткую модель `optionx_cpp`: что это за библиотека, какие
публичные include-точки есть и через какие классы обычно проходит сценарий
торговой платформы.

## Назначение

`optionx_cpp` - header-only C++17 библиотека для подключения торговых систем,
broker API и bridges. Основной сценарий: пользователь создает платформу,
настраивает auth data, подписывает callbacks, запускает lifecycle, отправляет
`TradeRequest`, восстанавливает результат одной сделки через
`fetch_trade_result` или выгружает историю аккаунта через
`fetch_trade_history`.

Опорные файлы:

- `include/optionx_cpp/optionx.hpp`
- `include/optionx_cpp/platforms/common/BaseTradingPlatform.hpp`
- `include/optionx_cpp/platforms/IntradeBarPlatform.hpp`
- `include/optionx_cpp/components/BaseTradeExecutionComponent.hpp`
- `include/optionx_cpp/utils/pubsub.hpp`
- `include/optionx_cpp/utils/tasks.hpp`

## Public Include Surface

`optionx_cpp/lifecycle.hpp` exposes the optional `ILifecycleModule` and
`LifecycleStack` API for applications that compose platform, Router, bot, and
node lifecycles. See [English](lifecycle-stack.md) or
[Русский](lifecycle-stack.ru.md).

| Include | Что открывает | Когда использовать |
|---|---|---|
| `optionx_cpp/optionx.hpp` | Все основные subsystems | В приложениях и examples, когда нужен полный API |
| `optionx_cpp/utils.hpp` | Pub-sub, tasks, crypto, strings, time, ids | Для инфраструктурного кода и новых components |
| `optionx_cpp/lifecycle.hpp` | `ILifecycleModule`, `LifecycleStack` | Для необязательной общей обработки и staged shutdown модулей |
| `optionx_cpp/data.hpp` | DTO, events, enums, account/symbol/tick/bar/trading data | Для API boundary и сообщений |
| `optionx_cpp/components.hpp` | `BaseComponent`, HTTP и trade execution base classes | Для нового manager/component |
| `optionx_cpp/market_data.hpp` | Market-data provider role, subscription DTOs and statuses | Для live tick/bar subscriptions и history API contracts |
| `optionx_cpp/platforms.hpp` | Base platform и Intrade Bar platform | Для клиентского кода платформ |
| `optionx_cpp/storages.hpp` | `ServiceSessionDB` | Для session storage |
| `optionx_cpp/bridges.hpp` | `BaseBridge` | Для внешних bridge integrations |

Внутренние файлы из `include/optionx_cpp/platforms/<Platform>/...` подключай
только при расширении конкретной платформы.

## Главные Домены

| Домен | Namespace / пути | Роль |
|---|---|---|
| Platform facade | `optionx::platforms`, `include/optionx_cpp/platforms` | Публичный API платформы: connect, auth, trades, account info |
| Components/managers | `optionx::components`, platform subnamespaces | Lifecycle-компоненты, которые получают events и выполняют работу |
| Trading data | `optionx`, `include/optionx_cpp/data/trading` | `TradeRequest`, `TradeResult`, enums, signals |
| Market data | `optionx::market_data`, `include/optionx_cpp/market_data`, `data/bars`, `data/ticks`, `data/symbol` | Live subscriptions, history requests/results, ticks, bars and symbols |
| Events | `optionx::events`, `include/optionx_cpp/data/events` | Pub-sub контракты между components |
| Infrastructure | `optionx::utils` | EventBus, tasks, crypto, ids, HTTP helpers |
| Storage | `optionx::storage` | AES + mdbx session storage |
| Bridges | `optionx::bridges` | Интеграция внешних систем с platform/trading API |

## Runtime Stack

Типовой поток платформы основан на `BaseTradingPlatform`:

1. Concrete platform (`IntradeBarPlatform`) создает общий `EventBus`,
   `TaskManager`, `BaseAccountInfoData` и platform managers.
2. Managers регистрируются как `BaseComponent`/`EventMediator` и общаются через
   events из `include/optionx_cpp/data/events`.
3. `run(true)` добавляет single task `initialize` и periodic task `loop`;
   worker thread вызывает `EventBus::process()` и `component->process()`.
4. `configure_auth()` публикует `AuthDataEvent`; `connect()`/`disconnect()`
   публикуют request events.
5. `place_trade()` у конкретной платформы делегирует в
   `BaseTradeExecutionComponent`, который использует `TradeQueueManager` и
   `TradeStateManager`.
6. `fetch_trade_result()` и `fetch_trade_history()` у конкретной платформы
   делегируются в platform managers и возвращают DTO через callbacks.
7. Market-data entry points (`fetch_bar_history`, `subscribe_ticks`,
   `subscribe_bars`, `unsubscribe`) are exposed by `BaseMarketDataProvider`
   implementations. Concrete platforms may use HTTP polling, websockets, or
   both internally; public subscription callbacks report desired-state
   acceptance, while physical stream readiness is reported through status
   callbacks.
8. `shutdown()` останавливает tasks, вызывает shutdown у components, затем
   draining event bus.

## Реальные Платформы

| Класс | Статус | Что умеет |
|---|---|---|
| `platforms::IntradeBarPlatform` | Основная реализация | Auth, balance, price/BTC price, request manager, trade execution, trade manager |
| `platforms::TradeUpPlatform` | Минимальная реализация | Auth, balance, HTTP client; `place_trade()` сейчас возвращает `false` |
| `platforms::BaseTradingPlatform` | Facade/base | Callbacks, auth/connect/disconnect events, account info provider, run/process/shutdown |

`platforms.hpp` сейчас подключает `IntradeBarPlatform`, а `TradeUpPlatform`
закомментирован. Это важный compatibility signal: не считай TradeUp публичной
include-точкой верхнего уровня без проверки задачи.

## Data Model

DTO и events обычно открытые классы/структуры с public fields и
`NLOHMANN_DEFINE_TYPE_INTRUSIVE`, если объект сериализуется. Примеры:

- `data/trading/TradeRequest.hpp`
- `data/trading/TradeResult.hpp`
- `data/bars/Bar.hpp`, `SingleBar.hpp`, `BarSequence.hpp`
- `data/ticks/Tick.hpp`, `SingleTick.hpp`, `TickSequence.hpp`
- `market_data/MarketDataBatch.hpp`, `MarketDataSubscription.hpp`
- `data/account/BaseAccountInfoData.hpp`
- `data/events/TradeRequestEvent.hpp`

Enums живут рядом с доменом и имеют `to_str`, `to_enum`, JSON conversion и
часто `operator<<`. Пример: `data/trading/enums.hpp`.

## Error Model

- Торговые ошибки представлены `TradeErrorCode` и состояниями `TradeState` в
  `data/trading/enums.hpp`.
- HTTP/network errors обычно остаются в `kurlyk::HttpResponse` и проверяются
  helpers из `utils/http_utils.hpp` и platform-specific `http_utils.hpp`.
- Storage ловит `mdbxc::MdbxException` и `std::exception`, логирует и возвращает
  `false`/`std::nullopt`.
- В callbacks и components чаще используется "return false + event/result", а не
  исключение наружу.

## Quick Start Для Агента

| Задача | Сначала открыть |
|---|---|
| Добавить новую платформу | `BaseTradingPlatform.hpp`, `IntradeBarPlatform.hpp`, `guides/platform-api-guide.md` |
| Добавить manager/component | `BaseComponent.hpp`, ближайший manager в platform folder |
| Добавить событие | `data/events/*.hpp`, `utils/pubsub/Event.hpp` |
| Добавить DTO/enum | `data/<domain>/*.hpp`, `data/trading/enums.hpp` |
| Изменить HTTP flow | `BaseHttpClientComponent.hpp`, platform `HttpClientComponent.hpp`, `http_utils.hpp` |
| Изменить trade lifecycle | `BaseTradeExecutionComponent.hpp`, `TradeQueueManager.hpp`, `TradeStateManager.hpp` |
| Изменить build/tests | `CMakeLists.txt`, `external/`, `tests/`, `guides/build-and-test.md` |
