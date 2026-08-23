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
- configurable timeframe semantics: `TIMEFRAME_DURATION` for SPRINT,
  `BAR_END` for an absolute CLASSIC expiry at the next UTC bar boundary, or
  `CUSTOM_DURATION` for a source-configured duration in seconds;
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

Statistics printed by a Telegram source, such as `9 | 1 =90.0` or
`251 | 17`, are source-reported metadata and are not authoritative for
OptionX performance statistics. The parser may preserve them in raw text, but
the local backtest/statistics layer must calculate its own values from parsed
signals, outcomes, and their raw message timestamps.

`date_ms` is retained on every parsed signal and outcome and propagated to the
executable `TradeSignal` as `source_time_ms`. It is the source publication time,
not the local receive time. `TelegramSignalBridgeConfig::max_signal_age_seconds`
is an optional live-intake guard: a positive value rejects a normally dispatched
signal when it arrives after that age, while `0` disables the guard. Archive
replay therefore preserves historical timing without accidentally executing
historical signals.

Martingale recognition is source-configured and explicit. A
`TelegramMartingaleRule` supplies one regex capture group for a non-negative
step number; strategy suffixes such as `COBRA -5` are never treated as a step
by default. The parsed step is metadata only and does not infer an amount or a
money-management multiplier. A configured step marker is removed from the
display signal name so steps from the same named strategy retain a stable
correlation key.

The bridge offers opt-in dispatch policies:

- `ALL_SIGNALS` keeps the normal behavior; recognized steps are only metadata
  and the stale-signal guard applies;
- `FIRST_SIGNAL_ONLY` accepts unmarked signals and explicit step `0`, while
  ignoring later explicit steps; the stale-signal guard applies;
- `CONTIGUOUS_STEPS` requires an explicit step, accepts a fresh `0`, then only
  `1`, `2`, and so on for the same chat/topic/symbol/direction/strategy group.
  Later expected steps intentionally bypass the stale-signal guard, because a
  source-side chain must not silently skip a delayed step. Intake is serialized
  through allocator/callback completion, so a later step cannot pass while its
  predecessor is still pending or rolls back.

Unknown `martingale_policy` values are rejected during configuration validation;
they never fall back to `ALL_SIGNALS`.

The policies only decide whether a source signal reaches the trade pipeline.
They do not infer a source-side stake multiplier from channel text.

The bridge also has a separate, opt-in local anti-martingale policy. It is an
execution-side sizing policy, not a parser feature and not an interpretation of
Telegram-reported statistics or outcomes. Its configuration is deliberately
explicit:

- `anti_martingale_enabled` is false by default;
- `anti_martingale_multiplier` must be finite and greater than one;
- `anti_martingale_max_steps` bounds consecutive winning increases;
- `anti_martingale_max_amount` is a required absolute amount cap and must be
  at least `fixed_amount`.

Each Telegram bridge instance keeps an independent series for a
chat/topic/symbol/direction/strategy group. The initial signal uses
`fixed_amount` at anti-martingale step `0`. A confirmed broker `WIN` advances
the next signal by one step and applies the multiplier, capped by
`anti_martingale_max_amount`. A `WIN` at the configured maximum step resets the
next signal to the base amount. Every other terminal broker result (`LOSS`,
`REFUND`, `STANDOFF`, cancellation, or execution/check error) also resets the
series to step `0`.

At most one anti-martingale-managed signal may be outstanding in a group. The
bridge marks a group pending while the signal callback is running and keeps it
pending until the execution pipeline reports a terminal `TradeResult` for the
same `signal_id`. A later source message for that group is rejected while the
result is pending, avoiding two trades that both assume the same next stake.
Repeated or non-terminal result updates do not advance the series.

The bridge reserves both its dedupe key and any source/local money-management
state before it transfers the signal to the callback. This makes a terminal
result delivered synchronously from inside the callback safe: its `signal_id`
is already registered. An allocator failure rolls the reservation back. A
callback exception is an ambiguous execution outcome, because the callback may
already have queued or submitted the trade before throwing. The bridge reports
`ambiguous_dispatch_failure` but deliberately keeps the dedupe and pending
state fail-closed rather than risking a duplicate order.

Only the actual broker/execution `TradeResult` delivered through
`BaseBridge::update_trade_result()` changes local anti-martingale state.
Telegram outcome messages remain parser/archive data and must not advance,
reset, or otherwise size a live trade. The local anti-martingale policy cannot
be combined with source-side `CONTIGUOUS_STEPS`, because that mode has a
different requirement to dispatch every explicit source chain step. It may be
used with `ALL_SIGNALS` or `FIRST_SIGNAL_ONLY` according to the desired source
filtering behavior.

An optional source-chain continuation rule can monitor a deliberately narrow
class of Telegram strategies. A rule names one strategy, its configured final
source step, one exact continuation outcome, a timeout (15 seconds by default),
and either `REPORT_ONLY` or `EMIT_ASSUMED_SIGNAL` action. The bridge starts a
series only at explicit source step `0` and accepts later source steps only in
strict contiguous order. It accepts an outcome only when Telegram provides an
exact `reply_to_message_id` link to the latest accepted source signal; text,
symbol, or timestamp heuristics never authorize a continuation.

When the configured continuation outcome arrives before the configured final
step, the bridge waits for exactly the next real source step. A real step
cancels the wait. On timeout, `REPORT_ONLY` emits the suspicious diagnostic
`expected_source_step_missing`. `EMIT_ASSUMED_SIGNAL` is separately opt-in and
constructs the expected next signal only from the last accepted SPRINT template
in that source chain. It has a synthetic dedupe identity and carries explicit
machine-readable provenance:

- `is_assumed = true`;
- `assumed_reason = expected_source_step_missing`;
- `assumed_source_identity` naming the original Telegram signal;
- `assumed_source_step` naming the missing source step.

The fallback is fail-closed when the template lacks a SPRINT duration, the
source sequence changed, a callback is unavailable, or a previous ambiguous
dispatch still owns the sequence. A synthetic step does not create another
watch: Telegram cannot reply to a message that it never published.

For normal source martingale, assumed dispatch requires
`CONTIGUOUS_STEPS`. For local anti-martingale, Telegram's correlated outcome
only authorizes the source continuation; the amount and local anti step still
come solely from the application's terminal broker `TradeResult`. A pending
local anti-martingale trade therefore blocks an assumed signal even if the
Telegram outcome arrives first.

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
correlation. The signal message `raw.date_ms` is the observed publication time;
the outcome message `raw.date_ms` is the observed result time. A scheduled
entry time embedded in text is a separate semantic field and must not silently
replace the publication timestamp until a source-specific time-zone policy is
configured.

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
executable signals. `telegram_archive_parser_smoke --input <messages.jsonl>`
consumes that JSONL one record at a time and reports message, signal, outcome,
diagnostic and unparsed totals.

Next steps:

1. Replay representative real channels through the C++ parser and preserve only
   anonymized or synthetic regression fixtures.
2. Correlate source outcomes with signals for archive statistics and replay;
   reported channel statistics remain non-authoritative.
3. Extend result-driven local money management only after replay statistics and
   execution contracts establish the required grouping and risk semantics.
4. Implement the bounded worker media contract before starting OCR work.
5. Revisit the deferred OCR provider after collecting representative image
   fixtures. OCR/vision must remain optional and must not block the text
   parser, live bridge, or archive replay.
