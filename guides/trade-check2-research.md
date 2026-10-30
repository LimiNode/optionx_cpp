# Intrade `trade_check2.php` Research

Status: `LIMITED_OBSERVATIONS_ONLY`

This report records the contract supported by the current `optionx_cpp`
implementation. It does not promote `trade_check2.php` to an authoritative
historical-tick API.

## Executive Decision

`trade_check2.php` may provide a trade-derived price observation for a trade
that the authenticated account owns and that the application already knows.
It cannot currently be used as a generic tick-history source:

- the request identifies one trade, not a symbol and time range;
- the response has no observation timestamp, symbol, bid, ask, last, or source
  sequence;
- the response does not prove that any interval between two observations was
  fully covered;
- the semantics of `price` are not named by the endpoint contract exposed to
  this repository.

Therefore no `range_complete=true` result, bar recovery source, or historical
tick archive may be built from this endpoint without new broker evidence.

## Observed Contract

`RequestManager::request_trade_check()` sends an authenticated POST to
`/trade_check2.php` with the following form body:

```text
user_id=<authenticated user id>
user_hash=<authenticated user hash>
trade_id=<numeric broker option id>
```

The public `TradeResultQuery` also carries a local `trade_id` and an
`option_hash`, but this Intrade request path ultimately requires a positive
numeric `option_id`. The local persistent ID and string hash are useful for
tracing and storage, but they are not sent as substitutes for `trade_id`.
Any synthetic IDs used in sanitized examples or isolated parser tests are
fixtures only; the production request path never fabricates, enumerates, or
substitutes a broker trade ID.

The implementation accepts a successful response only when the body contains
at least one semicolon and both sides parse as `double`:

```text
<price>;<profit>
```

The typed result is `TradeCheckInfo { price, profit }`. `price` is copied to
`TradeResult::close_price`. `profit` is interpreted by
`apply_trade_check_info_to_result()` as the broker-returned gross amount:

- gross amount approximately equal to the stake -> `STANDOFF`;
- gross amount near zero or below tolerance -> `LOSS`;
- otherwise gross amount minus stake is the net profit and the result is
  `WIN`.

This proves how the current client consumes the fields. It does **not** prove
that `price` is specifically bid, ask, last, midpoint, or a formally named
settlement field. Until the broker documents that meaning, the safest name is
`trade-derived close/reference price`.

## Polling Lifecycle

The normal `TradeManager::fetch_trade_result()` flow is:

1. Validate that the result has broker identity and a positive numeric
   `option_id`.
2. Call `request_trade_check_result()` with the configured retry count. The
   default `TradeResultQuery::retry_attempts` is 15, so there can be up to 16
   requests including the initial attempt.
3. `RequestManager` retries when the body is empty or contains the substring
   `error`, while the retry count remains positive. A transport/HTTP failure,
   malformed body, missing semicolon, or numeric parse failure terminates the
   check as failed.
4. On a parsed result, request the current account balance and publish the
   completed `TradeResult`.
5. A HTTP 451 result is surfaced as a blocked trade-result request and marks
   the account disconnected. Other failures remain trade-result errors.

All trade-result requests use `RateLimitType::TRADE_RESULT`, currently one
request every two seconds. The limiter is shared by the platform, so several
closing trades serialize their polling slots. This lifecycle is designed to
obtain the final result of known trades, not to scan a market-data interval.

## Related Data Sources

### Closed Trade History

`RequestManager::request_trade_history()` exposes closed trade records through
the configured CSV, HTML, or strict CSV+HTML source mode:

- CSV uses `/stat_trade_export.php` with account type and finite date fields;
- HTML reads the authenticated page and follows `/trade_load_more2.php` with
  the broker's pagination cursor;
- range filtering is applied locally using one selected `TradeRecord` time
  field (`place_date`, `send_date`, `open_date`, or `close_date`);
- HTML/CSV merging uses broker option IDs when available.

`TradeRecord` can contain an option ID, symbol, open/close prices, and open/
close dates. Those are trade lifecycle fields. They are not a sequence of
market observations and do not establish quote coverage between the dates.

### `/price_now`

`PriceManager` polls `GET /price_now`. The parser reads a broker symbol,
`ask`, `bid`, and `Updates`; the latter becomes the tick's broker `time_ms`,
while the local receipt time becomes `received_ms`. `PriceManager` records
these batches in `IntradeObservedTickHistory`.

That archive is bounded, in-memory, and session-scoped. It is cleared at
connect/disconnect boundaries and retains only observations that were actually
seen by polling. Its `range_complete` assertion means that every expected
one-second observation is present in the retained archive; it does not mean
that every broker micro-event was captured.

