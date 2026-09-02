# Руководство По Market Data Router

Язык: [English](market-data-router.md) | **Русский**

Это русский перевод канонического руководства по
`MarketDataRouter`, регистрации провайдеров, доставке в контексте конкретной
подписки, replay статусов и интеграции с потоками ботов. Он обновляется вместе с
канонической английской версией: обе версии меняются в одном pull request.

## Назначение

`MarketDataRouter` связывает публичные подписки на рыночные данные, которыми
владеет `BaseMarketDataProvider`, с конкретными объектами
`IMarketDataSubscriber`. Он решает три связанные задачи:

- подписчик получает только тики, бары и статусы своих маршрутов;
- каждое событие содержит дескриптор конкретной подписки провайдера;
- приложение может выбирать провайдеров по стабильным числовым ID или строковым
  aliases, не сохраняя указатели на конкретные типы провайдеров внутри ботов.

Используй `MarketDataRouter`, когда приложению нужно владение подписками и
доставка в контексте конкретной подписки. `MarketDataHub` предназначен для
fan-out уже существующего потока, когда hub не должен создавать подписки
провайдера и владеть ими. Оба объекта используют три live callback slots
провайдера, поэтому их нельзя одновременно привязать к одному провайдеру.

Публичная точка подключения:

```cpp
#include <optionx_cpp/market_data.hpp>

namespace md = optionx::market_data;
```

Поддерживаемые запускаемые примеры:

- `examples/market_data_router_example.cpp` для явного владения Router handles;
- `examples/market_data_subscriber_base_example.cpp` для самостоятельно
  подписывающегося бота и cross-thread owner-loop dispatch.

## Объектная Модель

| Тип | Владелец и смысл | Стабильность |
|---|---|---|
| `BaseMarketDataProvider` | Физический источник, реализующий subscribe, unsubscribe и live callbacks | Время жизни одного объекта |
| `ProviderInstanceId` | Runtime identity, создаваемый объектом провайдера | Только текущий runtime |
| `MarketDataProviderId` | Ненулевой ключ провайдера, назначенный приложением | Стабилен, если приложение сохраняет его в конфигурации |
| Provider alias | Точная регистрозависимая строка, связанная с `MarketDataProviderId` | Стабилен, если приложение сохраняет его в конфигурации |
| `SubscriptionId` | ID физической подписки, назначенный провайдером | Определяется провайдером |
| `RoutedSubscriptionId` | ID логического маршрута, назначенный одним Router | Время жизни одного Router |
| `MarketDataSubscriptionHandle` | Дескриптор конкретной подписки провайдера в батчах и статусах | Валиден после принятия подписки провайдером |
| `MarketDataRouterSubscription` | Move-only RAII-владелец pending или active маршрута | До move, reset, unsubscribe или shutdown Router |

Не подменяй один идентификатор другим. В частности:

- `MarketDataProviderId` подходит для конфигурации бота, а
  `ProviderInstanceId` — для связывания объектов в текущем процессе;
- `RoutedSubscriptionId` идентифицирует lifetime state внутри Router;
- `MarketDataSubscriptionHandle` идентифицирует подписку провайдера, видимую в
  событиях рыночных данных.

Default-constructed strong Router IDs невалидны. Проверяй `valid()` или явное
преобразование в `bool`; публичная invalid-sentinel константа не нужна.

## Регистрация Провайдера

Регистрация необязательна. Overloads с прямой ссылкой на провайдера остаются
доступны, но зарегистрированные IDs и aliases отделяют подписчика от wiring
приложения:

```cpp
md::MarketDataRouter router;
md::MarketDataProviderId intrade_id{1001};

const bool registered = router.register_provider(
    intrade_id,
    intrade_platform,
    {"intrade", "primary-options"});
```

Контракт регистрации:

- Router хранит non-owning указатель. Провайдер должен жить дольше регистрации,
  всех pending операций провайдера и shutdown Router.
- Числовой ID, instance провайдера и каждый alias должны быть уникальны внутри
  одного Router. Пустые aliases и нулевые IDs отклоняются.
- Aliases точные и регистрозависимые. `"Intrade"` и `"intrade"` — разные ключи.
- Регистрация не открывает websocket и не захватывает callback slots. Router
  лениво привязывается при создании первого маршрута этого провайдера.
- `unregister_provider()` успешен только тогда, когда у регистрации нет pending
  или active маршрутов.
- `registered_provider_id(runtime_id)` преобразует runtime identity из события
  обратно в стабильный ID приложения, пока регистрация существует.

Способ выбора меняет только поиск провайдера. Эти вызовы используют одну и ту же
реализацию маршрутизации:

