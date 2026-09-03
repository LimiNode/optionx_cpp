# Intrade Bar HTTP Workflows

This map describes the observed request sequences in
`include/optionx_cpp/platforms/IntradeBarPlatform`. It intentionally does not
duplicate parser constants or broker response literals.

## Domain Discovery

Used before authentication when `AuthData::auto_find_domain` is enabled, and by
`BalanceManager` while disconnected.

1. `request_find_working_domain`
2. Iterate configured `*.intrade.bar` domain indexes. A negative minimum index
   excludes the primary `https://intrade.bar` host and scans the absolute
   numbered range.
3. For each candidate, call host availability logic.
4. On first success, publish `AutoDomainSelectedEvent` and continue the caller
   workflow.

## Email And Password Auth

Used by `AuthManager` for `AuthMethod::EMAIL_PASSWORD`.

1. Validate email/password are present.
2. Try `ServiceSessionDB` for saved `user_id/user_hash` cookies.
3. If saved cookies work:
   `request_profile` -> optional settings switches -> `request_balance`.
4. If no valid session exists:
   `request_main_page` -> `request_login` -> `request_auth`.
5. Store returned credentials in `ServiceSessionDB`.
6. Continue with:
   `request_profile` -> optional `request_switch_account_type` ->
   optional `request_switch_currency` -> `request_balance`.
7. On successful balance fetch, mark the account connected and publish account
   updates.
8. `ActiveTradesSyncManager` reacts to the connected account update and calls
   `request_active_trades_snapshot` to synchronize broker-side active trades
   with the trade queue counter.

Negative password/auth checks are manual smoke tests only: repeated failed
broker logins can lock the account for hours.

## User Token Auth

Used by `AuthManager` for `AuthMethod::USER_TOKEN`.

1. Validate `user_id` and token are present.
2. Set auth credentials on the request manager.
3. Run:
   `request_profile` -> optional `request_switch_account_type` ->
   optional `request_switch_currency` -> `request_balance`.

## Settings Switch Acknowledgement

For Intrade Bar, `ok` from the account-type or currency switch endpoint is
treated as broker acknowledgement. The auth workflow updates local account info
and continues without an immediate second `request_profile`.

This is intentional: live smoke coverage validates the broker contract, and an
extra profile request would add latency and request pressure to every successful
switch. If a future smoke test shows `ok` without an actual settings change,
add verification then.

## Connected Maintenance

Started after `AccountInfoUpdateEvent::CONNECTED`.

1. Every 15 minutes, `BalanceManager` calls `request_balance`.
2. Every 15 seconds, `BalanceManager` calls
   `request_check_current_host_available`.
3. On failed host check, mark disconnected.
4. While disconnected, retry host check using
   `AuthData::disconnected_domain_retry_period_ms`; if it fails, run domain
   discovery and then request balance again.

## Active Trades Sync

Started by `ActiveTradesSyncManager` after a connected account update, and also
when `RestartAuthEvent` indicates suspicious account state.

1. Call `request_active_trades_snapshot`, which fetches the authenticated main
   page and parses rows from the `trade_active` block.
2. Publish `OpenTradesSnapshotEvent` with the broker active-trade count and
   known close timestamps.
3. Stale callbacks are ignored if the active request generation or account
   identity (`user_id`, account type, currency) changed while the HTTP request
   was in flight.
4. `TradeQueueManager` accepts the snapshot only while its local pending/open
   queues are empty.
5. Snapshot trades are counted separately from locally opened trades. The
   public `OpenTradesEvent` reports the combined count.
6. For each known close timestamp, `TradeQueueManager` decreases the snapshot
   part after `AuthData::active_trades_close_buffer_ms`.
7. If the snapshot has active trades without known close timestamps, or the
   local queue is busy when the snapshot arrives, `TradeQueueManager` publishes
   `OpenTradesSnapshotRefreshRequestEvent`.
8. `ActiveTradesSyncManager` schedules the next broker snapshot after
   `AuthData::active_trades_sync_period_ms`.

`TradeQueueManager` applies snapshots on the platform event loop. Its pending
queue mutex protects external enqueueing, while local/snapshot open-trade
counters are event-loop-owned state.

## Price Polling

Started after `AccountInfoUpdateEvent::CONNECTED`.

1. `PriceManager` calls batch-native `request_price_batches` every second.
2. Failed price request slows the task to five minutes.
3. Successful price request restores the one-second period and publishes
   `PriceUpdateEvent`.