### Generic Tick History Contract

`TickHistoryRequest` is an inclusive `(symbol, from_time_ms, to_time_ms)`
request with an explicit source. `LOCAL_OBSERVED` preserves the existing
`/price_now` archive behaviour and remains the default. `BROKER_HISTORICAL`
selects the authenticated `/quotes` adapter. There is no automatic fallback or
merging between the sources. `TickHistoryResult::range_complete` is an explicit
provider proof, not an inference from the number of returned ticks.

## Legacy `/quotes` Tick-Check Evidence

The older `mega-connector` implementation contains a separate authenticated
trade-correction flow for the broker's quote-check page. It is not the same
request as `trade_check2.php` and it is not a generic public history API.

For this broker, the relevant contract is the execution-second grid: the
broker fixes trade opening and closing on whole seconds, so one broker value
per second is the authoritative granularity needed to verify a trade. This is
not a claim that the chart has no sub-second market updates; those updates are
outside the trade-settlement contract.

The current authenticated browser request is:

```text
GET /quotes?symbol=EUR%2FUSD&date=2026-10-30&from=22%3A30%3A00&to=22%3A45%3A00

symbol=<symbol with a slash, for example EUR/USD>
date=<YYYY-MM-DD in the broker's local date format>
from=<HH:MM:SS>
to=<HH:MM:SS>
```

The response is HTML. The current page describes the supported range as one
price per second for currencies and metals, with a maximum period of 30
minutes per request. Its table rows have the following shape:

```html
<tr><td>22:30:00</td><td>bid</td><td>price</td><td>ask</td></tr>
```

The current response contains only the local time in the first cell; the date
comes from the request's `date` parameter. Older responses containing a full
date and time remain accepted for compatibility. OptionX uses the middle
`Price` cell as the neutral `Tick::reference` value. Timestamps are converted
from the broker's observed UTC+3 clock to Unix time. The production caller
requests a window beginning at the stored trade open time and ending at the
stored close time plus one minute. It requires exact entries for both the open
and close seconds before it compares the broker-returned prices and may change
a reported loss or standoff to a win or standoff.

This establishes the source needed for broker trade verification. It does not
make the endpoint an unlimited raw-market-event archive:

- one request accepts at most 30 minutes and one local date, so longer ranges
  must be split into consecutive requests and ranges crossing local midnight
  must use separate dates;
- future periods are rejected explicitly and must not be interpreted as empty
  history;
- the page states that quotes are retained for several days, but does not
  publish an exact retention boundary or a cursor;
- the fixed `GMT+3` conversion is an implementation assumption, not a
  broker-declared timezone contract;
- the trade-correction caller treats missing open or close rows as failure,
  but it does not expose a reusable completeness result to market-data users.

OptionX keeps the broker-specific HTML and request quirks in the internal
`BrokerQuoteCheck` adapter, but exposes the observations through the common
`IntradeBarPlatform::fetch_tick_history()` operation when the request selects
`BROKER_HISTORICAL`. The adapter sends the authenticated `/quotes` GET on the
platform owner loop, limits each request to 30 minutes, splits ranges at
broker-local midnight, and merges the second-aligned rows. The public request
is conservatively limited to two broker-local dates until a larger retention
policy is explicitly needed.

The returned `TickSequence` keeps `provider=INTRADE_BAR` and reports the actual
source separately as `BROKER_HISTORICAL`. Because the selected quote column has
not been proven to be bid, ask, or last, its value is stored in the neutral
`Tick::reference` field. For partial-second requests the adapter returns only
whole-second timestamps inside the original inclusive range; it does not
fabricate rounded output timestamps.

For this source, `range_complete=true` means that every broker whole-second
timestamp inside the requested range was returned without malformed rows. It
does not prove that every sub-second market event is represented. A valid HTTP
200 response with no rows is successful but incomplete when the range contains
an expected second. Known login HTML, unrecognized non-empty HTML, malformed
rows, transport failures, and DDoS responses are failures rather than empty
history. No fallback to `LOCAL_OBSERVED` is attempted. This integration does
not change trade settlement or correction decisions.

An authenticated browser capture and read-only smoke confirmed the current
contract and its visible range boundary. A completed 15-minute EUR/USD request
returned 901 rows, and a completed one-hour request returned 3,601 rows via
two GET requests. A direct request for exactly 30 minutes returned 1,801 rows.
A request for 30 minutes plus one second returned HTTP 200 HTML with the form
validation text `Period of no more than 30 minutes: each second is a separate
row` and no quote rows. This confirms the 30-minute limit at the current
authenticated `/quotes` interface; it is not an assertion about undocumented
server behavior for other endpoints.

