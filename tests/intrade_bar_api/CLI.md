# Intrade Bar Smoke CLI

`intrade_bar_smoke_cli` is a manual smoke tool for checking individual Intrade
Bar HTTP workflows through the normal platform components. It is not a parser
fixture and it does not bypass proxy/auth guards.

## Build

Configure and build tests from the repository root:

```powershell
cmake -G "MinGW Makefiles" -S . -B build-codex -DBUILD_DEPS=ON -DBUILD_TESTS=ON "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
cmake --build build-codex --target intrade_bar_smoke_cli -- -j4
```

The executable is created here:

```text
build-codex\intrade_bar_smoke_cli.exe
```

## Configuration

Create an untracked local env file from the example:

```powershell
Copy-Item tests\intrade_bar_api\intrade_bar_api.env.example tests\intrade_bar_api\intrade_bar_api.local.env
```

Fill in credentials and proxy in `*.local.env`, then point the CLI to it:

```powershell
$env:OPTIONX_INTRADE_BAR_CONFIG_FILE="E:\_repoz\optionx_cpp\tests\intrade_bar_api\intrade_bar_api.local.env"
```

The CLI also reads normal environment variables. Environment variables override
values from `OPTIONX_INTRADE_BAR_CONFIG_FILE`.

Required for broker access:

```text
OPTIONX_INTRADE_BAR_EMAIL
OPTIONX_INTRADE_BAR_PASSWORD
OPTIONX_INTRADE_BAR_PROXY
```

`OPTIONX_INTRADE_BAR_PROXY` uses this format:

```text
host:port:user:password
```

Alternatively set proxy pieces separately:

```text
OPTIONX_INTRADE_BAR_PROXY_SERVER
OPTIONX_INTRADE_BAR_PROXY_AUTH
OPTIONX_INTRADE_BAR_PROXY_TYPE
```

Automatic CLI broker commands refuse to run with credentials unless proxy
settings are present.

## Commands

Show command list:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe help
```

Check authorization:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe auth
```

Check fresh login vs saved session:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe auth-cache
```

Expected output shape:

```text
fresh_ms=8336 cached_ms=3215 session_saved=1 faster=1
```

Show account state after auth:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe show-account
```

Expected output shape:

```text
auth callback=1 success=1 elapsed_ms=3468
connected=1 account_type=DEMO currency=USD balance=10000 open_trades=0
```

Check automatic domain discovery:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe domain-check --domain-min=-1 --domain-max=1000
```

`domain-check` temporarily enables `auto_find_domain`, runs the normal
authorization flow through proxy, and prints the selected host reported by the
platform's `AutoDomainSelectedEvent`.

Expected output shape:

```text
domain_check auto_find_domain=1 domain_min=-1 domain_max=1000 timeout_ms=90000
auth callback=1 success=1 elapsed_ms=4200
domain selected=1 success=1 host=https://intrade35.bar
connected=1 account_type=DEMO currency=USD balance=10000 open_trades=0
```

For RF/CIS environments, keep automatic domain discovery enabled and exclude
the primary `https://intrade.bar` domain when it is blocked locally:

```text
OPTIONX_INTRADE_BAR_AUTO_FIND_DOMAIN=1
OPTIONX_INTRADE_BAR_DOMAIN_MIN=-1
OPTIONX_INTRADE_BAR_DOMAIN_MAX=1000
```

Use a negative `domain-min` when the primary `https://intrade.bar` domain must
be excluded from discovery:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe domain-check --domain-min=-1 --domain-max=1000
.\build-codex\intrade_bar_smoke_cli.exe domain-check --domain-min=-35 --domain-max=35
```

Get one quote snapshot:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe quotes --symbol=EURUSD
```

`BTCUSD` is accepted as an alias for the broker-side `BTCUSDT` symbol:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe quotes --symbol=BTCUSD
.\build-codex\intrade_bar_smoke_cli.exe quotes --symbol=BTCUSDT
```

Expected output shape:

```text
auth callback=1 success=1 elapsed_ms=3639
EURUSD bid=1.14911 ask=1.14914 mid=1.14913
```

Smoke live market-data streams:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe market-stream --symbols=BTCUSDT --ticks --bars --transport=WEBSOCKET --seconds=60 --timeframe=60
.\build-codex\intrade_bar_smoke_cli.exe market-stream --symbols=BTCUSDT,EURUSD --type=both --transport=AUTO --seconds=120
```

`market-stream` uses the public market-data API. It applies tick
subscriptions, prints stream status updates, and prints every delivered tick
batch. When `--bars` is set, it also:

- applies provider-level live bar subscriptions backed by tick-to-bar
  aggregation and prints the typed result;
- builds local realtime OHLC updates from the live tick stream for terminal
  inspection;
