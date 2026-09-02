# Build And Test

Краткая инструкция по сборке, зависимостям и проверкам. Код библиотеки в
основном header-only, но tests/examples линкуются с зависимостями из `external`.

## CMake Options

Опорный файл: `CMakeLists.txt`.

| Option / cache var | Default | Назначение |
|---|---:|---|
| `OPTIONX_BUILD_DEPS` | `OFF` | Собрать зависимости из `external/` |
| `OPTIONX_BUILD_EXAMPLES` | `OFF` | Включить examples, если они поддержаны CMake |
| `OPTIONX_BUILD_TESTS` | `OFF` | Собрать tests из `tests/*.cpp` |
| `OPTIONX_DEPS_BUILD_DIR` | empty | Путь к уже собранным зависимостям, когда `OPTIONX_BUILD_DEPS=OFF` |
| `LOGIT_BASE_PATH` | source dir | Base path для LOGIT logs |

Если `OPTIONX_BUILD_TESTS=ON` и `OPTIONX_BUILD_DEPS=OFF`, `OPTIONX_DEPS_BUILD_DIR` обязателен.

Legacy top-level aliases `BUILD_DEPS`, `BUILD_EXAMPLES`, `BUILD_TESTS` and
`DEPS_BUILD_DIR` are still accepted when `optionx_cpp` is configured as the
top-level project. When used via `add_subdirectory()`, prefer the prefixed
`OPTIONX_*` names so parent-project cache variables do not accidentally affect
this library.

## Baseline Commands

Полная локальная сборка с зависимостями:

