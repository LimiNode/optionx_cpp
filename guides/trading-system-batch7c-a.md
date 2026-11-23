# Trading System Batch 7C-A: Failed-Open Evidence

## Scope And Base

Batch 7B was accepted at `dad7bcf891f08323c8f4f07681dc660137cab197`
and squash-merged through PR #194 as
`6e5f0adac14ddb5aa135dc5b67a2d108a2d7f1cc`. The merged tree matches that
reviewed HEAD. This separate slice extends the existing terminal evidence
contract, not the platform API or a second execution/terminal ledger.

Use the supported `<optionx_cpp/trading_system.hpp>` aggregate. No live/demo
calls, credentials, dependencies, new tables or global schema migration are
involved. Stop at this Draft PR for independent review; neither its merge nor
7C-B/7D is authorized automatically.

## Contract Audit And Decision

| Existing owner | Finding and bounded change |
| --- | --- |
| `TradeSettlement::valid()` in `Settlement.hpp` | Previously REJECTED/CANCELLED required NOT_SUBMITTED. Append NOT_EXECUTED_PROVEN for REJECTED only. |
| `SettlementRecord` in `TradingSystemStorage.hpp` | New failed-open proof has zero native contribution, no financial net P&L and no FX valuation. Native currency/precision metadata remains available. |
| `settlement_to_json/from_json` | Preserve numeric ordinals 0/1/2 and old encoding; explicitly gate new ordinal 3 by settlement-local evidence version 2. |
| Journal and series checkpoint codecs | Embedded settlements use the same codec/version check; no independent representation. |
| `ExecutionGroupAggregator`, `SeriesProgression::hydrate()` | Existing validity, terminal equivalence and roster aggregation accept the new nonfinancial proof. Rejection does not advance the financial step. |
| Physical settlement / group completion UOW | Reuse indexed terminal facts, physical projection, reservation, series and MM paths. Add failed-open duplicate/conflict checks before another fact is written. |
| `ExecutionRecoveryManager` | Replace private no-submission-only helper with evidence-qualified non-execution helper. Synthetic pre-submit rejection supplies 0; authoritative simulated reconciliation supplies 3. |
| Platform mapper | Still pure. OPEN_ERROR is a failed-open observation, not yet a durable receipt. CHECK_ERROR supplies no new opening or terminal proof. |

`NOT_SUBMITTED` asserts no command submission; `NOT_EXECUTED_PROVEN` asserts
proven non-execution after an attempted submission. Existing legacy records
are never reclassified retrospectively. Older simulated reconciliation records
encoded as NOT_SUBMITTED remain 0 on read/replay; richer provenance cannot be
reconstructed by guessing.

| Outcome | Valid evidence | Financial net P&L | Terminal |
| --- | --- | --- | --- |
| WIN / LOSS / DRAW | EXECUTED = 2 | Checked positive / negative / zero | Yes |
| REJECTED | NOT_SUBMITTED = 0 or NOT_EXECUTED_PROVEN = 3 | Absent | Yes |
| CANCELLED | NOT_SUBMITTED = 0 only | Absent | Yes |
| UNKNOWN | MAY_HAVE_EXECUTED = 1 only | Absent | No |

No timeout, missing callback, cancellation request, error text or unmatched
history record constitutes proof 3. The future platform receipt owner must use
the established bound adapter result semantics. IntradeBar OPEN_ERROR,
including its valid HTTP 200 empty-body case, retains its existing meaning;
Trading System does not parse HTTP again. CHECK_ERROR cannot prove opening or
non-execution or create settlement. Preserving earlier durable acceptance in
a platform receipt owner remains a 7C-B requirement.

## Wire Compatibility

Global `trading_storage_schema_version` remains 1. Settlement-local encoding:

```json
{"settlement_id":900,"intent_id":41,"execution_group_id":80,"trade_id":700,"kind":3,"evidence":3,"evidence_version":2}
```

Absent `evidence_version` means v1. Valid legacy facts emit exactly the same
fields/canonical JSON bytes as before, with no new version field. The reader
accepts v1 with evidence 0/1/2 and v2 with evidence 3. It rejects unversioned 3,
v2 with a legacy tag, unknown versions/tags, fractional, negative and overflowing
numeric tags before enum narrowing. Unknown outcome tags fail likewise.

| Reader / writer | Legacy fact | New fact |
| --- | --- | --- |
| Current reader | Reads unchanged | Requires version 2 + ordinal 3 and valid REJECTED semantics |
| Current writer | Emits unchanged v1 | Emits v2 only for proof 3 |
| Pre-7C-A reader | Reads normally | Its validity predicate rejects ordinal 3, even though its decoder ignores the extra version field |
| Pre-7C-A writer | Writes legacy data | Cannot commit decoded ordinal 3 through its validated settlement writer |