```cpp
auto direct = router.subscribe_ticks(provider, bot, request);
auto by_id = router.subscribe_ticks(intrade_id, bot, request);
auto by_alias = router.subscribe_ticks("intrade", bot, request);
```

У маршрута, созданного через прямую ссылку, `registered_provider_id()`
невалиден, потому что стабильная регистрация не выбиралась.

## Явный Subscriber API

Получатель реализует только нужные каналы:

```cpp
class Chart final : public optionx::market_data::IMarketDataSubscriber {
public:
    void on_tick_data(
            const optionx::market_data::TickDataBatch& batch) override {
        // batch.subscription идентифицирует маршрут этого графика.
    }

    void on_market_data_status(
            const optionx::market_data::MarketDataStatusUpdate& update) override {
        // update.subscription идентифицирует маршрут этого статуса.
    }
};
```

Router хранит subscribers как `weak_ptr`. Вызывающая сторона удерживает объект:

```cpp
auto chart = std::make_shared<Chart>();

auto eur = router.subscribe_ticks(
    "intrade",
    chart,
    md::TickSubscriptionRequest(
        "EURUSD",
        md::MarketDataTransport::WEBSOCKET));

if (!eur.valid()) {
    // Router не смог создать pending маршрут.
} else if (eur.pending()) {
    // Маршрут есть, но провайдер ещё не подтвердил подписку.
} else if (eur.active()) {
    const auto provider_subscription = eur.provider_subscription();
}
```

`MarketDataRouterSubscription` владеет lifetime маршрута. Move передаёт
владение; `unsubscribe()`, `reset()`, destructor или shutdown Router освобождают
его. Handle должен жить всё время, пока нужен поток.

### Ошибка Физического Cleanup

Логическое освобождение маршрута и физический cleanup у провайдера разделены.
Сразу после освобождения handle становится невалидным, а маршрут перестаёт
получать scoped и unscoped данные. Если provider `unsubscribe()` вернул `false`,
бросил исключение или завершился с failed result, Router сохраняет физический
`MarketDataSubscriptionHandle`, callback binding провайдера и внутреннюю cleanup
entry. `subscription_count()` включает такую сохранённую entry, а
`failed_unsubscribe_count()` отдельно считает ошибки cleanup.

Router помещает provider с failed cleanup в карантин: существующие active
маршруты продолжают работать, но новые маршруты через этого provider отклоняются
до успешной очистки. Повтор выполняется из owner loop:

```cpp
if (router.failed_unsubscribe_count() != 0) {
    const auto accepted = router.retry_failed_unsubscribes();
    // accepted считает retry-операции, принятые providers. Завершение может
    // быть асинхронным, поэтому затем снова проверь failed_unsubscribe_count().
}
```

Исходный unsubscribe callback получает ошибку один раз; внутренний retry не
восстанавливает уже использованный публичный handle. Для постоянных ошибок
применяй backoff на уровне приложения. `shutdown()` повторяет cleanup entries,
которые уже один раз завершились ошибкой, но повторная ошибка оставляет Router в
состоянии draining до retry приложения или будущей явной abandon policy.

Необязательный subscription callback сообщает о принятии desired state
провайдером. Это не callback готовности транспорта:

```cpp
auto route = router.subscribe_ticks(
    intrade_id,
    chart,
    request,
    [](md::MarketDataSubscriptionResult result) {
        // SUBSCRIBED означает, что провайдер принял подписку.
        // READY отдельно придёт через on_market_data_status().
    });
```

## Самостоятельно Подписывающиеся Боты

`MarketDataSubscriberBase` объединяет `IMarketDataSubscriber` и внутреннее
хранилище move-only Router handles. Производный бот может подписываться из своих
методов, не передавая handles в composition root:

```cpp
class TradingBot final : public md::MarketDataSubscriberBase {
public:
    TradingBot(md::MarketDataRouter& router, md::MarketDataProviderId provider_id)
        : MarketDataSubscriberBase(router),
          m_provider_id(provider_id) {}

    bool request_start() {
        return post_subscribe_ticks(
            m_provider_id,
            md::TickSubscriptionRequest(
                "EURUSD",
                md::MarketDataTransport::WEBSOCKET),
            [this](md::RoutedSubscriptionId route) {
                m_eur_route = route;
            });
    }

    bool request_stop() {
        return post_unsubscribe(m_eur_route);
    }

    void on_tick_data(const md::TickDataBatch& batch) override {
        // При наличии dispatcher выполняется в настроенном owner loop.
    }

    void on_market_data_status(const md::MarketDataStatusUpdate& update) override {
        if (update.subscription.id == provider_subscription(m_eur_route).id) {
            // Этот статус относится к маршруту EURUSD.
        }
    }

private:
    md::MarketDataProviderId m_provider_id;
    md::RoutedSubscriptionId m_eur_route;
};
```

