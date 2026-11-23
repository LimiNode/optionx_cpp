# Trading System Batch 7B: Pure Platform DTO Mapper

## Scope And Ownership

Based on Batch 7A, merged in PR #193 as
`e81a43db85a31d6828fac72422e0fc5d384e718a`. Its merged tree exactly matches
reviewed HEAD `a48b348a0c5c861972c93318ad5e417b6ad0c6de`.
This slice contains pure, internal IntradeBar DTO conversions, not execution.

Include `<optionx_cpp/trading_system.hpp>`, which owns the complete include
closure. [PlatformDtoMapper.hpp](../include/optionx_cpp/trading_system/detail/PlatformDtoMapper.hpp)
is an internal leaf in `optionx::trading_system::detail::platform_mapping`.
Neither that namespace nor its types are a new supported public API. There is
no new facade, platform manager, scheduler, callback mechanism, dependency,
storage table, wire codec or migration.

The mapper does not call platforms, allocate trade/settlement IDs, apply durable
receipts, alter reservations or advance series/MM. Batch 7B was accepted at
`dad7bcf891f08323c8f4f07681dc660137cab197` and merged in PR #194 as
`6e5f0adac14ddb5aa135dc5b67a2d108a2d7f1cc`. The separate
[7C-A evidence slice](trading-system-batch7c-a.md) requires its own Draft review;
7C-B/7D and live/demo execution remain unimplemented and gated.

## Request Mapping

`map_request(intent, roster, operation, account, profile)` accepts existing
`TradeIntent`, `ExecutionRosterRecord`, `ExecutionOperationRecord` and
`AccountIdentityRecord` snapshots. It validates their intent/group/member,
account and risk-scope bindings. A fixed roster is mandatory; translation is
not proof of admission, freshness, platform availability or permission to send.

The physical roster stake is authoritative, not the logical template's amount.
Template trade/account/realm/currency/source IDs must be absent or consistent
when specified. Physical IDs must fit `uint32_t`, and account IDs must fit the
positive `int64_t` request field before casting. Alias IDs remain aliases; they
are not rewritten to the canonical account. The caller retains the operation
and client-operation IDs; local request metadata is not broker idempotency.

The configured Intrade account metadata accepts the existing `IntradeBar` and
`INTRADE_BAR` spellings and explicit `DEMO`/`REAL` realms. Unsupported platform
bindings fail explicitly. Deactivated accounts cannot produce new requests.
No new account equivalence or registry logic is introduced.

The result is a fresh callback-free `TradeRequest` copy. Existing source and
bridge metadata, order/option type, duration/expiry in seconds and correlation
fields survive. No additional timezone offset is applied. The profile does not
assert broker support for every currency or instrument; existing platform
validation and account capabilities remain authoritative.

## Observation Mapping

`map_observation(result, saved_request, saved_operation, saved_account, profile)`
returns an owned `PlatformObservation` even when translation fails. It retains
the original DTO, error code/description, numeric broker ID/hash, operation and
account snapshots and selected normalization profile. Failed normalization does
not erase confirmed opening or the original financial evidence.

The caller must supply the **captured** operation/account binding for this
physical request, including its identity revision. `TradeResult` has no account
ID or operation ID: a pure mapper cannot authenticate a callback's origin or
detect substitution of an otherwise consistent entire binding. Do not look up
a newly rebound account, infer ownership from list position or use a broker ID
as the local physical ID. Receipt generation/revision checks belong to 7C.
Historical observations remain admissible for a deactivated account tombstone.

Only `trade_state` supplies financial evidence; `live_state`, balance estimates,
payout and price projections are retained but never promoted to settlement.

| State | Internal observation |
| --- | --- |
| `UNKNOWN`, `WAITING_OPEN` | Progress only; no opening or money assertion. |
| `OPEN_ERROR` | Failed opening with original error; no settlement, no `NOT_SUBMITTED` assertion and no blanket unknown-execution conversion. |
| `OPEN_SUCCESS`, `IN_PROGRESS`, `WAITING_CLOSE` | Opening confirmed; missing numeric ID remains an identity-reconciliation condition, not failed opening. |
| `CHECK_ERROR` | Result-check failure, not independent proof of opening. Preserve previously established acceptance; nonzero estimated profit is not a final outcome. |
| `WIN`, `LOSS`, `STANDOFF` | Checked native net P&L and WIN/LOSS/DRAW, only after identity, stake, numeric and sign validation. |
| `REFUND` | This initial full-refund profile accepts only exactly zero raw net P&L as DRAW; original REFUND is retained. Nonzero, even if it would round to zero, is unsupported. |
| `CANCELED_TRADE` | Cancellation notification only, not proof that execution was impossible. |
| Unknown numeric state | Explicit unsupported-state error and raw value retained. |

