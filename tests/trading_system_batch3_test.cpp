#include <gtest/gtest.h>
#include <optionx_cpp/trading_system.hpp>

#include <limits>

namespace {

namespace ts = optionx::trading_system;
using optionx::CurrencyType;
using ts::ExecutionEvidence;
using ts::OutcomeKind;
using ts::ProgressionResult;
using ts::SeriesStatus;

ts::TradeSeries series(std::uint64_t revision = 0) {
    return {12, 7, 0, revision};
}

ts::TradeIntent intent(bool split = false, std::uint64_t id = 41, std::int32_t step = 0) {
    ts::TradeIntent result;
    result.intent_id = id;
    result.origin_signal_id = 7;
    result.series_id = 12;
    result.series_step = step;
    result.idempotency_key = std::to_string(id);
    result.cardinality = split ? ts::ExecutionCardinality::SPLIT_GROUP
                               : ts::ExecutionCardinality::EXACTLY_ONE;
    result.execution_group = split ? ts::ExecutionGroupIdentity{80, 5}
                                   : ts::ExecutionGroupIdentity{0, 1};
    return result;
}

ts::TradeSettlement settlement(std::uint64_t id = 100, std::uint64_t trade_id = 1,
                               std::int64_t units = 100, bool split = false) {
    ts::TradeSettlement result;
    result.settlement_id = id;
    result.intent_id = 41;
    result.execution_group_id = split ? 80 : 0;
    result.trade_id = trade_id;
    result.kind = units > 0 ? OutcomeKind::WIN : units < 0 ? OutcomeKind::LOSS : OutcomeKind::DRAW;
    result.evidence = ExecutionEvidence::EXECUTED;
    result.net_profit = ts::MoneyAmount{units, 2, CurrencyType::USD};
    return result;
}

ts::TradeSettlement no_execution(OutcomeKind kind, std::uint64_t id = 100,
                                 std::uint64_t trade_id = 1, bool split = false) {
    auto result = settlement(id, trade_id, 0, split);
    result.kind = kind;
    result.evidence = kind == OutcomeKind::UNKNOWN ? ExecutionEvidence::MAY_HAVE_EXECUTED
                                                  : ExecutionEvidence::NOT_SUBMITTED;
    result.net_profit.reset();
    return result;
}

ts::ExecutionGroupAggregator group() {
    return {41, {80, 5}, {1, 2, 3, 4, 5}, CurrencyType::USD};
}

void expect_unchanged(const ts::SeriesSnapshot& before, const ts::SeriesSnapshot& after) {
    EXPECT_EQ(nlohmann::json(before.series), nlohmann::json(after.series));
    EXPECT_EQ(before.status, after.status);
    EXPECT_EQ(before.current_intent_id, after.current_intent_id);
    EXPECT_EQ(before.current_group_id, after.current_group_id);
    EXPECT_EQ(before.current_step, after.current_step);
    EXPECT_EQ(before.outcome.final, after.outcome.final);
    EXPECT_EQ(before.outcome.unresolved, after.outcome.unresolved);
    EXPECT_EQ(before.outcome.completed_members, after.outcome.completed_members);
    EXPECT_EQ(before.outcome.kind, after.outcome.kind);
    ASSERT_EQ(before.outcome.net_profit.has_value(), after.outcome.net_profit.has_value());
    if (before.outcome.net_profit) {
        EXPECT_EQ(before.outcome.net_profit->units, after.outcome.net_profit->units);
    }
}

TEST(TradingSystemBatch3, TradingOutcomesAdvanceExactlyOneLogicalStep) {
    for (auto units : {100, -100, 0}) {
        ts::SeriesProgression owner(series());
        ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
        EXPECT_EQ(owner.snapshot().status, SeriesStatus::WAITING_OUTCOME);
        EXPECT_EQ(owner.snapshot().current_intent_id, 41U);
        ASSERT_EQ(owner.apply(settlement(100, 1, units), 1), ProgressionResult::APPLIED);
        const auto state = owner.snapshot();
        EXPECT_EQ(state.series.next_step, 1U);
        EXPECT_EQ(state.series.state_revision, 2U);
        EXPECT_EQ(state.status, SeriesStatus::ACTIVE);
        EXPECT_EQ(state.current_intent_id, 0U);
        EXPECT_TRUE(state.outcome.final);
        EXPECT_EQ(state.outcome.net_profit->units, units);
    }
}

TEST(TradingSystemBatch3, ExactReplayIsNoOpEvenWithOriginalRevision) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    const auto event = settlement();
    ASSERT_EQ(owner.apply(event, 1), ProgressionResult::APPLIED);
    const auto before = owner.snapshot();
    EXPECT_EQ(owner.apply(event, 1), ProgressionResult::DUPLICATE);
    expect_unchanged(before, owner.snapshot());
}