Unsupported local encoding yields STORAGE_ERROR through storage read wrappers
and blocks recovery; unsupported global record schema yields UNSUPPORTED_SCHEMA.
The codec throws `std::invalid_argument` on unknown encoding. Typed storage
writes reject unknown evidence. This is a narrow explicit evidence gate, not
an environment schema migration. Upgrade the owner before writing v2; do not
run mixed-version writers or downgrade a trading owner over new facts. Existing
single-owner storage/recovery rules apply; no environment-wide negotiation or
background rewriting of history is introduced.

The frozen test-only [v1 codec/validator](../tests/support/trading_system/LegacySettlementV1.hpp)
copies reviewed 7B rules without calling the new validator. It checks legacy
acceptance and rejection of raw ordinal 3. This is codec/predicate coverage,
not execution of a complete historical binary/storage engine. Raw MDBX writes
and low-level projection seeding are privileged maintenance operations, not
supported receipt-validation or downgrade paths.

## Atomicity, Terminality And Replay

The existing simulated recovery path uses one UOW for the rejected operation
event, physical REJECTED projection, immutable terminal settlement and RELEASED
reservation. Rollback commits none. Series/MM completion still uses the existing
UOW: callers can include all logical projections in `apply_settlement_atomic()`
or complete the already-persisted group afterward. There is no atomicity with
an external broker.

In the validated physical transition path, repeated proof 3 under the same ID
is duplicate only with identical stored record/physical facts. Identical proof
under a new ID is also DUPLICATE: no second settlement, journal event or logical
advance is written. The first durable ID is canonical. Canonical-ID replay can
complete pending logical projections; an unpersisted alias ID cannot substitute
for it. Retry pending completion using the canonical fact/ID.

If either incoming or existing terminal evidence is proof 3, conflicting terminal
kind/evidence, monetary metadata, valuation or physical facts yield CONFLICT and
poison the UOW. Legacy no-submission proof is not promoted to post-attempt proof,
or vice versa. Financial terminal evidence cannot be replaced by failed-open.
UNKNOWN alone cannot release risk or complete a logical group.

Raw immutable `put_settlement()` and revisioned `put_reservation()` remain
low-level repository operations; they do not substitute for validated physical
settlement/group completion or execution-owner protocols. This slice does not
validate arbitrary manually assembled projection writes.

Completion remains roster-based. Failed-open contributes no financial P&L and
cannot satisfy another member's identity. Mixed groups aggregate only executed
contributions. Proof 3 does not await FX even if reporting currency differs:
there is no financial contribution to convert. Executed contributions retain
their explicit valuation requirement.

Terminal checks reuse the existing group index, reading only that group's
facts, not the complete journal/history. For G indexed group facts and M facts
matching the physical member, work is O(G + M log M), temporary storage O(G + M).
There is no new global scan, full-history copy or index in the physical receipt
path. The simulator's existing full `recover()` is unchanged and is not claimed
to be the future bounded platform callback path.

The external-first mixed-group test exposed an allocator boundary: the first
external settlement in a fresh empty store must initialize the independent local
settlement sequence to 1. This is now done in the same UOW, without scanning or
using external IDs as the local high-water mark. A nonempty legacy table with
no sequence still fails closed and requires explicit `rebuild_settlement_sequence()`;
there is no implicit migration.

## Verification And Remaining Gate

[Tests](../tests/trading_system_batch7c_a_test.cpp) cover evidence validity,
legacy bytes, malformed/unknown codecs, writer rejection, old-reader validity,
MDBX reopen, rollback/poisoned transactions across physical/risk/series/MM,
duplicate IDs, conflicts in both directions, missing FX, unknown/check-error
nonproof, pre-submit rejection, repeated reconciliation and multi-account
executed/rejected aggregation. No real account is involved.

The [C++17 consumer](../tests/trading_system_batch7c_a_consumer_test.cpp) uses the
supported aggregate. Both targets run in Windows/Ubuntu CI. Earlier Batch and
offline parser/query/lifecycle tests remain separate checks. Exact-head results
belong in the PR report. The [MSVC platform-target baseline](trading-system-batch7b.md#separate-msvc-platform-baseline)
is not fixed here and remains a gate before 7D.

7C-B must review bound platform receipt provenance, monotonic durable acceptance
after CHECK_ERROR, revisions, known-ID result queries, bounded normal receipt
lookup, mirror ownership and crash recovery. Persist
`BINARY64_FIXED_NEAREST_EVEN_V1`, selected currency/precision and original numeric
facts whenever 7B conversions enter durable evidence; this nonfinancial slice
performs no such binary64 conversion. Durable platform receipt application,
callback/queue lifecycle, automatic resend and live/demo integration are absent.
