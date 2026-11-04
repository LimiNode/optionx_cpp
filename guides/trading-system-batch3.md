# Trading System Batch 3

## Scope And Ownership

Include `<optionx_cpp/trading_system.hpp>`. `SeriesProgression` owns a
`TradeSeries`, one outstanding operation, its fixed physical roster, and an
in-memory receipt ledger. `snapshot()` returns a value, not mutable owner state.
The Batch 1 DTO and JSON representation remain unchanged.

All calls must be serialized by one caller/owner. Revision checks are sequential
guards, not thread-safe CAS. Transitions validate a candidate copy and commit by
non-throwing swap; validation failure or allocation failure leaves the original
state intact. There are no callbacks, broker requests, threads or scheduling.

The owner starts an **idle** series, not a restored outstanding operation.
`SeriesSnapshot` is not a recovery checkpoint: it omits receipts and physical
member state. Neither reconstructing from its `TradeSeries` nor copying an owner
provides durable deduplication. Copies are independent simulations. Receipts and
used intent IDs/keys are retained for the entire owner lifetime. See the scaling
limits below before using a long-lived owner. Persistence/recovery is Batch 5.

## Series Transitions

| State | Input | Result |
| --- | --- | --- |
| `ACTIVE` | `begin()` with matching series/signal/step and fresh intent ID | `WAITING_OUTCOME`; bind current intent, group and step. |
| `WAITING_OUTCOME` / `UNRESOLVED` | Accepted partial result | Stay pending; `UNRESOLVED` while any recorded member is unknown. |
| Pending | All roster members terminal, at least one executed | `ACTIVE`; increment `next_step` once; outcome from total confirmed net P&L. |
| Pending | All members proven not submitted | `ACTIVE`; no step increment and no financial loss. |
| Pending | All members terminal but net P&L not representable | `BLOCKED_ERROR`; keep the operation and receipts, do not advance. |
| `ACTIVE` | `complete()` | `COMPLETED`; no new operation can start. |

Every accepted begin, new outcome event (including partial/unknown), or complete
increments the series revision once. Stale revisions and revision overflow are
typed rejections with no mutation. `begin()` accepts only `PROPOSED`/`ADMITTED`
intents. It does not mutate the caller's intent or claim broker admission.
The expected revision is the **series** revision, not the intent DTO revision.

`UNKNOWN` does not count as loss, cancellation or a completed member. It blocks
new work and `complete()`. Only a new explicitly evidenced event can reconcile
that same physical member. There is no timeout-to-cancel or automatic retry.

## Events And Idempotency

`TradeSettlement` binds a nonzero `SettlementId` to `intent_id`, group and a
nonzero physical `trade_id`. The caller supplies stable IDs; no IDs are generated
from callback arrival order. Event fields are immutable by convention.

| Outcome | Required evidence | Money | Terminal |
| --- | --- | --- | --- |
| `WIN` / `LOSS` / `DRAW` | `EXECUTED` | Positive / negative / zero `MoneyAmount` | Yes |
| `REJECTED` / `CANCELLED` | `NOT_SUBMITTED` | Absent | Yes, no execution |
| `UNKNOWN` | `MAY_HAVE_EXECUTED` | Absent | No |

`NOT_SUBMITTED` is a caller assertion of proven no execution, including a
conclusive reconciliation. An HTTP timeout or cancellation request during
submission is not such proof. Broker-specific interpretation belongs to a later
adapter. These event kinds must not be inferred from transport errors here.

Exact `(SettlementId, full payload)` replay returns `DUPLICATE` before revision
checks, even after reconciliation, a later step, or series completion. The same
ID with any different payload returns `SETTLEMENT_CONFLICT`. New IDs still
require the current revision and correct operation/member identity. Another ID
for an already terminal member is rejected; it never fills another roster slot.
A fresh ID is required to reconcile an unknown observation. Rejected inputs
are not recorded and may be retried with corrected input/current revision.
Numeric aggregate range errors are **not rejected inputs**: the valid event is
retained, `apply()` returns `APPLIED`, and its receipt/revision are committed.

Deduplication is scoped to the lifetime of this series owner (or standalone
aggregator), not a cross-series or persistent exactly-once guarantee. Settlement
producers must choose IDs unique within that scope.

### Identity Scopes

| Identity | Meaning and enforced scope |
| --- | --- |
| `IntentId` | One logical candidate; cannot be reused by this series owner, including after rejection/cancellation. |
| Intent `idempotency_key` | Exact case-sensitive key for one logical operation; cannot be rebound to a different intent within this owner's lifetime. Failed `begin()` calls consume neither ID nor key. |
| `SettlementId` | Immutable delivered event identity; deduplicated independently from intent keys, within the owner/aggregator lifetime. |
| Physical `trade_id` | OptionX physical record/slot ID, allocated before submission; roster members are distinct. It is not a settlement delivery counter. |
| Broker trade ID | Broker-assigned identity after acceptance; absent from these pure contracts and not a substitute for the preallocated OptionX ID. |

Different owner instances do not share a key index. Cross-series/account and
restart-safe uniqueness is not guaranteed here. The future host/account
admission owner (Batch 4), durable operation journal (Batch 5), and execution
adapter must coordinate that wider scope before broker submission. Batch 3 does
not transmit these keys to a broker or claim broker-side idempotence.

### Step Domain

