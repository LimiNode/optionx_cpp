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
request. `TickHistoryResult::range_complete` is an explicit provider proof,
not an inference from the number of returned ticks. Intrade implements this
contract from the `/price_now` archive, not from `trade_check2.php`.

## Provenance Boundary

| Data | What it can establish | What it cannot establish |
| --- | --- | --- |
| Broker-observed `/price_now` tick | A symbol/price snapshot with broker time and local receipt time was observed | Complete micro-event history or observations before the session/archive window |
| Local `/price_now` archive | Retained observed snapshots and, for Intrade, proven one-second archive coverage | A persistent broker archive or coverage outside retained observations |
| `trade_check2.php` result | A price/profit response associated with one known broker trade ID | Exact price semantics, independent observation timestamp, range coverage, or absence of other ticks |
| `TradeRecord` history | Closed-trade lifecycle facts and stored open/close fields | A quote stream, intermediate ticks, or dense coverage |
| Authoritative historical tick range | Only a provider API that explicitly supplies timestamped observations and completeness semantics | Not provided by the currently observed `trade_check2.php` contract |

## Modern SPA Chart-History Evidence

A read-only inspection of the current `trading.intrade.bar` SPA found a
separate chart-history API. This is evidence for aggregate market history; it
does not change the `trade_check2.php` decision above.

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

The SPA also exposes `/ajax/instrument/history`, which returns a mapping from
instrument ID to `[timestamp_ms, price]` pairs for asset previews. It is a
different response contract and is not a tick-history completeness proof.

The loaded application bundles did not contain a dedicated quote-check or
trade-history chart endpoint. The chart code fetches candles by instrument and
timeframe, while trade data comes from `/ajax/user/trades` and is overlaid as
local open/closed trade markers. The inspected SPA code indicates that a
closed marker selects the corresponding position locally; the chart-history
request itself carries no trade ID.

Therefore the current evidence supports this narrower conclusion:

- the broker exposes a useful aggregate OHLC history API for the modern SPA;
- it is not a raw tick source and does not expose a `range_complete` field;
- it does not prove that a chart around a trade is a special trade-scoped
  quote-check request;
- exact retention, maximum range, timezone, missing-candle semantics, and
  price-source semantics still require authenticated UI replay with a real
  closed trade.

No runtime provider should be implemented from this observation alone. If the
authenticated replay confirms the same contract, it is a candidate for a
separate provider-aligned bar-history adapter, not for `fetch_tick_history()`
or for replacing `trade_check2.php`.

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
