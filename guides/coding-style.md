# Coding Style

Стиль проекта выводится из headers в `include/optionx_cpp`. Не добавляй общие
правила C++, если они не совпадают с локальным стилем.

## Files And Headers

- Header-only by default.
- Расширение: `.hpp`.
- Project-owned C/C++ headers use `#pragma once` together with a
  non-reserved include guard. Guard names are derived from the project prefix
  and header path and must clearly look like internal header guards, for
  example `OPTIONX_HEADER_PLATFORMS_INTRADERBAR_PLATFORM_HPP_INCLUDED`.
  Do not use reserved identifiers: guards must not start with an underscore,
  must not start with an underscore followed by an uppercase letter, and must
  not contain a double underscore anywhere.
- Public aggregate headers лежат в `include/optionx_cpp/*.hpp`.
- External consumers include public headers with the installed prefix, for
  example `<optionx_cpp/data.hpp>` or
  `<optionx_cpp/platforms/IntradeBarPlatform.hpp>`.
- Headers in the same family use paths that are genuinely relative to the
  including file, for example `"telegram/TelegramRawMessage.hpp"` from
  `bridges/telegram.hpp` or `"detail/Parser.hpp"` from a neighboring family
  header.
- Nested headers include cross-domain dependencies through the installed
  prefix, for example `<optionx_cpp/data/trading.hpp>` or
  `<optionx_cpp/utils/tasks.hpp>`. Do not use quoted `"optionx_cpp/..."` paths
  and do not rely on `include/optionx_cpp` as an additional include root.
- Prefer the nearest aggregate header for public-domain and cross-domain
  dependencies; do not rebuild aggregate include order inside leaf DTO headers.
- Do not use `../` in `#include` directives.
- Avoid broad or implementation-only includes in leaf headers. Direct leaf
  includes are acceptable for white-box/internal tests only for domains that
  explicitly keep self-contained leaves. Bridge family leaf/detail headers are
  not standalone include points; use `bridges.hpp` or the nearest
  `bridges/<family>.hpp` umbrella.
- Platform-specific files лежат в папке платформы и подключаются относительными
  includes из facade header.

Пример: `include/optionx_cpp/platforms/IntradeBarPlatform.hpp`.

## Header Guards

For every project-owned C/C++ header, use `#pragma once` together with a
non-reserved include guard.

Include guard names must be derived from the project prefix and the header path,
and must clearly indicate that the macro is a header guard. Recommended form:

```cpp
<PROJECT_PREFIX>_HEADER_<PATH>_<FILE>_<EXT>_INCLUDED
```

Example:

```cpp
#pragma once
#ifndef PROJECT_HEADER_CORE_LIBRARY_INFO_HPP_INCLUDED
#define PROJECT_HEADER_CORE_LIBRARY_INFO_HPP_INCLUDED

// header contents

#endif // PROJECT_HEADER_CORE_LIBRARY_INFO_HPP_INCLUDED
```

Do not use identifiers reserved for the compiler, standard library, platform
SDK, or other implementation internals. In particular, do not use include guard
names that start with an underscore, start with an underscore followed by an
uppercase letter, or contain a double underscore anywhere.

Bad examples:

```cpp
_PROJECT_CORE_LIBRARY_INFO_HPP_INCLUDED
__PROJECT_CORE_LIBRARY_INFO_HPP_INCLUDED
PROJECT__CORE_LIBRARY_INFO_HPP_INCLUDED
```

Header guard macros are internal service macros and should be visually
distinguishable from public configuration or feature macros. Prefer using a
dedicated marker such as `HEADER`, `GUARD`, or `INCLUDED` in the guard name
rather than using leading underscores.

Public configuration macros may keep their normal project-prefixed names, for
example:

```cpp
PROJECT_ENABLE_LOGGING
PROJECT_VERSION_MAJOR
PROJECT_HAS_BACKEND_X
```

Implementation fragments such as `.ipp`, `.inl`, or `.tpp` files should follow
the project's chosen policy explicitly. If they are only included from already
guarded headers and are not intended for direct inclusion, they may remain
unguarded. If they are intended to be included directly, they must follow the
same non-reserved guard naming rule.

## Namespace

Основные namespace:

- `optionx` - общие DTO/enums trading/account/market data.
- `optionx::events` - event classes.
- `optionx::utils` - infrastructure helpers.
- `optionx::components` - base components/managers.
- `optionx::platforms` - platform facades.
- `optionx::platforms::intrade_bar` - Intrade Bar internals.
- `optionx::platforms::tradeup` - TradeUp internals.
- `optionx::storage` - storage services.
- `optionx::bridges` - bridge contracts.

Для нового platform-specific manager используй subnamespace платформы, а не
общий `optionx::platforms`.

## Naming

