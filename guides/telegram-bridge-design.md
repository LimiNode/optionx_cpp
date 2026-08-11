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
lifecycle without credentials. `TelegramWorkerMessageSource` binds that
interface to a `WorkerClient`-shaped process adapter. The parent repository
pins the merged production worker; the authorized-session smoke remains
opt-in and must never require credentials in normal CI.

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
- a universal symbol pattern with source-specific overrides, preserving broker
  prefixes and suffixes such as `xEURUSD-OTC`;
- symbol normalization that removes presentation whitespace but does not
  silently strip execution-symbol affixes;
- explicit OTC market classification; an unqualified symbol is never assumed
  to be a regular-market asset. Common OTC spellings (`EURUSD_OTC`,
  `EURUSD-OTC`, `EURUSDOTC`, `EURUSD OTC`) are canonicalized to
  `EURUSD_OTC` before execution. The suffix remains configurable for a
  source-specific execution alias;
- direction aliases (`BUY`, `SELL`, `CALL`, `PUT`, arrows);
- separate direction-token rules so a source can map custom words or emoji;
- expiry parsing (`5m`, `M5`, `00:05`, local broker wording);
- optional signal name from message, chat title or rule name;
- optional amount/sizing only when explicitly configured;
- diagnostics for ambiguous or missing fields.

Raw Telegram text remains UTF-8. The parser treats known emoji sequences as
semantic tokens and accepts common variation-selector forms; it does not
require a second UTF-32 parser or normalize away the original text. A result
marker such as `✅`, `❌` or `Profit` has precedence over an overlapping signal
candidate, while a signal on another line of the same message remains
parseable. Result details such as payout, amount and statistics are optional;
missing details must not turn a recognizable result into a new executable
signal.

Martingale parsing should be deferred until the base signal/outcome model is
stable. The parser may preserve raw martingale hints in diagnostics or metadata
without turning them into executable sizing decisions. Any future policy that
keeps only the first step or emits martingale steps must be an explicit
opt-in execution policy, not an implicit parser side effect.

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

Image parsing is deliberately deferred. It must remain an optional provider
outside the core Telegram parser and outside the normal worker installation.
The planned boundary is:

```text
Telegram worker downloads media
  -> raw message contains a controlled local media path and metadata
  -> optional image provider receives the path
  -> provider returns canonical extracted text for the MVP
  -> C++ normalizes, validates, and passes that text to the existing
     deterministic Telegram signal/outcome parser
```

The MVP image-provider contract is text-only: it must return extracted text,
not a synthetic `TradeSignal` and not a second parser result. This keeps image
messages on the same semantic path as ordinary Telegram text. Future
structured observations, such as `direction=BUY` from an arrow template or
`result=WIN` from an icon, are a different contract:

```text
structured visual observations
  -> ImageObservationValidator
  -> ImageObservationFusion
  -> TelegramParsedSignal / TelegramParsedOutcome
```

They must not be passed through `TelegramSignalParser` as fake text. The
fusion layer must apply the same symbol, OTC, ambiguity, identity, and
signal/outcome rules as the text parser. It remains deferred together with
the structured-vision provider.

The first provider should target compact, recurring signal layouts rather than
perform general document understanding. A source-specific image profile may
define normalized regions of interest (ROI), for example:

- symbol ROI for `EUR/USD`;
- direction ROI for text, an arrow, an icon, or a color marker;
- optional result/status ROI.

The provider should crop and preprocess those regions with OpenCV before OCR.
Running text detection over a full 1080x1920 screenshot is not the default
path when the source layout is stable.

The initial recognizer should be CPU-first:

1. Tesseract with a narrow whitelist for symbol characters;
2. constrained symbol normalization and vocabulary matching;
3. RapidOCR with ONNX Runtime as a replacement or fallback if real samples
   show that Tesseract is not reliable enough.

GPU support is not a requirement for the first version. It becomes relevant
only for high-volume image streams or full-image text detection. The OCR
engine must not make a final execution decision. Its output is untrusted input
and must pass the same validation boundary as text messages:

