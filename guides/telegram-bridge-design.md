# Telegram Bridge Design

This document captures the architecture and current boundaries of the Telegram
signal bridge. The public C++ DTO/parser/bridge layer is implemented, while
the authorized Telegram worker adapter remains a separate integration step.

## Problem Shape

Telegram signal sources are different from the existing bridge families:

- authorization is an interactive Telegram user-client flow, not a single bot
  token;
- operators need both live messages and historical exports for parser
  validation and backtesting;
- messages may contain trade signals, trade outcomes, both, or unrelated text;
- some channels use images, so OCR/vision should be optional and isolated from
  core bridge code;
- proxy support is a first-class runtime requirement.

The existing `NewYaroslav/telegram-monitoring-tool` already covers the expensive
Telegram side with Telethon: user authorization, dialog listing, forum topics,
history export through `iter_messages`, media export, and live
`NewMessage` monitoring. The future OptionX work should turn that capability
into a machine API instead of reimplementing MTProto in C++.

## Architecture

Use a sidecar worker process:

```text
OptionX C++ host
  -> starts Telegram worker binary
  -> exchanges stdio protocol messages
  -> receives raw Telegram message events
  -> parses them into OptionX signal/outcome records
  -> emits live TradeSignal callbacks or historical parser results
```

The sidecar can be Python packaged into a standalone binary. This keeps
Telethon, Python runtime details, Telegram session storage, and Telegram proxy
configuration out of the header-only C++ bridge library.

`NewYaroslav/qwen3-tts-bridge-cpp` is the nearest pattern to reuse:

- C++ supervises a persistent worker process;
- startup has a `hello` / `ready` handshake and timeout;
- stderr is diagnostics, not protocol data;
- event queues are bounded;
- shutdown is graceful first and forced after a timeout;
- packaged Python worker flows are tested separately.

Telegram payloads are JSON and do not need binary audio framing. Start with
newline-delimited JSON over stdin/stdout. Keep the protocol versioned so a
framed transport can replace JSONL later if media bytes ever need to cross the
stdio boundary.

The current OptionX bridge does not own a Telethon process. It consumes the
`TelegramMessageSource` interface, so fake sources can exercise parsing and
lifecycle without credentials. The concrete source adapter will bind that
interface to `tg-client-stdio::WorkerClient` after the worker repository's
supervisor and typed archive API are merged and pinned by OptionX.

## Stdio Protocol Envelope

Every JSONL record should use an envelope so responses, long-running exports,
live events and errors can share one stdout stream:

```json
{
  "protocol_version": 1,
  "message_type": "request",
  "request_id": 42,
  "operation": "messages.export",
  "payload": {}
}
```

Initial `message_type` values:

- `request`: C++ host asks the worker to perform an operation;
- `response`: terminal response for a request;
- `event`: asynchronous or streaming event;
- `error`: recoverable protocol, authorization, proxy or operation error.

`request_id` must be non-zero for host-initiated operations and must be copied
to every related response, event or error. Worker-initiated live events may use
`request_id = 0` when they are not tied to a specific active request.

Each request produces exactly one terminal record: either `response` or
`error`. The `request_id` remains active until that terminal record and must not
be reused while active. Session-fatal errors terminate all active requests and
the worker session; the host must report the active requests as failed locally
if the worker exits before emitting per-request terminal records.

Large historical exports must be streamed instead of returned as one giant
array:

```text
messages.export request
  -> export.started event
  -> export.message event x N
  -> export.completed response
```

Both peers must enforce a maximum outbound JSONL record size before writing
and a maximum inbound JSONL record size while reading, before unbounded
allocation or JSON parsing. The C++ client should use bounded queues; if the
application does not drain events fast enough, the client must fail the worker
session explicitly instead of accumulating unbounded history in memory.

## Component Split

Keep the live bridge, archive export, and parsing separate.

### Worker Client

`tg-client-stdio::WorkerClient` owns the process/session protocol. It exposes
operations such as:

- `auth.status`;
- `auth.send_code`;
- `auth.submit_code`;
- `auth.submit_password`;
- `dialogs.list`;
- `messages.export`;
- `messages.listen`;
- `messages.stop`;
- `shutdown`.

The C++ side should not know Telethon types. It receives normalized JSON data
from the worker.

### Live Bridge

`TelegramSignalBridge` is a normal bridge family that converts live Telegram
message events into `TradeSignal` callbacks and signal reports.

It should not expose historical export through `BaseBridge::run()` or
`process()`. Bridge lifecycle remains live-intake lifecycle.

The current bridge also applies a bounded identity-based dedupe cache. Parser
diagnostics, duplicate messages, allocator failures and callback failures are
reported through `BridgeSignalReport`; they do not silently become accepted
signals.

### Archive Source

Historical export is a separate capability. The first implementation can be
Telegram-only:

```text
TelegramArchiveClient::stream_messages(query, on_message) -> ExportSummary
TelegramSignalParser::parse(raw_message) -> TelegramParsedMessage
```

Do not add a broad `BaseBridge` history method until at least two families need
the same contract and the return type is stable. MetaTrader tester exports may
become the second user, but their source data is tester/file output rather than
Telegram dialogs.

### Parser