The existing DTO types and JSON remain unchanged. The common counter domain is
`[0, max_series_step]`, with `max_series_step == INT32_MAX`. `begin()` requires
`next_step < max_series_step`, reserving room for a possible confirmed financial
advance. Starting at the ceiling returns `STEP_LIMIT_REACHED` without consuming
IDs/keys or changing revision. An idle series at the ceiling may be explicitly
completed; it never moves to `INT32_MAX + 1`. An initial counter above the common
domain is rejected by all transitions with the same typed error. No-execution
outcomes below the ceiling leave the step available to a fresh candidate.

## Fixed Roster And Partial Submission

`ExecutionGroupAggregator` accepts a fixed `expected_members` and distinct,
nonzero OptionX physical trade IDs reserved **before** submission. No broker ID
is required. Missing members are pending. Callback count is not completion.
`ZERO_OR_ONE`/`EXACTLY_ONE` use one reserved slot. `SPLIT_GROUP` requires at least
two; `MULTIPLE` remains unsupported by the series owner because independent
operations have no implicit single-result aggregation rule.

For partial submission failure, never-submitted slots receive explicit
`REJECTED`/`CANCELLED` events. Submitted-but-ambiguous slots receive `UNKNOWN`.
The group waits for every slot. With any executed members, only their confirmed
net P&L determines the final `WIN`/`LOSS`/`DRAW`; no-execution slots contribute no
money. If none executed, any rejection yields `REJECTED`, otherwise `CANCELLED`.
This deterministic rule closes the logical operation without retrying members.

All financial values must have the roster's configured currency and precision.
No FX conversion or implicit rounding occurs. Invalid metadata, currency and
precision mismatches are still rejected without consuming the input.

### Exact Accounting And Numeric Errors

Private unsigned two-limb magnitudes sum positive and negative values exactly,
then subtract them before conversion to signed `int64_t`. The roster has at most
`UINT32_MAX` signed 64-bit contributions, so each sign needs at most 95 bits;
the two 64-bit limbs cannot overflow. This uses only portable C++17 integer
operations, including safe handling of `INT64_MIN`, without `__int128` or a new
public money type. Recomputing from retained member facts is arrival-order
independent.

`GroupOutcome::aggregation_status` separates accounting from representation:

- `PENDING`: not every member is terminal; representable partial net P&L may be
  exposed, but no logical outcome kind exists yet.
- `READY`: every member is terminal and the logical outcome is representable
  (or is an all-no-execution result); `final` is true.
- `NUMERIC_OVERFLOW`: the exact confirmed total is outside `int64_t`. Both
  `net_profit` and the optional logical `kind` are absent, never zero, LOSS,
  CANCELLED or UNKNOWN as a numeric fallback. The accepted settlement remains
  recorded and exact replay returns `DUPLICATE`.

`all_members_terminal` describes physical completion independently from `final`.
With outstanding members, numeric overflow is temporary: the series remains
`WAITING_OUTCOME` or, if actual unknown evidence exists, `UNRESOLVED`. Later
compensating results can make the total representable without replaying an
earlier accepted event. For example all permutations of `INT64_MAX, +1, -1`
finish with the same valid `INT64_MAX` total.

If all members are terminal and the exact total is still unrepresentable, the
series enters `BLOCKED_ERROR`, preserves the current operation, and cannot
advance, start another intent or be completed. No retry, receipt deletion or
overwrite of terminal members is permitted to hide the error. A future explicit
error-resolution/persistence policy may handle this; Batch 3 has none. The
accepted physical facts remain available internally for that future work.

## Scaling Limits

Strong exception safety still uses candidate copies. For `R` retained receipts,
`I` used intents/keys, and current-group state, memory grows linearly with that
history (plus key bytes). A fresh transition copies that retained history;
processing `N` ordinary one-member operations therefore has quadratic cumulative
copy/allocation cost, not a constant-time production-session guarantee.

`TransitionScalingReportsRetainedHistoryCost` runs one owner through history
sizes 64, 128, 256 and 512. It prints elapsed chunk time and time per new logical
operation (begin plus settlement), checks revision/step counts, and replays the
earliest retained receipt at every checkpoint. No wall-clock threshold or
monotonic-timing assertion is used on shared CI hardware. This is a small
diagnostic benchmark, not proof of production throughput or a memory cap.

One local MinGW Release run measured 18/53/107/235 microseconds per operation at
64/128/256/512 retained entries respectively (about 78 ms for the whole scaling
test). These are illustrative observations, not portable performance targets.

Before Batch 5, evaluate transactional receipt insertion/journaling and narrowly
staged state updates that preserve exception safety without copying the whole
history. Do not prune receipts/keys as a micro-optimization: that would silently
weaken deduplication. No persistence or performance refactor is included here.

## Verification And Non-Goals

`trading_system_batch3_test` covers state transitions, duplicate/conflicting
delivery, revision bounds, unknown reconciliation, five-member groups, explicit
partial submission failure, permutation/limb boundaries, blocked numeric errors,
step ceilings, key scope and scaling behavior. The C++17
`trading_system_batch3_consumer_test` uses only the supported aggregate and
standard C++ main, without a testing-framework include.

No broker execution, risk reservation, MDBX journal/recovery, graph runtime,
martingale policy or Batch 6 synthetic vertical slice is included. The minimal
progression rule is one step per confirmed logical financial outcome; strategy
specific continuation/stopping rules remain future policies.