- normalize separators and presentation whitespace;
- preserve broker affixes and canonicalize OTC to `BASE_OTC`;
- accept a fuzzy symbol correction only when it produces one unambiguous
  known instrument;
- reject low-confidence or ambiguous symbol/direction results;
- record the source image and diagnostics for manual review when available.

Direction detection by color, icon, or template matching is source-specific.
The parser must not assume that green always means BUY or red always means
SELL without an explicit source profile. Signal and result regions are also
separate: an image containing a WIN/LOSS marker must produce an outcome, not a
new executable signal. Such structured observations belong to the deferred
fusion layer described above.

Caption and image handling must use an explicit source policy:

- `disabled`: ignore image content and parse only the caption/text;
- `fallback`: invoke the text-only OCR provider when the caption yields no
  accepted semantic result; this mode is unsuitable when the image may contain
  an independent outcome after a valid caption signal;
- `fuse`: inspect both caption and image, then merge accepted signals and
  outcomes by message identity and semantic identity. Duplicate observations
  collapse, while conflicting observations produce a diagnostic and are not
  executable.

The text parser always handles caption text as text. OCR output is handled by
the same parser in the MVP, while future structured image observations go
through the separate fusion layer. Caption and image results share the
Telegram message identity and must be deduplicated before execution.

Before implementing an OCR provider, the worker media contract must support:

- opt-in media download for both history export and live messages;
- a bounded, controlled download directory;
- MIME type, byte size, stable root-relative path, and content hash metadata;
- explicit download, timeout, and size-limit errors;
- no credentials or unrestricted filesystem paths in JSONL payloads.

Media lifetime and ownership are part of the contract. The worker must write
to a temporary file and publish the media record only after the download is
complete and atomically renamed. A published path must resolve inside the
configured media root; `..` traversal, symlink escape, and arbitrary absolute
paths are invalid.

Each published media item has a lease. The worker must keep the file available
until the consumer sends `media.release` or the explicit lease TTL expires.
The terminal record for an export operation stops publication of new media for
that operation but does not invalidate leases that were already published.
Eviction must never remove a leased file. A missing or expired lease is an
explicit media error, not an invitation for the provider to read an untrusted
path. Bounded storage may reject new media or fail the operation when
consumers do not release items quickly enough.

- image-provider tests should use fixed image fixtures;
- core `TelegramSignalParser` tests should remain text-only;
- integration tests should use mocked provider text/results and verify fusion,
  identity deduplication, and fail-closed conflicts.

Real samples should be benchmarked for symbol/direction accuracy, rejection
rate, and CPU latency before choosing a heavier OCR stack.

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

Completed in the current Telegram stack:

1. `tg-client-stdio` worker protocol for dialogs, streaming export,
   live listen/stop, auth status/code/2FA and HTTP/SOCKS proxy configuration.
2. C++ worker supervisor and typed raw-message export DTOs in the standalone
   worker repository.
3. OptionX raw/parsed Telegram DTOs, deterministic regex parser and
   source-independent `TelegramSignalBridge`.
4. Fake-source unit coverage and a runnable no-credentials bridge example.
5. Opt-in authorized-session smoke covering auth status, dialog discovery,
   bounded history export, proxy use, and live message listening.

Current no-credentials examples include a live fake-source bridge smoke and a
deterministic archive/parser replay. The latter uses the same raw-message shape
that `messages.export` streams, while keeping parser outcomes separate from
executable signals.

Next steps:

1. Pin the authorized-session smoke in the OptionX consumer and rerun the
   integration CI.
2. Add operator-facing dialog listing and full JSONL history export tooling
   with date filters, then replay a real test channel through the C++ parser.
3. Implement the bounded worker media contract before starting OCR work.
4. Revisit the deferred OCR provider after collecting representative image
   fixtures. OCR/vision must remain optional and must not block the text
   parser, live bridge, or archive replay.
