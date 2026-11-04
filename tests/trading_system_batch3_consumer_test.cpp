#include <optionx_cpp/trading_system.hpp>
#include <limits>

int main() {
    using namespace optionx::trading_system;
    SeriesProgression owner(TradeSeries{1, 2});
    TradeIntent intent;
    intent.intent_id = 3;
    intent.series_id = 1;
    intent.origin_signal_id = 2;
    intent.series_step = 0;
    intent.idempotency_key = "consumer-intent-3";
    intent.cardinality = ExecutionCardinality::SPLIT_GROUP;
    intent.execution_group = {4, 2};
    if (owner.begin(intent, {5, 6}, optionx::CurrencyType::USD, 2, 0) !=
        ProgressionResult::APPLIED) return 1;

    TradeSettlement result;
    result.settlement_id = 7;
    result.intent_id = 3;
    result.execution_group_id = 4;
    result.trade_id = 5;
    result.kind = OutcomeKind::WIN;
    result.evidence = ExecutionEvidence::EXECUTED;
    result.net_profit = MoneyAmount{100, 2, optionx::CurrencyType::USD};
    if (owner.apply(result, 1) != ProgressionResult::APPLIED) return 2;
    if (owner.apply(result, 1) != ProgressionResult::DUPLICATE) return 3;
    result.settlement_id = 8;
    result.trade_id = 6;
    result.kind = OutcomeKind::LOSS;
    result.net_profit->units = -100;
    if (owner.apply(result, 2) != ProgressionResult::APPLIED) return 4;
    const auto state = owner.snapshot();
    if (state.series.next_step != 1 || state.outcome.kind != OutcomeKind::DRAW) return 5;
    if (owner.complete(3) != ProgressionResult::APPLIED) return 6;

    ExecutionGroupAggregator wide(9, {10, 3}, {11, 12, 13}, optionx::CurrencyType::USD);
    result.intent_id = 9;
    result.execution_group_id = 10;
    result.trade_id = 11;
    result.settlement_id = 14;
    result.kind = OutcomeKind::WIN;
    result.net_profit->units = (std::numeric_limits<std::int64_t>::max)();
    if (wide.apply(result) != ProgressionResult::APPLIED) return 7;
    result.trade_id = 12;
    result.settlement_id = 15;
    result.net_profit->units = 1;
    if (wide.apply(result) != ProgressionResult::APPLIED) return 8;
    const auto partial = wide.outcome();
    if (partial.aggregation_status != AggregationStatus::NUMERIC_OVERFLOW ||
        partial.net_profit || partial.kind || partial.final) return 9;
    if (wide.apply(result) != ProgressionResult::DUPLICATE) return 10;
    result.trade_id = 13;
    result.settlement_id = 16;
    result.kind = OutcomeKind::LOSS;
    result.net_profit->units = -1;
    if (wide.apply(result) != ProgressionResult::APPLIED) return 11;
    const auto final = wide.outcome();
    if (!final.final || final.aggregation_status != AggregationStatus::READY || !final.net_profit ||
        final.net_profit->units != (std::numeric_limits<std::int64_t>::max)()) return 12;
    return 0;
}