До вызова любого subscribe helper производный объект уже должен принадлежать
`std::shared_ptr`. Base использует `enable_shared_from_this`, чтобы передать
Router слабую ссылку на subscriber. Не добавляй ещё один
`enable_shared_from_this` в производный тип.

Синхронные `subscribe_ticks()`, `subscribe_bars()`, `unsubscribe()` и
`unsubscribe_all()` предназначены для кода, который уже выполняется в owner
loop. Варианты `post_*()` являются cross-thread API и описаны ниже.

## Доставка И Контекст Конкретной Подписки

Батчи и статусы провайдера могут быть subscription-scoped или stream-level:

- если провайдер передал валидный `MarketDataSubscriptionHandle`, Router
  отправляет событие только соответствующему active маршруту;
- если событие не привязано к подписке, Router находит подходящие active
  маршруты и создаёт отдельную доставку для каждого;
- перед вызовом subscriber Router записывает handle этого маршрута в
  `batch.subscription` или `update.subscription`.

Поэтому даже две подписки на один symbol различаются по provider subscription
ID. Subscriber должен связывать события через `subscription.provider_id` и
`subscription.id`, а не только через `symbol`.

Callbacks собираются под mutex Router и вызываются уже после его освобождения.
Router не вызывает пользовательский код, удерживая mutex контейнеров.

## Cache Статусов И Replay

Провайдеры публикуют статусы как stream-level lifecycle bus. Router добавляет
контекст подписки и replay для поздно созданного маршрута.

Рассмотрим последовательность:

1. Маршрут EURUSD привязывает Router к провайдеру.
2. Провайдер публикует `BTCUSDT / TICKS / WEBSOCKET / READY`.
3. Router сохраняет stream status, хотя маршрута BTCUSDT ещё нет.
4. Создаётся маршрут BTCUSDT, и провайдер принимает подписку.
5. Router сразу доставляет сохранённый статус, записав новый
   `MarketDataSubscriptionHandle` BTCUSDT в `update.subscription`.

Точные правила replay:

- Router хранит последний подходящий статус, а не только `READY`. Поздний
  маршрут может получить последний `CONNECTED`, `FAILED` или `DISCONNECTED`.
- Cache key состоит из provider, stream type, symbol, timeframe и transport.
  `AUTO` и `HYBRID` участвуют как совместимые selectors.
- Replay происходит только после принятия подписки провайдером, поэтому событие
  уже содержит handle конкретной подписки.
- Кэшируются только статусы. Payload тиков и баров никогда не replay-ится.
- Cache существует только пока Router привязан к провайдеру. Хотя бы один
  маршрут должен удерживать binding, чтобы Router увидел статус до создания
  нового маршрута. Удаление последнего маршрута освобождает callbacks и cache.
- Replay выполняется отдельно для каждого маршрута. Две логические подписки
  получают события со своими provider handles.

Это lifecycle replay, а не восстановление исторических рыночных данных.
Предварительная история и заполнение разрывов относятся к
`MarketDataContinuityService` и history API провайдера.

## Owner Loop И Потоки Ботов

Без owner dispatcher Router сохраняет синхронное поведение. Subscribe,
unsubscribe, provider completion и доставка должны быть заранее сериализованы
приложением в одном owner loop. Синхронный provider result и replay сохранённого
статуса могут сработать до возврата из `subscribe_*()`. Callback должен брать
конкретный `subscription` из события, а не рассчитывать, что вызывающий код уже
сохранил возвращённый route в другом поле.

Для отдельных или общих потоков ботов Router создаётся с thread-safe
dispatcher. Стандартный adapter — `BaseTradingPlatform::post_task()`:

```cpp
md::MarketDataRouter router(
    [&platform](md::MarketDataRouter::owner_task_t task) {
        return platform.post_task(std::move(task));
    });
```

Контракт dispatcher:

- его безопасно вызывать из provider, websocket и bot threads;
- дошедшие до исполнения задачи обрабатываются FIFO одним owner loop;
- задача foreign caller никогда не выполняется inline;
- dispatcher доступен до завершения cleanup subscribers и shutdown Router.

Принятие задачи очередью не гарантирует её выполнение. В частности,
`BaseTradingPlatform::post_task()` может отменить принятые задачи, оставшиеся в
очереди к моменту начала shutdown платформы.

При наличии dispatcher Router переносит provider subscription completions,
tick batches, bar batches и status updates в этот loop. Боты используют:

- `post_subscribe_ticks()` и `post_subscribe_bars()`;
- `post_unsubscribe()` и `post_unsubscribe_all()`.

Возвращаемый `bool` означает только принятие команды owner loop. Он не означает,
что Router создал маршрут, провайдер принял подписку или транспорт готов. Этапы
наблюдаются отдельно:

