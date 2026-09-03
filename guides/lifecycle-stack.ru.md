# Руководство По Lifecycle Stack

Это русский перевод канонического руководства по необязательному общему
lifecycle API. При изменении контракта синхронизируй его с
[`lifecycle-stack.md`](lifecycle-stack.md). Русская версия не является
источником обратных смысловых правок английского документа.

## Назначение

`LifecycleStack` позволяет приложению управлять несколькими модулями через один
вызов `process()` и один staged-запрос `shutdown()`. Это полезно, когда platform
owner loop, `MarketDataRouter`, боты, системы нод и похожие модули запускаются и
останавливаются вместе.

Stack необязателен. Каждый модуль сохраняет прямой lifecycle API и может
управляться без `LifecycleStack`.

Публичный include:

```cpp
#include <optionx_cpp/lifecycle.hpp>
```

## Контракт Модуля

Модули реализуют `optionx::lifecycle::ILifecycleModule`:

```cpp
class ILifecycleModule {
public:
    virtual void process() = 0;
    virtual void shutdown() noexcept = 0;
    virtual bool is_stopped() const noexcept = 0;
};
```

Контракт намеренно мал:

- `process()` продвигает обычную работу или уже начатый graceful shutdown;
- `shutdown()` является идемпотентным запросом прекратить приём новой работы;
- `is_stopped()` становится true только после завершения принадлежащей модулю
  работы и cleanup.

`shutdown()` не обязан завершать асинхронный cleanup до возврата. Owner loop
продолжает вызывать `process()` до достижения terminal state.

`MarketDataRouter` реализует этот интерфейс. Его общий `is_stopped()` совпадает
с `is_shutdown_complete()`. `BaseTradingPlatform` также реализует интерфейс и
возвращает своё существующее terminal lifecycle state.

## Регистрация И Владение

Сначала регистрируй зависимости, затем зависящие от них модули:

```text
platform / owner executor
    -> provider-facing Router
        -> bots или node systems
```

```cpp
lifecycle::LifecycleStack application;
application.add_module(platform);
application.add_module(router);
application.add_module(bot);
```

Stack хранит non-owning указатели. Каждый зарегистрированный модуль должен жить
дольше stack и полного завершения его shutdown. Повторная регистрация,
self-registration и регистрация после начала shutdown отклоняются.

Порядок регистрации описывает зависимости, а не только порядок отображения:

- обычный `process()` идёт вперёд;
- `shutdown()` идёт в обратном порядке;
- одновременно активна только одна стадия shutdown зависимого модуля;
- нижележащие модули продолжают обрабатываться, пока текущий зависимый модуль не
  вернёт true из `is_stopped()`.

Синхронные стадии могут завершиться за один вызов `shutdown()`. Асинхронная
стадия приостанавливает обратный проход до следующих вызовов `process()`.

## Запуск

`LifecycleStack` не вызывает `initialize()` или `run()`. У существующих объектов
разные контракты запуска: у платформы есть `run(bool)`, у component есть
`initialize()`, а боту может требоваться прикладная конфигурация. Оставляй эти
действия явными:

```cpp
platform.configure_auth(...);
platform.run(false);
bot.start();

lifecycle::LifecycleStack application;
application.add_module(platform);
application.add_module(router);
application.add_module(bot);
```

Так не появляется искусственный startup API по наименьшему общему знаменателю.
Отдельную initialization capability можно добавить позже, если несколько
реальных модулей получат одинаковую семантику.

## Owner Loop

Вызывай `LifecycleStack::process()` и `shutdown()` из одного owner loop. Stack не
создаёт поток и не добавляет синхронизацию вокруг методов модулей.

Для платформы в ручном режиме host loop выглядит так:

```cpp
while (running) {
    application.process();
}

application.shutdown();
while (!application.is_stopped()) {
    application.process();
}
```

При порядке регистрации `platform -> router -> bot` каждый tick сначала
позволяет платформе выполнить queued provider callbacks, затем Router забирает
сохранённые lifecycle completions, после чего обрабатывается ещё активный бот.

Не управляй платформой вручную, если она уже использует собственный worker
thread. В таком режиме запускай общий supervisor в настоящем owner loop или
продолжай использовать прямые lifecycle API модулей.

## Staged Shutdown

Для такого порядка регистрации:

```text
platform -> router -> bot
```

остановка выполняется так:

```text
bot.shutdown()
пока bot не stopped:
    platform.process()
    router.process()
    bot.process()

router.shutdown()
пока router не stopped:
    platform.process()
    router.process()

platform.shutdown()
```

Прикладной код видит только `application.shutdown()` и
`application.process()`. Stack сохраняет platform/executor живым, пока Router
ждёт поздние provider completions и результаты физического unsubscribe.

`LifecycleStack` сам является `ILifecycleModule`, поэтому stacks можно вкладывать,
если большое приложение состоит из независимо собранных подсистем.

Вложенные lifecycle stacks должны образовывать ацикличный граф зависимостей.
`add_module()` отклоняет саморегистрацию и повторную регистрацию одной ссылки,
но не обнаруживает косвенный цикл вроде `stack_a -> stack_b -> stack_a`;
приложение должно не допускать такие регистрации.

## Ошибки И Ограничения

Stack не придумывает retry, timeout или abandon policy. Например, ошибка Router
unsubscribe оставляет Router и поэтому весь stack в non-terminal состоянии.
Приложение может проверить `failed_unsubscribe_count()` и вызвать
`retry_failed_unsubscribes()` со своим backoff, пока provider ещё жив.

Исключения из `process()` передаются caller. Shutdown модуля имеет контракт
`noexcept`. Stack не владеет модулями, не уничтожает их и не вызывает shutdown
из своего destructor.

Рабочий интеграционный пример находится в
[`examples/lifecycle_stack_example.cpp`](../examples/lifecycle_stack_example.cpp).
Он объединяет owner-loop executor, deferred market-data provider и
`MarketDataRouter`, а затем дренирует их через общий stack.