```bash
cmake -S . -B build -DOPTIONX_BUILD_DEPS=ON -DOPTIONX_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

С уже готовыми зависимостями:

```bash
cmake -S . -B build -DOPTIONX_BUILD_TESTS=ON -DOPTIONX_DEPS_BUILD_DIR=/path/to/deps-build
cmake --build build
ctest --test-dir build
```

Code::Blocks / MinGW пример указан в комментарии `CMakeLists.txt`:

```bash
cmake -G "CodeBlocks - MinGW Makefiles" -S . -B build-cb
```

Submodule/build-tree consumer check:

```bash
cmake -S tests/cmake/submodule_consumer -B build-submodule-consumer
cmake --build build-submodule-consumer --target optionx_submodule_consumer
./build-submodule-consumer/optionx_submodule_consumer
```

This check verifies the intended `add_subdirectory(optionx_cpp)` flow where a
consumer links only `optionx_cpp::optionx_cpp`; dependency include and link
settings should come from the interface target.

## Tests

CMake берет `tests/**/*.cpp` через `GLOB_RECURSE CONFIGURE_DEPENDS` и создает
отдельный executable для каждого test source.

Target naming:

- Если basename уникален, target name равен имени файла без расширения:
  `tests/trade_result_query_test.cpp` -> `trade_result_query_test`.
- Если в разных подпапках есть одинаковый basename, для этого basename
  используется path-derived target:
  `tests/platforms/intrade_bar/api_response_test.cpp` ->
  `tests_platforms_intrade_bar_api_response_test`.
- После нормализации `MAKE_C_IDENTIFIER` CMake дополнительно проверяет
  target-name collisions и показывает оба конфликтующих source path.

Для нового теста достаточно добавить `.cpp` в `tests` или подпапку `tests/*`,
если он использует существующую схему include/link.

Полезные текущие targets:

- `intrade_bar_api_response_test` - parser/typed-result fixtures Intrade Bar.
- `intrade_bar_auth_smoke_test` - guarded online auth smoke; требует proxy.
- `intrade_bar_smoke_cli` - ручная CLI для broker workflows.
- `trade_result_query_test` - DTO/query behavior.
- `trade_record_storage_test`, `trade_record_db_test`,
  `trade_record_stats_test` - storage/statistics behavior.
- `trade_manager_test` - trade execution lifecycle.
- `tradeup_ws_invalid_token_probe` - TradeUp WebSocket probe.

Линкуемые libs для tests в `CMakeLists.txt`: `ws2_32`, `wsock32`, `crypt32`,
`ssl`, `crypto`, `curl`, `mdbx`, `shell32`, `ole32`, `ntdll`, `bcrypt`, `AES`, `gtest`.

Compile definition: `ASIO_STANDALONE`. Для tests также задается
`LOGIT_BASE_PATH`.

## Intrade Bar Smoke CLI

Живые broker workflows вынесены в `tests/intrade_bar_api`.

Документы:

- `tests/intrade_bar_api/README.md` - обзор smoke-подхода.
- `tests/intrade_bar_api/CLI.md` - команды `intrade_bar_smoke_cli`.
- `tests/intrade_bar_api/WORKFLOWS.md` - фактические HTTP sequences.

Правила:

- Любой автоматический credentialed broker request должен идти через proxy.
- Credentials/proxy хранятся только в untracked `*.local.env`.
- `OPTIONX_INTRADE_BAR_CONFIG_FILE` указывает на локальный env-файл.
- Negative-auth сценарии не автоматизировать: broker может заблокировать вход
  на несколько часов после failed login attempts.
- Guarded trade commands требуют `--confirm` или
  `OPTIONX_INTRADE_BAR_ALLOW_TRADE=1`.

Пример сборки CLI:

```bash
cmake --build build-codex --target intrade_bar_smoke_cli -- -j1
```

Пример запуска с локальным config file:

```powershell
$env:OPTIONX_INTRADE_BAR_CONFIG_FILE="tests\intrade_bar_api\intrade_bar_api.local.env"
.\build-codex\intrade_bar_smoke_cli.exe show-account
```

## Include-Contract Checks

GitHub CI also runs a focused Windows smoke job for MetaTrader file transport
helpers. It builds and runs `metatrader_paths_test`,
`metatrader_file_config_include_test`, `metatrader_file_bridge_test`,
`metatrader_file_command_writer_test` and `bridge_umbrella_include_test` on
`windows-latest`, and runs both
`metatrader_file_bridge_smoke` and `metatrader_file_command_writer_smoke`.
Windows filesystem API paths such as atomic replacement, exclusive temp
creation and MetaTrader command-log generation are covered by CI.

The Windows smoke job also runs `scripts/test-metaeditor-compile-smoke.ps1` with
a fake MetaEditor command, then invokes `scripts/compile-metatrader-mql.ps1` for
real compilers. On hosted GitHub runners the real compiler step is best-effort
and skips when MetaEditor is not installed. Local or self-hosted runs can make
it a real compiler smoke by setting `OPTIONX_METAEDITOR5_PATH` and/or
`OPTIONX_METAEDITOR4_PATH`, or by passing paths explicitly:

```powershell
.\scripts\compile-metatrader-mql.ps1 `
  -MetaEditor5Path "E:\Program Files\MetaTrader 5\MetaEditor64.exe" `
  -TimeoutSeconds 60 `
  -RequireMql5
```

An explicit MetaEditor path is authoritative: if it is set but missing, the
script fails instead of falling back to another installed terminal.

Use `-RequireMql4` only with a MetaEditor installation that supports `.mq4`
compilation; some broker folders are MT5-family installations even when their
directory name mentions MetaTrader 4.

The Windows smoke job also runs `scripts/test-metatrader-mql-install.ps1`,
which verifies the installer script against fake terminal data folders. To
install the MQL client header and sample indicators into a real terminal, pass
the terminal data folder shown by MetaTrader's `File -> Open Data Folder`:

```powershell
.\scripts\install-metatrader-mql.ps1 `
  -TargetPath "E:\Program Files\MetaTrader 5\MQL5" `
  -Platform MQL5 `
  -DryRun

.\scripts\install-metatrader-mql.ps1 `
  -TargetPath "E:\Program Files\MetaTrader 5" `
  -Platform MQL5 `
  -RequireTarget
```

`-TargetPath` accepts either a terminal data folder containing `MQL4`/`MQL5`
or a direct `MQL4`/`MQL5` root. Omitting `-TargetPath` scans
`%APPDATA%\MetaQuotes\Terminal`, which covers default installations but not
portable terminals outside the MetaQuotes profile tree.

The installer copies only source files (`.mqh`, `.mq4`, `.mq5`) from the
repository MQL tree, preserving relative subdirectories. It overwrites matching
OptionX files but does not remove unknown or stale files already present in the
destination `OptionX` folders.

При изменении публичных aggregate headers или include policy добавляй или
обновляй тест, который подключает intended public entry point:

```cpp
#include <optionx_cpp/data.hpp>
#include <optionx_cpp/platforms/IntradeBarPlatform.hpp>
```

Direct leaf includes допустимы для white-box tests only when that domain
explicitly keeps self-contained leaf headers. Bridge family tests must include
`optionx_cpp/bridges.hpp` or the nearest `optionx_cpp/bridges/<family>.hpp`
umbrella; bridge leaf/detail headers are not standalone include-contract
targets.

## Examples

Текущие examples:

- `examples/event_mediator_test.cpp`
- `examples/intrade_bar_api_example.cpp`
- `examples/market_data_router_example.cpp`
- `examples/market_data_subscriber_base_example.cpp`
- `examples/task_manager_example.cpp`
- `examples/test_encryption.cpp`
- `examples/test_intrade_bar_http_client.cpp`
- `examples/test_intrade_bar_http_client_module.cpp`
- `examples/test_service_session_db.cpp`
- `examples/test_trade_manager_module.cpp`
- `examples/metatrader_file_bridge_smoke.cpp`
- `examples/metatrader_file_command_writer_smoke.cpp`
- `mql/MQL4/Include/OptionX/OptionXFileBridge.mqh`
- `mql/MQL4/Indicators/OptionX/OptionXFileBridgeSignalExample.mq4`
- `mql/MQL5/Include/OptionX/OptionXFileBridge.mqh`
- `mql/MQL5/Indicators/OptionX/OptionXFileBridgeSignalExample.mq5`

Перед изменением user-facing API проверь ближайший example и обнови его, если
старый сценарий стал недействительным.

## Dependency Layout

| Путь | Роль |
|---|---|
| `external/` | Third-party subcomponents и CMake scripts |
| `external/cmake/*.cmake` | Сборка/подключение зависимостей |
| `external/mdbx-containers` | Storage dependency для `ServiceSessionDB` |
| `external/libmdbx` | MDBX dependency |
| `external/uni-algo` | Unicode Default Case Folding helpers used by text/keyword matching |

Не меняй subcomponents или dependency scripts при обычных правках API/доков.

## Existing Clone Migration

После переименования `libs/` в `external/` существующим клонам стоит
синхронизировать submodule paths:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

Если после обновления на диске осталась старая неотслеживаемая папка `libs/`,
сначала убедись, что в ней нет локальных файлов, затем удали только этот путь:

```bash
git clean -fd -- libs/
```

Не запускай общий `git clean -fd` без проверки: он удаляет все untracked файлы
и каталоги в рабочем дереве.

## Generated Output

Обычно generated и не редактируется вручную:

- `build/`
- `build-*`
- CMake generated files
- copied DLLs рядом с test executables
- dependency build/install output

Если build/test создали такие файлы, не включай их в commit без отдельного
запроса.

## Documentation-Only Checks

Для изменений только в `guides/*.md` и `AGENTS.md` достаточно:

1. Проверить `git diff -- AGENTS.md guides`.
2. Проверить, что ссылки из `AGENTS.md` ведут на существующие файлы.
3. Выполнить smoke-check по ключевым темам:
   - есть build/test инструкция;
   - есть platform/class API guide;
   - есть architecture/codebase orientation;
   - есть implementation notes;
   - есть coding style.

Кодовые tests для documentation-only изменений не обязательны.
