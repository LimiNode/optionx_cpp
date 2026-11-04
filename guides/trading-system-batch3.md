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
used intent IDs are retained for the entire owner lifetime; memory use and
copy-on-transition cost grow with history. Persistence/recovery is Batch 5.

## Series Transitions

| State | Input | Result |
| --- | --- | --- |
| `ACTIVE` | `begin()` with matching series/signal/step and fresh intent ID | `WAITING_OUTCOME`; bind current intent, group and step. |
| `WAITING_OUTCOME` / `UNRESOLVED` | Accepted partial result | Stay pending; `UNRESOLVED` while any recorded member is unknown. |
| Pending | All roster members terminal, at least one executed | `ACTIVE`; increment `next_step` once; outcome from total confirmed net P&L. |
| Pending | All members proven not submitted | `ACTIVE`; no step increment and no financial loss. |
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

Deduplication is scoped to the lifetime of this series owner (or standalone
aggregator), not a cross-series or persistent exactly-once guarantee. Settlement
producers must choose IDs unique within that scope.

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
No FX conversion or implicit rounding occurs. Signed integer aggregation cancels
opposite signs before checked addition to avoid roster-order overflow. A partial
confirmed total outside `int64_t` returns `NUMERIC_OVERFLOW` atomically; its event
is not consumed. A final outcome exists only after every slot is terminal, never
from partial net P&L alone.

## Verification And Non-Goals

`trading_system_batch3_test` covers state transitions, duplicate/conflicting
delivery, revision bounds, unknown reconciliation, five-member groups, explicit
partial submission failure, and fixed-point boundaries. The C++17
`trading_system_batch3_consumer_test` uses only the supported aggregate and
standard C++ main, without a testing-framework include.

No broker execution, risk reservation, MDBX journal/recovery, graph runtime,
martingale policy or Batch 6 synthetic vertical slice is included. The minimal
progression rule is one step per confirmed logical financial outcome; strategy
specific continuation/stopping rules remain future policies.