For IntradeBar, `TradeResult::profit` is already net P&L after
`apply_trade_check_info_to_result()`: do not subtract stake again. A financial
fact also requires a positive numeric broker ID, either reported now or already
bound durably. The saved string form is the canonical decimal numeric ID;
conflicting IDs, negative IDs, overflow and malformed saved IDs fail. The hash
is retained separately and does not substitute for Intrade's numeric check ID.

`proves_opening` is a positive assertion made by this observation, not the
stored operation's acceptance state. A false value does not prove non-execution
or instruct the future durable owner to clear earlier acceptance. `CHECK_ERROR`
always leaves it false, even with a reported or saved numeric broker ID: an ID
alone supplies correlation, not independent proof of execution. The captured
operation and raw error remain unchanged; no automatic `OPEN_ERROR` or
`EXECUTION_UNKNOWN` conversion occurs.

This distinction follows existing platform behavior, not a new HTTP rule:
`TradeQueueManager` can finalize a still-unconfirmed `WAITING_OPEN` operation
as `CHECK_ERROR / CLIENT_FORCED_CLOSE`, and `TradeManager::fetch_trade_result()`
can return `CHECK_ERROR / INVALID_REQUEST` without a numeric broker ID. Neither
establishes opening. Conversely, an earlier `OPEN_SUCCESS` or durable
`BROKER_ACCEPTED` / `BROKER_ID_PERSISTED` fact is not demoted by a later check
failure. A subsequent validated WIN/LOSS remains financial evidence.

Identical inputs produce equivalent data. No clock, random source, counter or
SettlementId is used. Observation kinds are classifications, not another
execution state machine. The future owner must apply receipts monotonically;
a late error/progress observation cannot demote an already settled operation.

## Native Money Profile

`MoneyProfile` explicitly specifies a known `CurrencyType` and decimal precision
in `[0, 18]`, matching the existing `MoneyAmount` range. There is no implicit
two-decimal profile or FX conversion. Recognized currencies are the current
USD/EUR/GBP/BTC/ETH/USDT/USDC/RUB/UAH/KZT enum values; UNKNOWN and unrecognized
numeric values fail. Required account currency/provenance remains host-owned.

Normalization rule `BINARY64_FIXED_NEAREST_EVEN_V1` interprets the **exact input
IEEE 754 binary64 value**, not an imagined original decimal spelling. C++17
`to_chars(fixed, precision)` rounds to the nearest decimal grid point, ties to
even, independently of locale and the active floating-point rounding mode.
The existing checked `MoneyAmount::from_decimal()` then parses that fixed
representation. There is no intermediate multiplication by a floating scale,
epsilon adjustment or `long double` precision dependency. For example, 1.125
at precision 2 becomes 1.12, 1.375 becomes 1.38, and the actual binary64 value
of the literal 2.675 becomes 2.67. This rule does not recover discarded wire
decimal information. In 7C, every durable decision/evidence record using this
conversion must retain the immutable normalization version
`BINARY64_FIXED_NEAREST_EVEN_V1`, selected currency/precision profile and
original numeric facts together. Replay must use that recorded version, never
silently reinterpret old facts under newer rules; unsupported versions fail
closed. This is a future persistence requirement, not a 7B codec/schema change.

NaN/infinity, currency/precision mismatches and fixed-point overflow fail
explicitly. WIN/LOSS must have the right sign both before and after rounding;
an unrepresentably small win/loss is not fabricated as DRAW. STANDOFF and a full
REFUND require exactly zero raw profit. Native stake/result amounts must match
on the configured grid. For outgoing stake, `from_chars` parses the existing
exact fixed-point decimal; normalization must reproduce the original units or
translation fails as lossy. This does not change `MoneyAmount`'s public codec.

## Failed-open Evidence: Historical 7B Proposal

No representation below was implemented by 7B. In particular, its
`TradeSettlement::valid()`, storage schema version and numeric evidence codec
remain unchanged. The existing simulated pre-submission rejection continues
to mean `NOT_SUBMITTED`.