## Trade Open

Started from `TradeRequestEvent`.

1. `TradeQueueManager` releases queued requests no faster than
   `AuthData::order_interval_ms`.
2. `TradeManager` calls `request_execute_trade`.
3. On failure, mark trade `OPEN_ERROR`; HTTP 451 also disconnects the account.
4. On success, fill broker option id/open time/open price.
5. Call `request_balance`.
6. Balance success updates trade/account balance and marks trade `OPEN_SUCCESS`.

## Trade Result Check

Started from `TradeStatusEvent`.

1. Wait 500 ms.
2. Call `request_trade_check` with retry attempts.
3. Empty/temporary responses are retried inside `RequestManager`.
4. On failure, mark trade `CHECK_ERROR`; HTTP 451 also disconnects the account.
5. On success, call `request_balance`.
6. Combine close price, broker profit, payout rules, and balance to set final
   trade state.

## Trade History Export

Started from `BaseTradingPlatform::fetch_trade_history` or
`intrade_bar_smoke_cli history`.

1. `TradeManager::fetch_trade_history` validates the requested time range,
   reads the current account type from the connected account info, and reports
   success or failure through `TradeHistoryResult`.
2. `RequestManager::request_trade_history` chooses the source from
   `AuthData::trade_history_source`.
3. `CSV` calls `/stat_trade_export.php` with `name_method=stat_export`,
   `status_real`, `date1`, and `date2`, then parses closed financial
   `TradeRecord` entries.
4. `HTML` requests the authenticated main page, parses `trade_close`, reads the
   `trade_btn_load_more` `data-last` cursor, and keeps paging older closed rows
   through `/trade_load_more2.php` until the cursor is empty, repeats, the page
   is empty, or the requested start time is reached.
5. `HTML_CSV` requires both sources to succeed and returns only records that
   can be matched in both CSV and HTML. The merged records use CSV financial
   fields enriched with HTML broker IDs and timing details.

The HTML load-more endpoint does not take `status_real`; it follows the account
currently selected in the broker session. The auth/connect workflow must switch
the broker to the desired account before an HTML history request.

`TradeHistoryRequest::all()` disables client-side time filtering for full broker
export workflows; the CSV source still sends the broker a finite range starting
at `2000-01-01` because that endpoint requires dates. Ranged requests default
to `CLOSE_DATE`, which matches the usual closed-trade DB query semantics;
callers can choose another `TradeRecordTimeField` when they need a different
time axis. Records missing the selected timestamp are excluded from ranged
results. Intrade Bar closed-history sources provide a close timestamp, which is
stored in `TradeRecord::close_date`. Optional `TradeHistoryRequest::comment` is
copied to each returned `TradeRecord.comment`.

## Manual Trade Result Recovery Check

Started from `intrade_bar_smoke_cli open-check-result`.

1. Open a guarded demo sprint trade, usually `BTCUSDT`, amount `1`, duration
   `300` seconds.
2. Wait for the normal trade lifecycle to reach a terminal callback.
3. Create a fresh `TradeResult` containing only the recovered intermediate
   context: local trade id, broker option id, amount, account/currency, and
   open timing/price fields.
4. Call `BaseTradingPlatform::fetch_trade_result` with `TradeResultQuery`.
5. Compare the fetched terminal result with the lifecycle terminal result.

## Typed Adapter Surface

`RequestManager` keeps the old callback methods intact. New `*_result` methods
wrap the same request/parser path into `ApiResult<T>` payloads from
`ApiResponses.hpp`, so other HTTP brokers can reuse a similar typed workflow
shape without changing Intrade Bar parser literals.

## Live Smoke Coverage

`intrade_bar_auth_smoke_test` covers these online mechanisms independently:

1. Proxy guard before any credentialed broker request.
2. Successful authentication through the configured proxy.
3. Fresh login followed by cached-session login using `ServiceSessionDB`.
4. Account type, currency, balance, and one live quote snapshot after auth.

`intrade_bar_smoke_cli` exposes the same pieces manually through `auth`,
`auth-cache`, `show-account`, `quotes`, `history`, guarded `open-trade`,
`open-trades-sync-check`, and `open-check-result` commands. Use
`open-trades-sync-check` when validating broker active-trade snapshots,
reconnect counter synchronization, and close-time countdown behavior. Use
`--count=2` or more with `--interval-ms=...` when validating staggered
close-time countdowns.