- requests a historical/backfill bar batch through `MarketDataContinuityService`
  unless `--no-backfill` is passed.

Useful options:

```text
--symbols=BTCUSDT,EURUSD
--type=ticks
--type=bars
--type=both
--ticks
--bars
--transport=WEBSOCKET
--transport=POLLING
--transport=AUTO
--transport=HYBRID
--seconds=60
--timeframe=60
--price=MID
--price=BID
--price=ASK
--price=LAST
--backfill-minutes=30
--no-backfill
```

Expected output includes lines like:

```text
[subscribe-ticks] status=APPLIED results=1
[status] type=TICKS symbol=BTCUSDT transport=WEBSOCKET status=READY
[tick] symbol=BTCUSDT time_ms=... bid=... ask=... flags=LIVE_SOURCE|REALTIME
[bar-update] symbol=BTCUSDT timeframe=60 ... flags=LIVE_SOURCE|REALTIME|INCOMPLETE
[bar-batch] symbol=BTCUSDT timeframe=60 items=...
```

Intrade live bars are built from the selected tick transport. The CLI also
keeps its local aggregation output visible so manual smoke runs can compare the
provider-level bar batches against the raw tick stream.

Fetch closed trade history:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe history --source=CSV --days=14 --account-type=DEMO
.\build-codex\intrade_bar_smoke_cli.exe history --source=HTML_CSV --all --account-type=DEMO --comment=account-history-export
```

History source modes:

```text
CSV       use /stat_trade_export.php; best financial result coverage
HTML      parse the authenticated page trade_close block and page older rows
          through /trade_load_more2.php
HTML_CSV  return only rows found in both CSV and HTML, using CSV financial
          fields enriched with matching HTML broker IDs as TradeRecord entries
```

`HTML` follows the broker UI flow: `GET /` gives the recent closed trades and
the `trade_btn_load_more` `data-last` cursor, then `POST /trade_load_more2.php`
continues while the broker returns older rows. The endpoint uses the currently
selected account on the broker side, so connect the platform with the desired
account type before requesting HTML history.

The table HTML does not expose every field. For example, `option_type` can stay
`UNKNOWN` in pure `HTML` mode. Use `HTML_CSV` when you need rows verified by
both broker sources. Use `CSV` when you intentionally want the widest export
coverage, including rows no longer visible in HTML pagination.

The default source is configurable:

```text
OPTIONX_INTRADE_BAR_TRADE_HISTORY_SOURCE=CSV
```

Useful options:

```text
--source=CSV
--source=HTML
--source=HTML_CSV
--days=14
--all
--from-ms=1719000000000
--to-ms=1719600000000
--time-field=CLOSE_DATE
--time-field=OPEN_DATE
--range-mode=CLOSED
--range-mode=HALF_OPEN
--account-type=DEMO
--comment=account-history-export
--timeout-ms=90000
```

`--account-type` selects the account used during authentication. The history
request itself uses the account that is currently selected in the broker
session. By default the time range filters by `CLOSE_DATE`; `--all` disables
time filtering and asks the broker for all available rows. For the CSV source,
the adapter maps this to the broker-required date range starting at
`2000-01-01`. `--comment` is copied to each returned `TradeRecord.comment`.
Rows without the selected `--time-field` timestamp are excluded from ranged
requests. Intrade Bar history currently provides `OPEN_DATE` and `CLOSE_DATE`;
the adapter stores the broker close timestamp in `TradeRecord::close_date`.

The command prints a compact summary to stdout and writes every fetched record
to the normal log files under `build-codex\data\logs\`.

Check automatic account/currency recovery:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe switch-check --confirm --account-type=DEMO --currency=USD
```

`switch-check` first connects with the target settings, then temporarily
switches the broker to opposite settings through a second runtime. The original
runtime must notice the mismatch through its connected balance poll and recover
back to the target account type/currency.

If the broker account refuses switching to REAL, use a currency-only source
state while still verifying the same recovery path:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe switch-check --confirm --account-type=DEMO --currency=USD --from-account-type=DEMO --from-currency=RUB
```

Useful options:

```text
--account-type=DEMO
--currency=USD
--from-account-type=REAL
--from-currency=RUB
--timeout-ms=120000
```

For smoke runs the connected balance poll is intentionally fast:

```text
OPTIONX_INTRADE_BAR_BALANCE_CHECK_PERIOD_MS=15000
OPTIONX_INTRADE_BAR_DISCONNECTED_DOMAIN_RETRY_PERIOD_MS=15000
OPTIONX_INTRADE_BAR_SETTINGS_SWITCH_TIMEOUT_MS=120000
OPTIONX_INTRADE_BAR_SETTINGS_SWITCH_RETRY_TIMEOUT_MS=600000
OPTIONX_INTRADE_BAR_SETTINGS_SWITCH_RETRY_DELAY_MS=15000
OPTIONX_INTRADE_BAR_SETTINGS_SWITCH_ACTIVE_TRADE_BUFFER_MS=5000
OPTIONX_INTRADE_BAR_ORDER_INTERVAL_MS=1000
```

When testing switch retries blocked by open trades, make the auth timeout longer
than the longest demo trade:

```powershell
$env:OPTIONX_INTRADE_BAR_AUTH_TIMEOUT_MS="480000"
$env:OPTIONX_INTRADE_BAR_SETTINGS_SWITCH_RETRY_TIMEOUT_MS="480000"
```

Open a guarded demo trade:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe open-trade --confirm --symbol=EURUSD --amount=1 --duration=60 --buy
```