TEST(TradingSystemBatch3, ConflictingReplayRejectsEveryPayloadDifference) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    const auto event = settlement();
    ASSERT_EQ(owner.apply(event, 1), ProgressionResult::APPLIED);
    const auto before = owner.snapshot();
    for (int field = 0; field < 8; ++field) {
        auto conflict = event;
        switch (field) {
            case 0: ++conflict.intent_id; break;
            case 1: ++conflict.trade_id; break;
            case 2: ++conflict.execution_group_id; break;
            case 3: conflict.kind = OutcomeKind::LOSS; break;
            case 4: conflict.evidence = ExecutionEvidence::NOT_SUBMITTED; break;
            case 5: ++conflict.net_profit->units; break;
            case 6: ++conflict.net_profit->precision; break;
            case 7: conflict.net_profit->currency = CurrencyType::EUR; break;
        }
        EXPECT_EQ(owner.apply(conflict, 1), ProgressionResult::SETTLEMENT_CONFLICT);
        expect_unchanged(before, owner.snapshot());
    }
}

TEST(TradingSystemBatch3, RejectedAndCancelledBeforeSubmitNeverCountAsLoss) {
    for (auto kind : {OutcomeKind::REJECTED, OutcomeKind::CANCELLED}) {
        ts::SeriesProgression owner(series());
        ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
        ASSERT_EQ(owner.apply(no_execution(kind), 1), ProgressionResult::APPLIED);
        const auto state = owner.snapshot();
        EXPECT_EQ(state.series.next_step, 0U);
        EXPECT_EQ(state.status, SeriesStatus::ACTIVE);
        EXPECT_EQ(state.outcome.kind, kind);
        EXPECT_FALSE(state.outcome.net_profit);
        EXPECT_EQ(owner.begin(intent(false, 42), {2}, CurrencyType::USD, 2, 2),
                  ProgressionResult::APPLIED);
    }
}

TEST(TradingSystemBatch3, UnknownBlocksUntilExplicitReconciliation) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    const auto unknown = no_execution(OutcomeKind::UNKNOWN);
    ASSERT_EQ(owner.apply(unknown, 1), ProgressionResult::APPLIED);
    const auto unresolved = owner.snapshot();
    EXPECT_EQ(unresolved.status, SeriesStatus::UNRESOLVED);
    EXPECT_EQ(unresolved.series.next_step, 0U);
    EXPECT_EQ(unresolved.current_intent_id, 41U);
    EXPECT_EQ(owner.begin(intent(false, 42), {2}, CurrencyType::USD, 2, 2),
              ProgressionResult::INVALID_STATE);
    EXPECT_EQ(owner.complete(2), ProgressionResult::INVALID_STATE);
    expect_unchanged(unresolved, owner.snapshot());
    ASSERT_EQ(owner.apply(settlement(101), 2), ProgressionResult::APPLIED);
    EXPECT_EQ(owner.snapshot().series.next_step, 1U);
    const auto resolved = owner.snapshot();
    EXPECT_EQ(owner.apply(unknown, 1), ProgressionResult::DUPLICATE);
    expect_unchanged(resolved, owner.snapshot());
}

TEST(TradingSystemBatch3, ReconciliationMayProveNoSubmissionWithoutLoss) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    ASSERT_EQ(owner.apply(no_execution(OutcomeKind::UNKNOWN), 1), ProgressionResult::APPLIED);
    ASSERT_EQ(owner.apply(no_execution(OutcomeKind::CANCELLED, 101), 2), ProgressionResult::APPLIED);
    EXPECT_EQ(owner.snapshot().series.next_step, 0U);
    EXPECT_EQ(owner.snapshot().status, SeriesStatus::ACTIVE);
}

