# Examples

The examples directory contains small programs that are intended to build
against the current public aggregate headers.

Examples share small local helpers in `example_utils.hpp` for CLI parsing,
config loading, temporary paths, file reads and short waits. Bridge smoke
examples extend them with `bridge_example_utils.hpp` for Ctrl+C handling,
status formatting and demo account snapshots. The bridge-specific `.cpp` files
keep the actual configuration, callbacks and self-test flow close to `main()`.
Examples that need an initial account snapshot use `bridges::BridgeHost` hooks
to publish it before the bridge starts accepting clients.

Bridge examples follow the family layout described in
`guides/bridge-taxonomy.md`: native OptionX API examples use
`protocol_v1`, while compatibility examples keep their platform or legacy wire
contract (`metatrader_file`, `trading_view`, `bot_binary`, or legacy named
pipe) even when they use the same underlying transport.

For a compact family/include/config/example matrix, see
`guides/bridge-examples.md`.

Currently maintained examples:

- `account_info_hub_example.cpp` demonstrates routing account information
  updates through `AccountInfoHub` into an `IAccountInfoSubscriber`
  implementation.
- `intrade_market_data_example.cpp` demonstrates Intrade Bar market-data
  subscriptions, stream status callbacks, direct bar history, and history routed
  through `MarketDataContinuityService`. Live authenticated lifecycle reads
  `OPTIONX_INTRADE_EMAIL` and `OPTIONX_INTRADE_PASSWORD` from the environment.
- `market_data_hub_example.cpp` demonstrates routing provider callbacks through
  `MarketDataHub` into an `IMarketDataSubscriber` implementation.
- `market_data_router_example.cpp` demonstrates provider registration by stable
  numeric ID and aliases, subscription-scoped routing, concrete subscription
  context, late `READY` replay and RAII unsubscription.
- `trading_condition_hub_example.cpp` demonstrates routing payout, session and
  expiration-limit changes through `TradingConditionHub`, plus querying the
  merged current condition snapshot for a concrete symbol.
- `tradingview_extension_bridge_smoke.cpp` starts the local TradingView
  browser-extension HTTP bridge and can run `--self-test` to POST a sample
  indicator signal to itself.
- `named_pipe_bridge_smoke.cpp` starts the legacy named-pipe JSON bridge and,
  on Windows, can run `--self-test` with a local pipe client.
- `bot_binary_command_builder_smoke.cpp` demonstrates the BotBinary/BinaryBot
  compatibility helper that turns an OptionX trade request into the observed
  `request=...` WebRequest value and file-signal name, then parses those legacy
  values back into an OptionX trade signal snapshot.
- `bot_binary_bridge_smoke.cpp` starts the BotBinary/BinaryBot compatibility
  bridge and can run `--self-test` through both the HTTP `request=...` surface
  and the file-signal filename surface.
- `protocol_v1_bridge_smoke.cpp` starts the Bridge Protocol v1 HTTP/WebSocket
  server bridge and can run `--self-test` to POST a `trade.open` command over
  HTTP and send an `account.balance.get` command over WebSocket. The companion
  config is `protocol_v1_bridge_smoke.config.json`; see
  `guides/protocol-v1-bridge-runtime.md` for embedding notes and local endpoint
  examples.
- `protocol_v1_named_pipe_bridge_smoke.cpp` starts Bridge Protocol v1 over a
  local named pipe and, on Windows, can run `--self-test` with a local pipe
  client.
- `telegram_signal_bridge_smoke.cpp` demonstrates the Telegram parser and live
  bridge lifecycle with a deterministic in-memory source. The production
  source boundary can be backed by `tg-client-stdio`; this example needs no
  Telegram credentials.
- `telegram_archive_parser_smoke.cpp` replays exported Telegram raw-message
  records through the parser and keeps executable signals, outcomes and
  diagnostics separate. With `--input <messages.jsonl>` it streams an exported
  JSONL archive one record at a time and prints aggregate totals. It uses
  deterministic fixtures by default and needs no Telegram credentials.
- `telegram_live_bridge_smoke.cpp` starts tg-client-stdio, listens to a live
  Telegram chat, and feeds raw messages through the C++ Telegram signal bridge.
  It requires an authorized worker session and accepts settings from `.env` or
  command-line options.
- `metatrader_file_bridge_smoke.cpp` runs the C++ side of the MetaTrader
  Common\Files bridge against a temporary command/event layout.
- `metatrader_file_command_writer_smoke.cpp` demonstrates the C++ command-writer
  companion used by tests and non-MQL clients to append file commands.
- `metatrader_file_end_to_end_smoke.cpp` runs a local loopback where the command
  writer appends `account.balance.get`, `signal.submit` and `trade.open`, then
  `MetaTraderFileBridge` processes the same `commands.ndjson` and emits
  `events.ndjson`.
- `trade_record_db_example.cpp` demonstrates basic `TradeRecordDB` usage.

MetaTrader-facing examples live under `mql/` in terminal-like layouts:

- `mql/MQL5/Include/OptionX/OptionXFileBridge.mqh`
- `mql/MQL5/Indicators/OptionX/OptionXFileBridgeSignalExample.mq5`
- `mql/MQL4/Include/OptionX/OptionXFileBridge.mqh`
- `mql/MQL4/Indicators/OptionX/OptionXFileBridgeSignalExample.mq4`

Use `scripts/install-metatrader-mql.ps1` to install the matching `OptionX`
include and indicator source files into a terminal data folder. The installer
copies only `.mqh`, `.mq4`, and `.mq5` files, overwrites known OptionX files,
and leaves unknown destination files in place. Portable terminals are supported
by passing the path from MetaTrader's `File -> Open Data Folder` via
`-TargetPath`.

Use `scripts/compile-metatrader-mql.ps1` to compile those MQL examples with a
local MetaEditor installation.

## Bridge Protocol V1 HTTP/WebSocket Smoke

`protocol_v1_bridge_smoke --self-test` binds HTTP and WebSocket on loopback
with ephemeral ports, prints both endpoints, then sends:

- HTTP `POST /api/v1/bridge/command` with a `trade.open` JSON-RPC command;
- WebSocket `/api/v1/bridge/ws` with an `account.balance.get` JSON-RPC command.

Both transports use the same bridge instance, static local secret and protocol
callbacks. Pass `--config path` to override `BridgeProtocolServerConfig` fields
such as bind address, ports, paths, secret and local unauthenticated mode.

For a stable local endpoint layout, start:

```powershell
.\build\protocol_v1_bridge_smoke.exe --config examples\protocol_v1_bridge_smoke.config.json
```

The sample config binds HTTP to `127.0.0.1:6562` and WebSocket to
`127.0.0.1:6563`, both with `X-OptionX-Secret: local-dev-secret`.

Old exploratory broker and component probes were removed because they referenced
pre-refactor include paths and contained local credentials. Broker smoke flows
live under `tests/intrade_bar_api/` and read credentials from local environment
files instead.