| Сущность | Стиль | Пример |
|---|---|---|
| Классы/structs | PascalCase | `TradeRequest`, `BaseTradingPlatform` |
| Base classes | `Base<Name>` | `BaseComponent`, `BaseBridge` |
| Managers/components | PascalCase + `Manager`/`Component` | `AuthManager`, `HttpClientComponent` |
| Methods | snake_case | `place_trade`, `configure_auth`, `get_info` |
| Fields | `m_` + snake_case для private/protected | `m_event_bus`, `m_task_manager` |
| Public DTO fields | snake_case без `m_` | `account_type`, `expiry_time` |
| Type aliases | snake_case + `_t` где уже есть стиль | `trade_request_t`, `callback_t` |
| Enum types | PascalCase | `PlatformType`, `TradeState` |
| Enum values | UPPER_SNAKE_CASE | `INTRADE_BAR`, `WAITING_OPEN` |
| Files | PascalCase для классов, snake_case для helpers | `TradeRequest.hpp`, `http_utils.hpp` |

## Doxygen

Публичные headers обычно имеют:

```cpp
/// \file MyHeader.hpp
/// \brief Short description.
```

Публичные классы:

```cpp
/// \class MyClass
/// \brief Short class description.
```

Методы документируются через `/// \brief`, `\param`, `\return`, если они часть
public API. Для private helper comments добавляй только если логика неочевидна.

Не копируй mojibake из старых комментариев; новые комментарии должны быть
читаемыми.

## Error Handling

Локальные паттерны:

- Public operations часто возвращают `bool` для success/failure.
- Optional read возвращает `std::optional<T>`, например session value.
- Trade errors отражаются в `TradeResult`/`TradeErrorCode`.
- HTTP failures остаются в `kurlyk::HttpResponsePtr`.
- Storage ловит `mdbxc::MdbxException` и `std::exception`, логирует и
  возвращает failure value.
- String-to-enum имеет два слоя: non-throwing `to_enum(str, value)` и throwing
  `to_enum<T>(str)`.

Не бросай исключения из component `process()`/callbacks без явной обработки:
platform loop не должен падать из-за обычной сетевой или validation ошибки.

## Logging

Проект использует LOGIT macros:

- `LOGIT_TRACE0()`
- `LOGIT_TRACE(...)`
- `LOGIT_WARN(...)`
- `LOGIT_ERROR(...)`
- `LOGIT_PRINT_ERROR(...)`

Смотри примеры в `BaseTradingPlatform.hpp`, `TaskManager.hpp`,
`BaseHttpClientComponent.hpp`, `ServiceSessionDB.hpp`.

Правила:

- Логируй boundary failures: network/storage/async exception/shutdown misuse.
- Не логируй secrets: auth token, password, raw session, AES key.
- Для повторяющихся periodic paths избегай noisy logs без причины.

## Const, References, Pointers

Локальный ownership стиль:

- `std::unique_ptr<T>` - передача владения через public API/events, например
  `configure_auth`, `place_trade`, bridge callbacks.
- `std::shared_ptr<T>` - shared domain state/events, например account info,
  `TradeRequestEvent`.
- Raw pointers - non-owning registered components/listeners; lifetime должен быть
  обеспечен owner platform.
- `const T&` - входные DTO без владения, например `BarHistoryRequest`.
- Callback references возвращаются как `callback_t&`, чтобы пользователь мог
  назначить handler.

Do:

- Принимая ownership, используй `std::move`.
- Для nullable ownership возвращай/принимай smart pointer, не raw owning pointer.
- Для event payload не сохраняй raw pointer дольше callback.

Avoid:

- Не делай `shared_ptr` там, где поток владения единственный.
- Не отдавай mutable reference на internal state без существующего проекта
  pattern.
- Не регистрируй raw component pointer, если объект может умереть раньше platform.

## C++ Standard

Проект CMake объявляет C++ project, а agent instructions фиксируют C++17.
В коде уже используются:

- nested namespace syntax `namespace optionx::utils`
- `std::optional`
- `std::unique_ptr`, `std::shared_ptr`
- `std::atomic`, `std::thread`, `std::future`
- `nlohmann::json`

Не добавляй зависимости на более новый стандарт без явного запроса.

## Formatting

Локальный стиль неоднороден, но для нового кода придерживайся:

- 4 spaces indent.
- Opening brace на той же строке для классов/методов.
- Короткие inline methods допустимы в headers.
- Группируй public/protected/private явно.
- В таблицах enum conversion сохраняй существующий стиль `to_str`/`to_enum`.

## Compatibility Checklist

Перед изменением public header проверь:

- Не сломался ли aggregate include.
- Не изменились ли JSON field names.
- Не изменились ли enum string values.
- Не изменилась ли callback signature.
- Не появился ли новый required include у пользователей.
- Не добавлен ли platform-specific dependency в common/utils/data layer.