The parser consumes raw messages and produces a richer result than
`TradeSignal` alone:

```text
TelegramRawMessage
  -> TelegramParsedMessage
       raw_identity
       parsed_signals[]
       parsed_outcomes[]
       diagnostics[]
```

This shape matters because Telegram channels often publish:

- entry signals;
- correction messages;
- martingale steps;
- outcome/result messages;
- screenshots;
- unrelated chat text.

Live bridge code should emit every accepted executable `TradeSignal` produced
by parser rules. If a source must allow at most one executable signal per
message, that rule should be enforced explicitly by the parser and additional
matches should become diagnostics rather than being silently dropped by the
bridge.

## Raw Message Shape

The worker should provide enough stable identity for dedupe and replay:

```json
{
  "chat_id": "-1001234567890",
  "chat_title": "Signals",
  "topic_id": "42",
  "message_id": 1234,
  "date_ms": 1784830000000,
  "edit_date_ms": 1784830010000,
  "sender_id": "777",
  "reply_to_message_id": 1230,
  "grouped_id": "987654321",
  "text": "EURUSD BUY 5m",
  "media": [
    {
      "kind": "photo",
      "file_path": "downloads/-1001234567890/1234.jpg",
      "mime_type": "image/jpeg"
    }
  ]
}
```

Keep message identity separate from revision identity:

```text
message_identity  = telegram:<chat_id>:<topic_id-or-0>:<message_id>
revision_identity = <message_identity>:<edit_date_ms-or-0>
```

`message_identity` is the transport dedupe key for live trade execution.
`revision_identity` is useful for parser diagnostics, replay and backtesting.
An edit must not automatically open a second trade just because
`edit_date_ms` changed. It may create a correction/diagnostic event, and a
source may opt into re-execution only through an explicit parser or source
rule.

`reply_to_message_id` should be present when Telegram exposes it because result
messages are often replies to the original signal. `grouped_id` should be
preserved for media albums. Forward origin metadata can be added later without
blocking the first raw schema.

## Historical Export Query

History export should support:

- chats by id, username, or configured alias;
- optional forum topic id;
- `from_date_ms`;
- `to_date_ms`;
- `limit`;
- newest-first or oldest-first output order;
- text-only or media-metadata mode;
- optional media download directory.

The worker may internally page with Telethon `iter_messages`. It must emit
export records through the streaming lifecycle described above and surface
flood waits and authorization failures as explicit protocol errors, not as
empty successful exports.

## Signal Parser Rules

The first parser should be deterministic and testable:

- per-source regex rules;
- symbol normalization;
- direction aliases (`BUY`, `SELL`, `CALL`, `PUT`, arrows);
- expiry parsing (`5m`, `M5`, `00:05`, local broker wording);
- optional signal name from message, chat title or rule name;
- optional amount/sizing only when explicitly configured;
- diagnostics for ambiguous or missing fields.

Martingale parsing should be deferred until the base signal/outcome model is
stable. The parser may preserve raw martingale hints in diagnostics or metadata
without turning them into executable sizing decisions.

## Outcomes

Do not force outcome messages into `TradeSignal`. Add a separate parsed outcome
shape when needed:

```text
TelegramParsedOutcome
  source_message_identity
  reply_to_message_identity?
  symbol?
  direction?
  signal_name?
  result = win | loss | refund | unknown
  step?
  raw_text
```

Backtesting can later correlate parsed signals and outcomes by source, time
window, symbol, direction and optional signal name. When
`reply_to_message_id` is available, it should be preferred over heuristic
correlation.

## OCR / Vision

Image parsing should be optional and external:

```text
Telegram worker downloads media
  -> parser sees media metadata/path
  -> optional localhost OCR/vision provider returns extracted text
  -> deterministic text parser processes extracted text
```

The OCR provider can be a local service, a remote API router, or disabled. Core
tests should mock it with fixed text fixtures.

## Authentication And Proxy

Telegram authentication state belongs to the worker:

- API id/hash;
- phone number;
- code submission;
- 2FA password submission;
- session file path;
- proxy configuration.

The C++ host should provide config and surface status/errors. It should not
store Telegram passwords in bridge DTOs or logs.

Proxy config should support at least SOCKS5 and HTTP where the underlying
Telegram client library supports them. Proxy failures must be distinct from
authorization failures.

## Implementation Status And Next Steps

Completed without an authorized Telegram session:

1. `tg-client-stdio` worker protocol for dialogs, streaming export,
   live listen/stop, auth status/code/2FA and HTTP/SOCKS proxy configuration.
2. C++ worker supervisor and typed raw-message export DTOs in the standalone
   worker repository.
3. OptionX raw/parsed Telegram DTOs, deterministic regex parser and
   source-independent `TelegramSignalBridge`.
4. Fake-source unit coverage and a runnable no-credentials bridge example.

Next steps:

1. Merge and pin the worker repository's supervisor/archive PRs.
2. Pin the merged worker repository in an OptionX consumer and run the adapter
   against the mock worker process.
3. Add a historical archive/parser fixture example.
4. Perform the first real authorization, proxy and live-channel check with an
   operator-provided Telegram session.

OCR/vision remains a separate optional provider and should not block the text
parser or the first authorized-session test.
