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
- [Market data router guide](guides/market-data-router.md) - канонический
  контракт Router, provider registry, replay, ownership и bot-thread dispatch.
- [Market data router guide RU](guides/market-data-router.ru.md) - русский
  перевод, обновляемый вместе с каноническим руководством.
- [Lifecycle stack guide](guides/lifecycle-stack.md) - канонический контракт
  общего process/shutdown API и staged reverse-order shutdown.
- [Lifecycle stack guide RU](guides/lifecycle-stack.ru.md) - русский перевод,
  обновляемый вместе с каноническим руководством.
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

## Header Ownership And Include Context

Before editing a header under `include/optionx_cpp`, classify it as a
supported public entry point or an internal leaf:

- Supported public entry points are the aggregate/facade headers listed in
  `guides/api-and-header-contracts.md`. Headers under paths such as
  `platforms/<Platform>/`, `market_data/`, and `data/*` are internal leaves
  unless the documentation explicitly promotes them.
- An internal leaf is not a standalone include target. Do not add a project
  cross-domain include, a `../` path, or a broad aggregate merely to make the
  leaf compile in isolation. A leaf may use standard-library, third-party,
  and same-family dependencies supplied by its owning domain.
- The nearest owning aggregate/facade owns the complete cross-domain include
  closure and its order. Add prerequisites there, before including the leaf.
  For example, `platforms.hpp` prepares `utils.hpp`, `data.hpp`,
  `components.hpp`, and the platform contracts before including
  `platforms/IntradeBarPlatform.hpp`; that context transitively supplies
  `platforms/IntradeBarPlatform/ObservedTickHistory.hpp`.
- Tests and examples that verify the include contract must include the same
  supported aggregate used by consumers. Do not use a direct leaf include as
  an aggregate/include-policy test. A direct leaf test is valid only when the
  leaf is intentionally documented and tested as self-contained.
- When the ownership is unclear, inspect the owning aggregate and its include
  order first, then verify the chosen public path with an aggregate consumer
  compile before changing a leaf include.

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
- Для market-data Router каноническая версия - английская
  `guides/market-data-router.md`. Любое смысловое изменение синхронизируй с
  `guides/market-data-router.ru.md` в том же PR; русский перевод не является
  источником обратных изменений английского контракта.
- Для общего lifecycle stack каноническая версия - английская
  `guides/lifecycle-stack.md`. Любое смысловое изменение синхронизируй с
  `guides/lifecycle-stack.ru.md` в том же PR; русский перевод не является
  источником обратных изменений английского контракта.