| Concern | A: Append a proven-no-execution evidence value | B: Separate failed-open operation receipt |
| --- | --- | --- |
| Semantics | Distinguishes an attempted command that conclusively did not execute from a never-submitted command. Adapter provenance must qualify the evidence. | Records the attempted opening and refusal as a typed operation fact outside settlement. |
| `TradeSettlement::valid()` | Narrowly allow REJECTED with the new evidence and no financial P&L. Do not weaken UNKNOWN/EXECUTED validation or automatically extend CANCELLED. | Leave settlement validation unchanged; add a separate validated path for physical non-execution completion. |
| Codec/schema/replay | Preserve existing evidence ordinals 0/1/2; append an explicitly numbered value. Review schema/capability versioning before writing it. Old readers must fail closed, never reinterpret it as 0. | Version the receipt envelope and its canonical journal entry. Existing settlement readers remain valid, but older recovery cannot reconstruct completion from the new fact. |
| Rollback/release | Reuse the settlement/group/series/reservation UOW with qualified provenance; commit rejection and release together, or neither. | Commit receipt, physical completion and release in one UOW; recovery and group completion need another input path. |
| Idempotency | Reuse canonical physical terminal evidence and settlement deduplication; differing evidence/provenance is a conflict, not a new attempt. | Key by captured operation/member/receipt identity and compare evidence; avoid a second terminal ledger disagreeing with settlements. |
| Migration | Existing records retain their meaning. Add reader compatibility/upgrade gates; never infer historical proof from an error string. | Add receipt storage/projection rebuild and recovery compatibility gates; old records still require their original settlement path. |

**7B recommended A for a separate evidence review**, because it reuses the existing
single terminal ledger and atomic completion path. A possible name is
`NOT_EXECUTED_PROVEN` with explicit ordinal 3; neither spelling nor schema
version is approved here. Audit `settlement_to_json/from_json` (currently numeric
evidence), hydration, terminal comparison, reservation release and
`reject_without_submission()` before changing them. Do not rename/reinterpret
NOT_SUBMITTED in place or classify an HTTP attempt as never sent.
The subsequent [7C-A contract](trading-system-batch7c-a.md) selects A with
ordinal 3 and a settlement-local v2 gate, preserving global schema version 1.

Required review tests: old-record replay, unknown/new evidence under old reader
and upgrade gates, qualified broker refusal versus missing callback, duplicate
and conflicting terminal evidence, crash/rollback at every release/group/MM
write, and no second progression or automatic resend. Absence of execution
must come from the established adapter contract, not HTTP heuristics invented
by Trading System. Keep current IntradeBar OPEN_ERROR semantics unchanged.

## Verification Boundaries

[Mapper tests](../tests/trading_system_batch7b_test.cpp) and the
[C++17 aggregate consumer](../tests/trading_system_batch7b_consumer_test.cpp)
exercise pure translation without platform instances, callbacks, MDBX files,
credentials or broker traffic. Existing parser/result-query/lifecycle and
Batch 1-6 regressions remain separate checks. Ubuntu CI adds result-query and
lifecycle tests; Windows CI adds only mapper/consumer to its existing matrix.
Local MinGW GCC 15.2 and MSVC 19.44 passed all 41 mapper tests and the C++17
consumer. Exact-head CI and full regression results are recorded in the PR;
these offline checks do not establish new external broker guarantees.

Before any 7B changes, local MSVC builds of `intrade_bar_api_response_test` and
`trade_result_query_test` failed in existing `mdbx_containers/detail/utils.hpp`
(C2632/C2059) and `IntradeBarPlatform/RequestManager.hpp` (C3493, uncaptured local
constants). This is a recorded baseline limitation of those platform targets,
not a claim that Windows CI exercises them. No workaround or platform/header
refactor is included. MinGW/Ubuntu cover these platform regressions; the new
mapper and domain aggregate are checked independently on MSVC.

### Separate MSVC Platform Baseline

Reproduce from the repository root with the existing VS 2022/MSVC 19.44 Debug
build (`build-codex-msvc`). Run each target separately, since a failed target
stops a multi-target build:

```powershell
cmake --build build-codex-msvc --config Debug --target intrade_bar_api_response_test --parallel 2
cmake --build build-codex-msvc --config Debug --target trade_result_query_test --parallel 2
```

Diagnostics observed before 7B and reproduced during the corrective pass:

- `build-codex-msvc/include/mdbx_containers/detail/utils.hpp:201`: C2632
  (`char char`) and C2059, with cascading errors at lines 235/237.
- `include/optionx_cpp/platforms/IntradeBarPlatform/RequestManager.hpp:2113`
  and `:2671`: C3493 for uncaptured `domain_check_batch_size` and
  `max_html_history_pages`.

These are platform-header build failures, not an independently established
third-party root cause. They do not make the pure mapper/MSVC tests fail and
are not fixed by this PR. Before 7D, resolve them in separately reviewed work
or establish a reproducible isolated Windows-platform verification path.
Passing Windows domain CI alone does not discharge that platform gate.