TEST(TradingSystemBatch3, StaleRevisionAndWrongOperationLeaveStateUnchanged) {
    ts::SeriesProgression owner(series());
    EXPECT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 1),
              ProgressionResult::REVISION_CONFLICT);
    ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    const auto before = owner.snapshot();
    EXPECT_EQ(owner.apply(settlement(), 0), ProgressionResult::REVISION_CONFLICT);
    auto wrong = settlement();
    ++wrong.intent_id;
    EXPECT_EQ(owner.apply(wrong, 1), ProgressionResult::WRONG_OPERATION);
    wrong = settlement();
    ++wrong.execution_group_id;
    EXPECT_EQ(owner.apply(wrong, 1), ProgressionResult::WRONG_OPERATION);
    wrong = settlement();
    ++wrong.trade_id;
    EXPECT_EQ(owner.apply(wrong, 1), ProgressionResult::UNKNOWN_MEMBER);
    expect_unchanged(before, owner.snapshot());
    EXPECT_EQ(owner.apply(settlement(), 1), ProgressionResult::APPLIED);
}

TEST(TradingSystemBatch3, RevisionOverflowRejectsWithoutConsumingSettlement) {
    const auto max = (std::numeric_limits<std::uint64_t>::max)();
    ts::SeriesProgression owner(series(max - 1));
    ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, max - 1), ProgressionResult::APPLIED);
    const auto before = owner.snapshot();
    EXPECT_EQ(owner.apply(settlement(), max), ProgressionResult::REVISION_OVERFLOW);
    EXPECT_EQ(owner.apply(settlement(), max), ProgressionResult::REVISION_OVERFLOW);
    expect_unchanged(before, owner.snapshot());
    ts::SeriesProgression idle(series(max));
    EXPECT_EQ(idle.begin(intent(), {1}, CurrencyType::USD, 2, max), ProgressionResult::REVISION_OVERFLOW);
    EXPECT_EQ(idle.complete(max), ProgressionResult::REVISION_OVERFLOW);
}

TEST(TradingSystemBatch3, OnlyOneUnfinishedOperationCanOwnSeries) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    EXPECT_EQ(owner.begin(intent(false, 42), {2}, CurrencyType::USD, 2, 1),
              ProgressionResult::INVALID_STATE);
    EXPECT_EQ(owner.complete(1), ProgressionResult::INVALID_STATE);
}

TEST(TradingSystemBatch3, OldReceiptsSurviveNextOperationAndIntentCannotBeReused) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    const auto rejected = no_execution(OutcomeKind::REJECTED);
    ASSERT_EQ(owner.apply(rejected, 1), ProgressionResult::APPLIED);
    EXPECT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 2),
              ProgressionResult::INTENT_ALREADY_USED);
    ASSERT_EQ(owner.begin(intent(false, 42), {2}, CurrencyType::USD, 2, 2), ProgressionResult::APPLIED);
    const auto before = owner.snapshot();
    EXPECT_EQ(owner.apply(rejected, 0), ProgressionResult::DUPLICATE);
    auto old = settlement(101);
    EXPECT_EQ(owner.apply(old, 3), ProgressionResult::WRONG_OPERATION);
    expect_unchanged(before, owner.snapshot());
}

TEST(TradingSystemBatch3, CompletedSeriesRejectsNewWorkButAcceptsExactReplay) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    ASSERT_EQ(owner.apply(settlement(), 1), ProgressionResult::APPLIED);
    EXPECT_EQ(owner.complete(1), ProgressionResult::REVISION_CONFLICT);
    ASSERT_EQ(owner.complete(2), ProgressionResult::APPLIED);
    EXPECT_EQ(owner.snapshot().status, SeriesStatus::COMPLETED);
    EXPECT_EQ(owner.begin(intent(false, 42, 1), {2}, CurrencyType::USD, 2, 3),
              ProgressionResult::INVALID_STATE);
    EXPECT_EQ(owner.complete(3), ProgressionResult::INVALID_STATE);
    EXPECT_EQ(owner.apply(settlement(), 1), ProgressionResult::DUPLICATE);
}