A future request returned HTTP 200 HTML with `This period has not ended yet.
Choose a time in the past.` A request crossing midnight returned an explicit
invalid time-order message because the form accepts only one date. These
responses are classified as broker validation failures rather than empty
history. No new trades are needed for this source.

The adapter's two-broker-date guard is a local safety limit, not evidence that
the broker retains exactly two days. The page only states that quotes are
retained for several days; the exact retention boundary remains unverified.

## Provenance Boundary

| Data | What it can establish | What it cannot establish |
| --- | --- | --- |
| Broker-observed `/price_now` tick | A symbol/price snapshot with broker time and local receipt time was observed | Complete micro-event history or observations before the session/archive window |
| Local `/price_now` archive | Retained observed snapshots and, for Intrade, proven one-second archive coverage | A persistent broker archive or coverage outside retained observations |
| Authenticated `/quotes` quote-check row | The broker's execution-second value for a requested symbol/time row | Sub-second market events, unlimited retention, or coverage outside the returned rows |
| `trade_check2.php` result | A price/profit response associated with one known broker trade ID | Exact price semantics, independent observation timestamp, range coverage, or absence of other ticks |
| `TradeRecord` history | Closed-trade lifecycle facts and stored open/close fields | A quote stream, intermediate ticks, or dense coverage |
| Authoritative historical tick range | Only a provider API that explicitly supplies timestamped observations and completeness semantics | Not provided by the currently observed `trade_check2.php` contract |

## Modern SPA Chart-History Evidence

A read-only inspection of the current `trading.intrade.bar` SPA found a
separate chart-history API. This is evidence for aggregate market history; it
does not replace the broker's `/quotes` execution-second contract.

The main observed request is:

```text
GET /ajax/instrument/chart-history
    instrumentId=<numeric instrument id>
    period=1m|5m|1h|1d
    limit=<number of candles>
    to=<optional ISO timestamp for older-page loading>
```

The following is an abbreviated response observed from the broker; only one of
the six returned `aggs` items is shown:

```json
{
  "instrumentId": 1,
  "period": "1m",
  "from": 1791596815,
  "to": 1792806415,
  "count": 6,
  "limit": 5,
  "aggs": [
    {
      "timestamp": 1792799940000,
      "open": 1.12025,
      "high": 1.1205,
      "low": 1.1202,
      "close": 1.1202
    }
  ]
}
```

The observed aggregate timestamps are milliseconds, while the response
`from`/`to` fields are seconds. The SPA normalizes and sorts `aggs` itself,
and uses an initial request of up to 1000 candles followed by older-page
requests with `limit=300` and `to` set to the oldest loaded candle. A request
with `limit=5` returned six aggregates in one observation, so `limit` must be
treated as a broker hint until its exact boundary semantics are established.

The current SPA bundle constructs this request with `to` as an ISO timestamp
when paging from an epoch-second cursor. In the unauthenticated live probe on
2026-10-28, a `limit=5` request returned six aggregates. Supplying an old ISO
`to` value returned five aggregates near that historical cursor plus an extra
aggregate at the current live edge. Supplying a future `to` within roughly a
week returned current-edge aggregates, while a farther future cursor returned
an empty list. This means callers must filter every aggregate to the requested
inclusive range and must treat future-date results as non-authoritative.

The same bundle calls the separate preview endpoint as:

```text
GET /ajax/instrument/history
    instruments=[<numeric instrument id>]
    wow=<preview mode>
    period=<preview period>
    points=<number of points>
```

Its response is a mapping from instrument ID to sampled
`[timestamp_ms, price]` pairs. A public probe returned ten points for ten
requested points, but the endpoint did not expose range or completeness
metadata. It is therefore not equivalent to either the legacy second-level
HTML table or the chart-history OHLC endpoint.

The loaded application bundles did not contain a dedicated quote-check or
trade-history chart endpoint. The chart code fetches candles by instrument and
timeframe, while trade data comes from `/ajax/user/trades` and is overlaid as
local open/closed trade markers. The inspected SPA code indicates that a
closed marker selects the corresponding position locally; the chart-history
request itself carries no trade ID.

Therefore the current evidence supports this narrower conclusion:

- the broker exposes a useful aggregate OHLC history API for the modern SPA;
- it is not a raw tick source and does not expose a `range_complete` field;
- the legacy `/quotes` flow is the broker-specific authoritative source for
  execution-second quote checks;
- `/ajax/instrument/history` is a sampled preview endpoint and does not
  provide a usable second-level trade-check history;
- it does not prove that a chart around a trade is a special trade-scoped
  quote-check request;
