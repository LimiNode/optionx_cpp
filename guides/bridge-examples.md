# Bridge Examples Map

This page maps bridge families to the example that shows the intended
application-side wiring. Bridge ownership follows `guides/bridge-taxonomy.md`:
families are grouped by external protocol or adapter contract, not by transport.

| Bridge family | Public include | Config type | Example | What it shows |
| --- | --- | --- | --- | --- |
| Bridge Protocol v1 HTTP/WebSocket | `optionx_cpp/bridges/protocol_v1.hpp` | `BridgeProtocolServerConfig` | `examples/protocol_v1_bridge_smoke.cpp` | Native JSON-RPC command intake over HTTP and WebSocket, account snapshot responses, and WebSocket command handling. |
| Bridge Protocol v1 named pipe | `optionx_cpp/bridges/protocol_v1.hpp` | `BridgeProtocolNamedPipeConfig` | `examples/protocol_v1_named_pipe_bridge_smoke.cpp` | The same native protocol over a local named pipe with newline-delimited JSON framing. |
| MetaTrader Common Files | `optionx_cpp/bridges/metatrader_file.hpp` | `MetaTraderFileBridgeConfig` | `examples/metatrader_file_bridge_smoke.cpp` | C++ bridge side of the MT4/MT5 `Common\Files` layout. |
| MetaTrader command writer | `optionx_cpp/bridges/metatrader_file.hpp` | `MetaTraderFileBridgeConfig` | `examples/metatrader_file_command_writer_smoke.cpp` | Companion writer used by C++ tests and non-MQL clients to append protocol commands. |
| MetaTrader loopback | `optionx_cpp/bridges/metatrader_file.hpp` | `MetaTraderFileBridgeConfig` | `examples/metatrader_file_end_to_end_smoke.cpp` | Writer plus bridge in one local temp layout for end-to-end command/result files. |
| TradingView extension | `optionx_cpp/bridges/trading_view.hpp` | `TradingViewExtensionBridgeConfig` | `examples/tradingview_extension_bridge_smoke.cpp` | Adapter for the browser extension payload shape, not the native protocol_v1 HTTP API. |
| BotBinary/BinaryBot | `optionx_cpp/bridges/bot_binary.hpp` | `BotBinaryBridgeConfig` | `examples/bot_binary_bridge_smoke.cpp` | Compatibility intake for BotBinary `request=...` HTTP URLs and file-signal filenames. |
| BotBinary command helper | `optionx_cpp/bridges/bot_binary.hpp` | none | `examples/bot_binary_command_builder_smoke.cpp` | Formatter/parser helper for legacy BotBinary command strings. |
| Legacy trading pipe | `optionx_cpp/bridges/legacy_trading.hpp` | `LegacyTradingBridgeConfig` | `examples/named_pipe_bridge_smoke.cpp` | Compatibility bridge for the older named-pipe JSON trading protocol. |
| Telegram signal bridge | `optionx_cpp/bridges/telegram.hpp` | `TelegramSignalBridgeConfig` | `examples/telegram_signal_bridge_smoke.cpp` | Deterministic parser, source boundary, signal callback, duplicate report, and shutdown lifecycle without Telegram credentials. |

## Choosing A Bridge

- Use Bridge Protocol v1 when the client can speak OptionX's native JSON-RPC
  contract. HTTP/WebSocket is the default for tools and services; named pipe is
  useful for local Windows integrations.
- Use MetaTrader Common Files when MT4/MT5 indicators or experts must exchange
  commands through terminal files.
- Use TradingView extension when the source is the bundled browser extension.
  It intentionally preserves the extension payload contract.
- Use BotBinary/BinaryBot when existing MQL indicators already emit the legacy
  BotBinary command string or file-signal filename.
- Use the legacy trading pipe only for old clients that already speak that
  named-pipe JSON format.
- Use the Telegram signal bridge when a user-client source must turn channel
  messages into normalized signals. The production source is expected to be a
  `tg-client-stdio` adapter; the parser and bridge can be tested independently
  with a fake source.

Compatibility bridges should convert their external payload into `TradeSignal`
callbacks and reports. They do not need to expose Bridge Protocol v1 endpoints
unless the external system can actually speak that protocol.

## Example Lifecycle Pattern

Bridge examples should keep callback state alive longer than the bridge, install
callbacks before `run()`, and call `shutdown()` before leaving `main`.

```cpp
std::atomic<optionx::SignalId> next_signal_id{1};
std::mutex callback_mutex;

Bridge bridge;
BridgeConfig config;

bridge.configure(std::make_unique<BridgeConfig>(config));
bridge.on_signal_id() = [&] {
    return next_signal_id.fetch_add(1);
};
bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    // Hand the signal to the application.
    (void)signal;
};

bridge.run();
// ...
bridge.shutdown();
```

For longer-running examples, prefer a small RAII cleanup object that calls
`shutdown()` and clears callbacks before local callback state is destroyed.