TEST(TradingSystemBatch3, InvalidBindingStatusAndMultipleCardinalityAreRejected) {
    ts::SeriesProgression owner(series());
    for (int field = 0; field < 5; ++field) {
        auto wrong = intent();
        switch (field) {
            case 0: wrong.series_id = 13; break;
            case 1: wrong.series_step = 1; break;
            case 2: wrong.intent_id = 0; break;
            case 3: wrong.origin_signal_id = 8; break;
            case 4: wrong.idempotency_key.clear(); break;
        }
        EXPECT_EQ(owner.begin(wrong, {1}, CurrencyType::USD, 2, 0), ProgressionResult::INVALID_IDENTITY);
    }
    auto wrong = intent();
    wrong.status = ts::IntentStatus::EXECUTION_UNKNOWN;
    EXPECT_EQ(owner.begin(wrong, {1}, CurrencyType::USD, 2, 0), ProgressionResult::INVALID_STATE);
    wrong = intent(true);
    wrong.cardinality = ts::ExecutionCardinality::MULTIPLE;
    EXPECT_EQ(owner.begin(wrong, {1, 2, 3, 4, 5}, CurrencyType::USD, 2, 0),
              ProgressionResult::UNSUPPORTED_CARDINALITY);
    ts::SeriesProgression invalid(ts::TradeSeries{});
    EXPECT_EQ(invalid.complete(0), ProgressionResult::INVALID_IDENTITY);
}

TEST(TradingSystemBatch3, ZeroOrOneStillRequiresReservedMemberAndExplicitNoExecution) {
    auto candidate = intent();
    candidate.cardinality = ts::ExecutionCardinality::ZERO_OR_ONE;
    candidate.execution_group.expected_members = 0;
    ts::SeriesProgression owner(series());
    EXPECT_EQ(owner.begin(candidate, {}, CurrencyType::USD, 2, 0), ProgressionResult::INVALID_CONFIGURATION);
    ASSERT_EQ(owner.begin(candidate, {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    EXPECT_EQ(owner.apply(no_execution(OutcomeKind::REJECTED), 1), ProgressionResult::APPLIED);
}

TEST(TradingSystemBatch3, FivePhysicalTradesProduceOneLogicalResult) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(true), {1, 2, 3, 4, 5}, CurrencyType::USD, 2, 0),
              ProgressionResult::APPLIED);
    for (std::uint64_t i = 1; i <= 5; ++i) {
        ASSERT_EQ(owner.apply(settlement(100 + i, i, i == 5 ? -200 : 100, true), i),
                  ProgressionResult::APPLIED);
        EXPECT_EQ(owner.snapshot().series.next_step, i == 5 ? 1U : 0U);
    }
    const auto final = owner.snapshot();
    EXPECT_EQ(final.outcome.kind, OutcomeKind::WIN);
    EXPECT_EQ(final.outcome.net_profit->units, 200);
    EXPECT_EQ(final.outcome.completed_members, 5U);
    EXPECT_EQ(final.series.state_revision, 6U);
    EXPECT_EQ(final.status, SeriesStatus::ACTIVE);
}

TEST(TradingSystemBatch3, PhysicalDuplicateNeverFillsAnotherMemberSlot) {
    auto aggregator = group();
    const auto event = settlement(100, 1, 100, true);
    ASSERT_EQ(aggregator.apply(event), ProgressionResult::APPLIED);
    EXPECT_EQ(aggregator.apply(event), ProgressionResult::DUPLICATE);
    auto duplicate_trade = event;
    ++duplicate_trade.settlement_id;
    EXPECT_EQ(aggregator.apply(duplicate_trade), ProgressionResult::MEMBER_ALREADY_TERMINAL);
    auto impersonation = event;
    impersonation.trade_id = 2;
    EXPECT_EQ(aggregator.apply(impersonation), ProgressionResult::SETTLEMENT_CONFLICT);
    EXPECT_EQ(aggregator.outcome().completed_members, 1U);
    EXPECT_FALSE(aggregator.outcome().final);
}

TEST(TradingSystemBatch3, UnknownMemberKeepsSplitPendingAfterOtherFourSettle) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(true), {1, 2, 3, 4, 5}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    ASSERT_EQ(owner.apply(no_execution(OutcomeKind::UNKNOWN, 100, 5, true), 1), ProgressionResult::APPLIED);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        ASSERT_EQ(owner.apply(settlement(100 + i, i, 100, true), i + 1), ProgressionResult::APPLIED);
    }
    EXPECT_EQ(owner.snapshot().status, SeriesStatus::UNRESOLVED);
    EXPECT_EQ(owner.snapshot().series.next_step, 0U);
    EXPECT_EQ(owner.snapshot().outcome.completed_members, 4U);
    EXPECT_FALSE(owner.snapshot().outcome.final);
    ASSERT_EQ(owner.apply(settlement(105, 5, -500, true), 6), ProgressionResult::APPLIED);
    EXPECT_EQ(owner.snapshot().outcome.kind, OutcomeKind::LOSS);
    EXPECT_EQ(owner.snapshot().outcome.net_profit->units, -100);
    EXPECT_EQ(owner.snapshot().series.next_step, 1U);
}