Other trade options:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe open-trade --confirm --symbol=EURUSD --amount=1 --duration=60 --sell
```

`open-trade` will not run unless one of these is true:

```text
--confirm
OPTIONX_INTRADE_BAR_ALLOW_TRADE=1
```

Real-account trades are refused unless both are set:

```text
--allow-real
OPTIONX_INTRADE_BAR_ALLOW_REAL_TRADE=1
```

Keep `OPTIONX_INTRADE_BAR_ACCOUNT_TYPE=DEMO` for routine smoke checks.

Check active-trade counter synchronization:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe open-trades-sync-check --confirm --symbol=BTCUSDT --amount=1 --duration=300 --count=1
```

`open-trades-sync-check` opens guarded demo trades, verifies that the local
counter increments, reconnects, waits for the broker active-trades snapshot,
and then waits for the snapshot counter to decrease after the planned close
time. Use `BTCUSDT` for weekend/holiday checks; use `EURUSD` with a shorter
duration while the normal market is open:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe open-trades-sync-check --confirm --symbol=EURUSD --amount=1 --duration=180
```

To observe staggered close-time countdowns, open several trades with a gap
between attempts:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe open-trades-sync-check --confirm --symbol=BTCUSDT --amount=1 --duration=300 --count=5 --interval-ms=15000 --min-accepted=1
```

Useful options:

```text
--symbol=BTCUSDT
--amount=1
--duration=300
--count=1
--interval-ms=15000
--min-accepted=1
--order-interval-ms=1000
--close-buffer-ms=1000
--sync-period-ms=15000
--snapshot-timeout-ms=30000
--local-counter-timeout-ms=10000
--close-timeout-ms=420000
--buy
--sell
```

`--interval-ms` is the CLI pause between requested trades. `--order-interval-ms`
configures `AuthData::order_interval_ms`, which the trade queue uses as the
minimum delay between broker order requests.

Open a guarded demo trade and then recover its final result by broker ID:

```powershell
.\build-codex\intrade_bar_smoke_cli.exe open-check-result --confirm --symbol=BTCUSDT --amount=1 --duration=300 --buy
```

`open-check-result` first waits for the normal lifecycle callback to reach a
terminal state. Then it creates a new `TradeResult` with only the intermediate
fields a restarted bot would reasonably know: local trade id, broker option id,
amount, account/currency, and open timing/price fields. The command calls
`fetch_trade_result` with `TradeResultQuery` and compares the fetched state and
profit with the lifecycle result.

Useful options:

```text
--symbol=BTCUSDT
--amount=1
--duration=300
--buy
--sell
--result-timeout-ms=420000
--retry-attempts=15
```

The default result timeout can also be set globally:

```text
OPTIONX_INTRADE_BAR_TRADE_RESULT_TIMEOUT_MS=420000
```

## Logs And Data

Because the executable lives in `build-codex`, runtime data is created under:

```text
build-codex\data\
```

Session DB:

```text
build-codex\data\db\session_data\mdbx.dat
build-codex\data\db\session_data\mdbx.lck
```

Logs:

```text
build-codex\data\logs\
build-codex\data\logs\unique_logs\
```

By default CLI stdout stays compact and workflow logs go to log files. To mirror
internal workflow logs to console:

```powershell
$env:OPTIONX_INTRADE_BAR_CLI_CONSOLE_LOG="1"
```

Turn it off again:

```powershell
Remove-Item Env:OPTIONX_INTRADE_BAR_CLI_CONSOLE_LOG -ErrorAction SilentlyContinue
```

## Exit Codes

```text
0  command succeeded
1  broker workflow failed or timed out
2  CLI usage/configuration guard refused to run
3  auth-cache succeeded but cached login was not faster
```

## Safety Notes

Do not use automated negative-auth checks with the real broker account. Failed
login attempts can lock the account for hours.

Do not commit `*.local.env`. The repository ignores
`tests/intrade_bar_api/*.local.env` for this reason.
