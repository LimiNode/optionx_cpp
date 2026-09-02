# Codebase Orientation

Практическая карта проекта для AI-агентов. Цель - быстро понять, где искать
код, какие зависимости допустимы и где безопасно добавлять новый функционал.

## Project Map

| Путь | Назначение | Когда редактировать |
|---|---|---|
| `include/optionx_cpp/*.hpp` | Публичные aggregate include-точки | Когда новый публичный header должен стать частью API |
| `include/optionx_cpp/data` | DTO, enums, events, account/symbol/tick/bar/trading data | При изменении API сообщений или доменной модели |
| `include/optionx_cpp/components` | Base lifecycle components и общие managers trade execution | При добавлении общего поведения для платформ |
| `include/optionx_cpp/platforms/common` | Platform facade/base helpers | Только для общих правил всех платформ |
| `include/optionx_cpp/platforms/IntradeBarPlatform` | Реализация Intrade Bar managers/http/parsers | При изменении Intrade Bar API flow |
| `include/optionx_cpp/platforms/TradeUpPlatform` | Реализация TradeUp managers/http/ws | При изменении TradeUp flow |
| `include/optionx_cpp/utils` | Общие утилиты: pub-sub, tasks, crypto, strings, time, HTTP | При добавлении reusable инфраструктуры |
| `include/optionx_cpp/storages` | Session storage на mdbx + AES | Только для session persistence/security изменений |
| `include/optionx_cpp/bridges` | Base bridge контракт | При интеграции внешних систем |
| `examples` | Ручные usage samples | Когда меняется user-facing flow |
| `tests` | GoogleTest/probes | Для регрессионных проверок |
| `external` | Subcomponents/dependency CMake scripts | Только при изменении third-party build/deps |
| `build`, `build-*` | Generated build output | Обычно не редактировать и не коммитить |

## Public Include Points

Главная include-точка - `include/optionx_cpp/optionx.hpp`. Она включает
`utils.hpp`, `lifecycle.hpp`, `data.hpp`, `storages.hpp`, `components.hpp`,
`platforms.hpp`, `bridges.hpp`.

Aggregate headers в корне `include/optionx_cpp` - часть публичной поверхности.
Если добавляешь новый публичный DTO/component/platform, проверь соответствующий
aggregate header. Если код нужен только внутреннему manager, не расширяй
публичный include без необходимости.

## DDD Слои И Namespace

| Слой | Namespace | Правило зависимости |
|---|---|---|
| `data` | в основном `optionx`, events в `optionx::events` | Не должен зависеть от platform managers |
| `utils` | `optionx::utils`, crypto частично `optionx::crypto` | Reusable infrastructure, не зависит от платформ |
| `components` | `optionx::components` | Может зависеть от `data`/`utils`, не должен знать детали конкретной платформы |
| `platforms/common` | `optionx::platforms` | Собирает components и platform lifecycle |
| platform implementations | `optionx::platforms::intrade_bar`, `optionx::platforms::tradeup` | Может зависеть от components/data/utils/storage |
| `storage` | `optionx::storage` | Может использовать utils/crypto и mdbx-containers |
| `bridges` | `optionx::bridges` | Контракт для внешних adapters, работает через DTO/callbacks |

Предпочтительное направление зависимостей:

`utils` <- `data` <- `components` <- `platforms` <- application/bridge.

Избегай обратных зависимостей: DTO не должны включать platform manager,
`components` не должны напрямую знать `intrade_bar::RequestManager`, а `utils`
не должны зависеть от trading platform API.

## Где Искать Точки Входа

| Что ищешь | Файлы |
|---|---|
| User-facing platform API | `platforms/common/BaseTradingPlatform.hpp` |
| Intrade Bar composition | `platforms/IntradeBarPlatform.hpp` |
| TradeUp composition | `platforms/TradeUpPlatform.hpp` |
| Base component lifecycle | `components/BaseComponent.hpp` |
| HTTP async queue | `components/BaseHttpClientComponent.hpp` |
| Trade request queue/state | `components/BaseTradeExecutionComponent.hpp`, `components/BaseTradeExecutionComponent/*` |
| Pub-sub contracts | `utils/pubsub/*.hpp`, `data/events/*.hpp` |
| Task scheduling | `utils/tasks/Task.hpp`, `utils/tasks/TaskManager.hpp` |
| Account info access | `data/account/BaseAccountInfoData.hpp`, `components/BaseTradeExecutionComponent/AccountInfoProvider.hpp` |
| Session storage | `storages/ServiceSessionDB.hpp` |

## Используемые Паттерны