TEST(TradingSystemBatch3, SplitPartialSubmissionFailureAggregatesOnlyConfirmedMoney) {
    auto aggregator = group();
    ASSERT_EQ(aggregator.apply(settlement(100, 1, -100, true)), ProgressionResult::APPLIED);
    for (std::uint64_t i = 2; i <= 5; ++i) {
        ASSERT_EQ(aggregator.apply(no_execution(OutcomeKind::REJECTED, 100 + i, i, true)),
                  ProgressionResult::APPLIED);
    }
    EXPECT_TRUE(aggregator.outcome().final);
    EXPECT_EQ(aggregator.outcome().kind, OutcomeKind::LOSS);
    EXPECT_EQ(aggregator.outcome().net_profit->units, -100);
}

TEST(TradingSystemBatch3, EntirelyUnsubmittedSplitDoesNotAdvanceSeries) {
    for (auto kind : {OutcomeKind::REJECTED, OutcomeKind::CANCELLED}) {
        ts::SeriesProgression owner(series());
        ASSERT_EQ(owner.begin(intent(true), {1, 2, 3, 4, 5}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
        for (std::uint64_t i = 1; i <= 5; ++i) {
            ASSERT_EQ(owner.apply(no_execution(kind, 100 + i, i, true), i), ProgressionResult::APPLIED);
        }
        EXPECT_EQ(owner.snapshot().status, SeriesStatus::ACTIVE);
        EXPECT_EQ(owner.snapshot().series.next_step, 0U);
        EXPECT_EQ(owner.snapshot().outcome.kind, kind);
        EXPECT_FALSE(owner.snapshot().outcome.net_profit);
    }
}

TEST(TradingSystemBatch3, MixedWinsAndLossesUseNetPnlIncludingZero) {
    for (auto last : {-101, -100, -99}) {
        auto aggregator = group();
        ASSERT_EQ(aggregator.apply(settlement(101, 1, 100, true)), ProgressionResult::APPLIED);
        ASSERT_EQ(aggregator.apply(settlement(102, 2, last, true)), ProgressionResult::APPLIED);
        for (std::uint64_t i = 3; i <= 5; ++i) {
            ASSERT_EQ(aggregator.apply(no_execution(OutcomeKind::CANCELLED, 100 + i, i, true)),
                      ProgressionResult::APPLIED);
        }
        EXPECT_TRUE(aggregator.outcome().final);
        EXPECT_EQ(aggregator.outcome().net_profit->units, 100 + last);
        EXPECT_EQ(aggregator.outcome().kind, last < -100 ? OutcomeKind::LOSS :
                  last > -100 ? OutcomeKind::WIN : OutcomeKind::DRAW);
    }
}

TEST(TradingSystemBatch3, CurrencyPrecisionAndMetadataMismatchAreAtomic) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(true), {1, 2, 3, 4, 5}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    const auto before = owner.snapshot();
    auto event = settlement(100, 1, 100, true);
    event.net_profit->currency = CurrencyType::EUR;
    EXPECT_EQ(owner.apply(event, 1), ProgressionResult::CURRENCY_MISMATCH);
    event.net_profit->currency = CurrencyType::USD;
    event.net_profit->precision = 3;
    EXPECT_EQ(owner.apply(event, 1), ProgressionResult::PRECISION_MISMATCH);
    event.net_profit->precision = 19;
    EXPECT_EQ(owner.apply(event, 1), ProgressionResult::INVALID_OUTCOME);
    expect_unchanged(before, owner.snapshot());
    event.net_profit->precision = 2;
    EXPECT_EQ(owner.apply(event, 1), ProgressionResult::APPLIED);
}

TEST(TradingSystemBatch3, SignedMoneyOverflowNeverConsumesResultOrRevision) {
    for (auto boundary : {(std::numeric_limits<std::int64_t>::max)(),
                          (std::numeric_limits<std::int64_t>::min)()}) {
        ts::SeriesProgression owner(series());
        ASSERT_EQ(owner.begin(intent(true), {1, 2, 3, 4, 5}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
        ASSERT_EQ(owner.apply(settlement(100, 1, boundary, true), 1), ProgressionResult::APPLIED);
        const auto before = owner.snapshot();
        auto event = settlement(101, 2, boundary > 0 ? 1 : -1, true);
        EXPECT_EQ(owner.apply(event, 2), ProgressionResult::NUMERIC_OVERFLOW);
        expect_unchanged(before, owner.snapshot());
        event = settlement(101, 2, boundary > 0 ? -1 : 1, true);
        ASSERT_EQ(owner.apply(event, 2), ProgressionResult::APPLIED);
        EXPECT_EQ(owner.snapshot().outcome.net_profit->units, boundary > 0 ? boundary - 1 : boundary + 1);
    }
}

TEST(TradingSystemBatch3, InvalidRostersNeverBecomeUsable) {
    for (const auto& ids : std::vector<std::vector<std::uint64_t>>{
             {}, {1, 2, 3, 4}, {1, 2, 3, 4, 4}, {0, 2, 3, 4, 5}, {1, 2, 3, 4, 5, 6}}) {
        ts::ExecutionGroupAggregator aggregator(41, {80, 5}, ids, CurrencyType::USD);
        EXPECT_FALSE(aggregator.valid());
        EXPECT_EQ(aggregator.apply(settlement()), ProgressionResult::INVALID_CONFIGURATION);
    }
    EXPECT_FALSE(ts::ExecutionGroupAggregator(0, {80, 5}, {1, 2, 3, 4, 5}, CurrencyType::USD).valid());
    EXPECT_FALSE(ts::ExecutionGroupAggregator(41, {0, 5}, {1, 2, 3, 4, 5}, CurrencyType::USD).valid());
    EXPECT_FALSE(ts::ExecutionGroupAggregator(41, {80, 5}, {1, 2, 3, 4, 5}, CurrencyType::UNKNOWN).valid());
}

TEST(TradingSystemBatch3, RepresentableTotalDoesNotDependOnRosterSummationOrder) {
    auto aggregator = group();
    const auto max = (std::numeric_limits<std::int64_t>::max)();
    ASSERT_EQ(aggregator.apply(settlement(101, 1, max, true)), ProgressionResult::APPLIED);
    ASSERT_EQ(aggregator.apply(settlement(103, 3, -1, true)), ProgressionResult::APPLIED);
    ASSERT_EQ(aggregator.apply(settlement(102, 2, 1, true)), ProgressionResult::APPLIED);
    EXPECT_EQ(aggregator.outcome().net_profit->units, max);
}

TEST(TradingSystemBatch3, OutcomeEvidenceCannotDisguiseUnknownAsCancellation) {
    auto aggregator = group();
    for (int field = 0; field < 7; ++field) {
        auto event = settlement(100, 1, 100, true);
        switch (field) {
            case 0: event.kind = OutcomeKind::CANCELLED; break;
            case 1: event.evidence = ExecutionEvidence::MAY_HAVE_EXECUTED; break;
            case 2: event.kind = OutcomeKind::LOSS; break;
            case 3: event.kind = OutcomeKind::DRAW; break;
            case 4: event.net_profit.reset(); break;
            case 5: event.settlement_id = 0; break;
            case 6: event.kind = static_cast<OutcomeKind>(100); break;
        }
        EXPECT_EQ(aggregator.apply(event), ProgressionResult::INVALID_OUTCOME);
    }
    auto cancelled = no_execution(OutcomeKind::CANCELLED, 100, 1, true);
    cancelled.evidence = ExecutionEvidence::MAY_HAVE_EXECUTED;
    EXPECT_EQ(aggregator.apply(cancelled), ProgressionResult::INVALID_OUTCOME);
    EXPECT_EQ(aggregator.outcome().completed_members, 0U);
}

TEST(TradingSystemBatch3, SnapshotAndCopiedOwnersHaveIndependentValueState) {
    ts::SeriesProgression owner(series());
    ASSERT_EQ(owner.begin(intent(), {1}, CurrencyType::USD, 2, 0), ProgressionResult::APPLIED);
    auto copy = owner;
    auto view = owner.snapshot();
    view.series.next_step = 99;
    EXPECT_EQ(copy.apply(settlement(), 1), ProgressionResult::APPLIED);
    EXPECT_EQ(owner.snapshot().series.next_step, 0U);
    EXPECT_EQ(owner.snapshot().series.state_revision, 1U);
    EXPECT_EQ(copy.snapshot().series.next_step, 1U);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