- exact `/quotes` retention remains unspecified beyond the page's "several
  days" statement;
- future periods are explicitly rejected, while missing rows in a completed
  range still require conservative completeness handling;
- the current page exposes `Bid`, `Price`, and `Ask`; OptionX uses `Price` as a
  neutral reference and does not infer settlement-side semantics from it.

The runtime integration can consume this dedicated `/quotes` adapter for known
trade open/close verification. It must not replace `trade_check2.php` or claim
complete sub-second market history. The modern
`chart-history` endpoint remains a separate minute-bar source.

## Hypothesis Review

| Question | Evidence in this repository | Conclusion |
| --- | --- | --- |
| What is `price`? | Stored as `TradeResult::close_price`; no field name or broker documentation identifies its market-price side | Trade-derived close/reference price only; bid/ask/last/settlement semantics unknown |
| When is it fixed? | Queried after the trade lifecycle reaches result checking; empty/error responses can be retried | Likely intended as a final-result value, but the endpoint does not prove when it becomes immutable |
| Can it be linked to symbol and exact timestamp? | The caller knows the local trade and option ID; response supplies neither symbol nor timestamp | Only a local association is possible; do not invent an observation timestamp |
| Can one ID return different values before/after settlement? | Polling explicitly tolerates empty/error responses and does not define idempotence | Possible; repeated values and finality require broker evidence |
| Is the value stable on repeated post-settlement requests? | No cache or stability assertion exists in the client contract | Unknown |
| Is it the same value as `/price_now`? | The two paths have different response contracts and no shared sequence or timestamp | Unproven; do not merge them as identical ticks |
| Can a stored `TradeRecord` reconstruct a historical tick? | It stores trade lifecycle fields, including close price/date | It can yield a trade-derived observation for analysis, not a broker tick |
| Can the endpoint fetch a symbol/time range without enumerating trades? | `trade_id` is required and there are no range parameters | No |
| Does it prove no observations existed between known points? | No coverage, cursor, or completeness field exists | No; `range_complete` must remain false |
| What do missing/expired/error responses mean? | Client reports typed failure, status, and in some cases disconnects on 451 | They are request/result failures, not evidence of market inactivity |

## Sanitized Fixtures

The following examples are contract-shaped and contain no credentials,
cookies, tokens, real account IDs, or real trade IDs. They are documentation
fixtures only; no live request is implied.

| Case | Request identity | Response | Permitted interpretation |
| --- | --- | --- | --- |
| Parsed result | `<owned numeric option id>` | `1.2345;1.80` | One trade-derived price/profit result |
| Empty pending response | `<owned numeric option id>` | empty body | Retryable pending/empty result while retry budget remains |
| Broker/application error | `<owned numeric option id>` | `error` or an error body | Retryable while budget remains, then typed failure |
| Malformed response | `<owned numeric option id>` | `1.2345` | Typed parse failure; no observation is created |
| HTTP failure | `<owned numeric option id>` | non-success status or transport failure | Typed request failure; not a market-data gap proof |

No fixtures were added to the test suite because a realistic successful
response requires a real owned trade ID and must not be fabricated. Existing
parser tests may use sanitized `price;profit` bodies to verify parsing, but
they must not promote those bodies to historical tick evidence.

## Permitted and Forbidden Uses

Permitted:

- validate the result of a known owned trade;
- populate `TradeResult::close_price`, state, profit, and balance follow-up;
- retain a clearly labelled trade-derived observation for audit or analysis
  when the local trade identity and local lifecycle timestamp are preserved;
- compare the value with `/price_now` as an explicitly unproven diagnostic.

Forbidden under the current evidence:

- using fabricated IDs, another user's IDs, or enumeration to populate history;
- opening extra trades to manufacture observations;
- treating `price` as bid, ask, mid, last, or settlement without broker proof;
- assigning the response an exact broker observation timestamp that it does not
  return;
- inserting the response into `fetch_tick_history()` as a normal tick;
- setting `TickHistoryResult::range_complete=true` from trade-derived values;
- claiming that a successful response proves no other quotes existed;
- increasing trade-result polling frequency to compensate for missing history.

## Follow-up Evidence Required For `IMPLEMENT`

An authoritative range-history implementation would require broker evidence
for all of the following: symbol/market identity, exact observation timestamp,
price semantics, ordering/cursor behaviour, retention and maximum range,
repeat-request stability, error/expired-session semantics, and a way to prove
or explicitly report incomplete coverage. Until that contract exists, the
correct architectural decision is `LIMITED_OBSERVATIONS_ONLY`, with
`trade_check2.php` kept on the trade-result side of the provenance boundary.