| Паттерн | Где виден | Как переиспользовать |
|---|---|---|
| Facade | `BaseTradingPlatform`, concrete platforms | Новый user-facing API добавляй на facade и делегируй в component/manager |
| Publish-subscribe | `EventBus`, `EventMediator`, `data/events` | Связывай components событиями, а не прямыми вызовами между managers |
| Component lifecycle | `BaseComponent::initialize/process/shutdown` | Любой long-running manager должен вписаться в lifecycle |
| Manager composition | `IntradeBarPlatform` private manager fields | Platform собирает managers, но бизнес-логику держит внутри managers |
| Strategy/override hook | `BaseTradeExecutionComponent::preprocess_trade_request` | Platform-specific validation/preprocess делай override |
| Queue/state managers | `TradeQueueManager`, `TradeStateManager` | Trade lifecycle меняй через эти классы, не обходи queue |
| Adapter/bridge | `BaseBridge` | Внешнюю интеграцию подключай через callbacks и DTO |
| Singleton storage | `ServiceSessionDB::get_instance()` | Для sessions используй существующий сервис, не создавай вторую DB |

## Механики Расширения

### Новый DTO

1. Положи header в `include/optionx_cpp/data/<domain>/`.
2. Используй namespace как у соседних DTO.
3. Для сериализуемых объектов добавь `NLOHMANN_DEFINE_TYPE_INTRUSIVE`.
4. Подключи файл в domain aggregate header, например `data/trading.hpp`.
5. Если DTO становится общепубличным, проверь `data.hpp`.

### Новый Enum

1. Добавляй enum рядом с доменом, чаще в `data/<domain>/enums.hpp`.
2. Сохраняй стиль: `enum class`, `UNKNOWN`/нулевое значение где уместно,
   `to_str`, `to_enum`, specialization `to_enum<T>`, JSON conversion.
3. Не меняй существующие string values без причины: они могут быть частью JSON
   compatibility.

### Новый Event

1. Добавь `data/events/<Name>Event.hpp`.
2. Наследуйся от `utils::Event`.
3. Реализуй `type()` через `typeid(<Name>Event)` и `name()` как строку класса.
4. Подключи в `data/events.hpp`.
5. Используй `notify_async(std::make_unique<...>)` для межмодульного async
   сообщения и `subscribe<EventType>(...)` в manager/component.

### Новый Component/Manager

1. Наследуйся от `components::BaseComponent` или подходящего base class.
2. В конструктор принимай `utils::EventBus&` либо ссылку на platform facade,
   если так делают соседние managers.
3. Подпишись на events в конструкторе или initialize, следуя ближайшему
   platform manager.
4. Работу, которая должна тикать, делай в `process()`.
5. Cleanup/cancel делай в `shutdown()` и деструкторе, если есть внешние ресурсы.
6. Зарегистрируй manager в concrete platform, если он должен участвовать в
   lifecycle.

### Новая Platform Implementation

1. Создай `platforms/<Name>Platform.hpp` и папку `platforms/<Name>Platform/`.
2. Унаследуй facade от `platforms::BaseTradingPlatform`.
3. Передай platform-specific `AccountInfoData` в base constructor.
4. Собери HTTP/auth/balance/trade managers как private fields.
5. Реализуй `platform_type()` и только реально поддерживаемые public методы.
6. Добавь `PlatformType` и conversions в `data/trading/enums.hpp`.
7. Подключай в `platforms.hpp` только если это готовая публичная платформа.

## Do / Avoid

Do:

- Используй `BaseTradingPlatform::event_bus()` и `EventMediator` для
  коммуникации между components.
- Добавляй reusable helpers в `utils`, если они не знают про конкретный broker.
- Расширяй platform-specific `http_parsers.hpp`/`http_utils.hpp`, если логика
  привязана к конкретному API.
- Поддерживай header-only стиль и include guards `#pragma once` + macro guard.

Avoid:

- Не создавай второй event loop рядом с `TaskManager`.
- Не вызывай private managers платформы из внешнего API напрямую.
- Не меняй aggregate headers для внутренних деталей.
- Не добавляй platform-specific include в `utils` или `data`.
- Не обходи `TradeQueueManager` при изменении trade lifecycle.

## Обычно Не Менять Без Необходимости

| Путь | Почему осторожно |
|---|---|
| `include/optionx_cpp/optionx.hpp` | Главный публичный include; изменение влияет на всех пользователей |
| `include/optionx_cpp/data/trading/enums.hpp` | JSON/string compatibility и enum indexes |
| `include/optionx_cpp/platforms/common/BaseTradingPlatform.hpp` | Общий lifecycle всех платформ |
| `include/optionx_cpp/components/BaseTradeExecutionComponent/*` | Trade state/queue invariants |
| `include/optionx_cpp/utils/pubsub/*` | Центральная доставка событий и awaiters |
| `include/optionx_cpp/utils/tasks/*` | Threading/lifecycle всех платформ |
| `include/optionx_cpp/storages/ServiceSessionDB.hpp` | Encryption key/session persistence |
| `external/*` | Third-party build/subcomponents |