1. Результат `post_*()`: команда принята очередью;
2. route callback: валидный или невалидный `RoutedSubscriptionId` после
   сохранения handle;
3. provider callback: типизированный результат subscribe/unsubscribe;
4. `on_market_data_status()`: готовность и lifecycle live stream.

Если subscriber уничтожен до выполнения posted subscribe command, команда
отменяется, а её callbacks не вызываются. Когда настроенный dispatcher начинает
отклонять работу во время shutdown, новые tick, bar и status deliveries
отбрасываются, а не исполняются inline в source thread. Эти deliveries не
требуют физического cleanup и сразу удаляются.

Subscribe и unsubscribe completions записываются в состояние Router до
постановки owner task. Успешный subscribe result резервирует concrete provider
handle; owner task атомарно забирает reservation перед переводом route из pending
в active. Поэтому отклонённая или позднее отменённая owner task не владеет
единственной копией physical handle или completion result.

`process()` продвигает принадлежащую Router deferred lifecycle work в owner
thread. При обычной работе posted completion tasks применяют transitions
напрямую, поэтому Router не требует отдельной прокачки. После начала draining
через `shutdown()` поздние provider completions остаются в состоянии Router, а
`process()` превращает каждый поздний успешный subscribe в physical unsubscribe
без user callbacks и replay. Метод не опрашивает providers или transports; в
ручном режиме платформы сначала вызывай `platform.process()`, чтобы provider
смог сформировать completion.

Все callbacks Router сериализованы owner loop, но поля, которые напрямую читает
другой поток бота, всё равно требуют собственного mutex, atomics или очереди
сообщений.

## Lifetime И Порядок Shutdown

Безопасное отношение времени жизни:

```text
provider и owner dispatcher
    живут дольше Router
        который живёт дольше subscribers и posted cleanup work
```

`shutdown()` — идемпотентный запрос остановки, а не безусловное утверждение, что
все асинхронные provider operations завершились до возврата метода. Он сам
выполняет один проход `process()`, поэтому синхронный cleanup по-прежнему
завершается внутри вызова. Используй такой порядок остановки:

1. Прекрати создавать новые команды в потоках ботов.
2. Запроси unsubscribe или уничтожь subscribers, пока dispatcher принимает
   работу.
3. Вызови `router.shutdown()` из owner loop. Новые routes и user delivery
   прекращаются сразу; pending provider operations остаются cleanup tombstones.
4. Оставь provider и owner loop работающими, пока
   `router.is_shutdown_complete()` не вернёт true. В ручном режиме каждый host
   tick вызывает `platform.process()`, а затем `router.process()`.
5. Проверяй `failed_unsubscribe_count()` и выполняй retry с backoff приложения,
   пока owner loop и providers доступны. Failed cleanup не позволяет
   `is_shutdown_complete()` стать true.
6. Останови platform/dispatcher, затем уничтожай providers.

Если приложение объединяет несколько process/shutdown modules, этот drain loop
должен находиться в lifecycle supervisor, а не в Router-specific business code.

Не откладывай уничтожение subscriber до момента, когда dispatcher уже закрыт.
Обычно `MarketDataSubscriberBase` отправляет оставшиеся handles одной cleanup
задачей; если posting уже невозможен, destructor переходит к синхронному
освобождению. Этот fallback не бросает handles, но не заменяет правильный
owner-loop shutdown order.

## Типичные Ошибки

- `register_provider()` вернул false: проверь нулевой ID, повтор instance
  провайдера, повтор ID, пустые aliases и alias collisions.
- Route callback получил невалидный ID: проверь provider key, валидность request,
  shared ownership subscriber, состояние shutdown Router и конфликт callback
  slots.
- Маршрут остаётся pending: провайдер ещё не отправил subscription result.
- Пришёл `SUBSCRIBED`, но нет цен: проверь lifecycle statuses; принятие подписки
  и `READY` — разные состояния.
- Поздний маршрут не получил replay: Router мог не быть привязан в момент
  статуса, либо не совпали type/symbol/timeframe/transport.
- После posting бот не получил callback: продолжай вызывать platform `process()`
  в ручном режиме и проверь, что dispatcher принял команду.
- Дублирование или cross-thread teardown: drain cleanup subscribers до shutdown
  Router и platform.

## Связанные API

- `BaseMarketDataProvider`: физические подписки и live callback contract.
- `IMarketDataSubscriber`: минимальный receiving interface.
- `MarketDataSubscriberBase`: self-subscription и ownership handles.
- `MarketDataHub`: non-owning альтернатива для stream fan-out.
- `MarketDataContinuityService`: граница historical prefill и gap recovery.
- `BaseTradingPlatform::post_task()`: ingress в owner loop платформы.
