# Intrade Bar API Smoke Tests

These tests are meant for broker workflow smoke checks, not for parser fixtures.
Parser string constants stay in the platform implementation.
See `WORKFLOWS.md` for the current request sequence map.
See `CLI.md` for the `intrade_bar_smoke_cli` command reference.

Set configuration through environment variables, or point
`OPTIONX_INTRADE_BAR_CONFIG_FILE` to an untracked `*.local.env` file in this
directory. Use `intrade_bar_api.env.example` as the list of supported keys.

Automatic auth tests refuse to run with credentials unless proxy settings are
present. Negative-auth scenarios are intentionally manual because failed login
attempts can lock the broker account for hours.

Quick CLI smoke helper:

```powershell
$env:OPTIONX_INTRADE_BAR_CONFIG_FILE="tests\intrade_bar_api\intrade_bar_api.local.env"
.\build-codex\intrade_bar_smoke_cli.exe show-account
.\build-codex\intrade_bar_smoke_cli.exe auth-cache
.\build-codex\intrade_bar_smoke_cli.exe domain-check --domain-min=-1 --domain-max=1000
.\build-codex\intrade_bar_smoke_cli.exe quotes --symbol=EURUSD
.\build-codex\intrade_bar_smoke_cli.exe tick-history --symbol=EURUSD --days-ago=0 --seconds=65
.\build-codex\intrade_bar_smoke_cli.exe market-stream --symbols=BTCUSDT --ticks --bars --transport=WEBSOCKET --seconds=60
.\build-codex\intrade_bar_smoke_cli.exe history --source=CSV --days=14 --account-type=DEMO
.\build-codex\intrade_bar_smoke_cli.exe history --source=HTML_CSV --all --account-type=DEMO --comment=account-history-export
.\build-codex\intrade_bar_smoke_cli.exe switch-check --confirm --account-type=DEMO --currency=USD
.\build-codex\intrade_bar_smoke_cli.exe open-trade --confirm --symbol=EURUSD --amount=1 --duration=60 --buy
.\build-codex\intrade_bar_smoke_cli.exe open-trades-sync-check --confirm --symbol=BTCUSDT --amount=1 --duration=300 --count=1
.\build-codex\intrade_bar_smoke_cli.exe open-check-result --confirm --symbol=BTCUSDT --amount=1 --duration=300 --buy
```

For RF/CIS domain blocking checks, keep `OPTIONX_INTRADE_BAR_AUTO_FIND_DOMAIN=1`
and use a negative `OPTIONX_INTRADE_BAR_DOMAIN_MIN` to skip the primary
`https://intrade.bar` host.

`open-trade`, `open-trades-sync-check`, and `open-check-result` require
`--confirm` or `OPTIONX_INTRADE_BAR_ALLOW_TRADE=1`.
Real-account trades are refused unless both `--allow-real` and
`OPTIONX_INTRADE_BAR_ALLOW_REAL_TRADE=1` are set.
Set `OPTIONX_INTRADE_BAR_CLI_CONSOLE_LOG=1` when you want internal workflow
logs mirrored to the console; otherwise CLI output stays compact and logs go to
the configured log files.
