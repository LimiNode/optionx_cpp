# AGENTS.md

Этот файл намеренно короткий. Это индекс для AI coding agents, которые
работают в `optionx_cpp`; открывай только те документы, которые нужны для
текущей задачи.

## Read First

- [Coding agent workflow](guides/coding-agent-workflow.md) - базовый процесс для
  задач с изменением файлов.
- [Project overview](guides/project-overview.md) - назначение библиотеки,
  публичная поверхность, основные домены и поток выполнения.
- [Platform API guide](guides/platform-api-guide.md) - практический справочник
  по платформам, модулям, событиям, DTO, storage и bridge API.
- [API and header contracts](guides/api-and-header-contracts.md) - публичный
  API, include-модель, typed broker results, trade result/history contracts.
- [Codebase orientation](guides/codebase-orientation.md) - карта проекта,
  DDD-слои, зависимости, расширение и безопасные точки входа.
- [Build and test](guides/build-and-test.md) - CMake options, зависимости,
  локальные проверки, примеры и generated output.
- [Implementation notes](guides/implementation-notes.md) - lifecycle, pub-sub,
  task scheduling, HTTP/WebSocket, trade queue, session DB и ограничения.
- [Bridge lifecycle guide](guides/bridge-lifecycle-guide.md) - reusable
  lifecycle, callback, shutdown, thread-joining and transport-race patterns for
  bridge implementations and reviews.
- [TradingView bridge research](guides/tradingview-bridge-research.md) -
  найденные пути TradingView -> bridge, webhook constraints, browser-extension
  MVP и риски.
- [Bridge protocol v1 draft](guides/bridge-protocol-v1.md) -
  общий draft протокола для HTTP/WebSocket/named-pipe мостов.
- [Bridge Protocol v1 runtime quickstart](guides/protocol-v1-bridge-runtime.md) -
  практическое подключение HTTP/WebSocket и named-pipe runtime-мостов.
- [Bridge examples map](guides/bridge-examples.md) - карта runnable examples,
  public includes и config types для каждого bridge family.
- [Bridge protocol v1 draft RU](guides/bridge-protocol-v1.ru.md) -
  русский перевод draft протокола; английская версия каноническая, RU
  синхронизируется с EN и не является источником обратных правок.
- [Bridge taxonomy](guides/bridge-taxonomy.md) - как раскладывать bridge family
  и transport ownership, включая protocol_v1, legacy named pipe, TradingView и
  BotBinary.
- [Telegram bridge design](guides/telegram-bridge-design.md) - план будущего
  Telegram sidecar/stdio bridge, history export, parser и auth/proxy boundary.
- [Coding style](guides/coding-style.md) - naming, namespace, Doxygen,
  обработка ошибок, ownership и header-only правила.
- [Git workflow](guides/git-workflow.md) - branch policy, PR-only workflow,
  branch naming и проверки перед PR.
- [Commit conventions](guides/commit-conventions.md) - формат коммитов, если
  пользователь просит создать commit.

## Critical Defaults

- Перед правками проверь `git status --short` и не перетирай чужие изменения.
- Не коммить напрямую в `main`; создай отдельную ветку и PR, если пользователь
  явно не попросил прямой commit в `main`.
- Для поиска по репозиторию используй `rg` / `rg --files`.
- Держи библиотеку header-only, если задача явно не требует нового `.cpp`.
- Для публичных data/module/platform domains используй ближайший aggregate
  include point вместо ручного восстановления порядка зависимостей в leaf headers.
- Для bridge families используй только `include/optionx_cpp/bridges.hpp` или
  umbrella headers `bridges/metatrader_file.hpp`, `bridges/named_pipe.hpp`,
  `bridges/trading_view.hpp`; leaf/detail bridge headers не являются
  самостоятельными include-точками.
- Внутри одной header family используй реальный путь относительно включающего
  файла, например `"telegram/TelegramRawMessage.hpp"` из umbrella в `bridges/`
  или `"detail/Parser.hpp"` из соседнего family header.
- Cross-domain dependencies из вложенных headers подключай через установленный
  префикс, например `<optionx_cpp/data/trading.hpp>`. Не используй
  `"optionx_cpp/..."` и не полагайся на `include/optionx_cpp` как include-root.
- Для project-owned C/C++ headers используй `#pragma once` и non-reserved
  include guard без leading underscore, например
  `OPTIONX_HEADER_<PATH>_<FILE>_<EXT>_INCLUDED`.
- Проект ориентирован на C++17 и CMake `>= 3.18`.
- Переиспользуй `utils::EventBus`, `utils::EventMediator`, `utils::TaskManager`
  и `Base*Module` вместо локальных аналогов pub-sub, loop и task queue.
- Не меняй account/trade/session state напрямую, если для этого уже есть event,
  manager или provider.
- В async/HTTP/WebSocket коде сохраняй lifecycle: `run()` -> periodic
  `process()` -> `shutdown()`/drain/cancel.
- Для публичных headers сохраняй Doxygen `///` с `\file`, `\class`, `\brief`.
- Для code changes запускай самые узкие релевантные tests/examples; для
  documentation-only изменений достаточно Markdown/link smoke-check.
- Для bridge protocol draft основная версия - английская
  `guides/bridge-protocol-v1.md`; при изменении её синхронизируй русский
  перевод `guides/bridge-protocol-v1.ru.md`. Не вноси смысловые изменения в
  английский документ, исходя только из русской версии.
