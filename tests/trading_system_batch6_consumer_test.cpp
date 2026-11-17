#include <optionx_cpp/trading_system.hpp>

int main() {
    namespace ts = optionx::trading_system;

    optionx::TradeSignal signal;
    signal.signal_id = 7;
    signal.symbol = "EUR/USD";
    signal.order_type = optionx::OrderType::BUY;
    signal.currency = optionx::CurrencyType::USD;
    signal.duration = 60;

    const auto context = ts::TradingDecisionContext::from_signal(
        signal, 12 * 60 * 60 * 1000, optionx::TradeTimeZone::utc());
    const auto symbol = ts::SymbolFilter({"EURUSD"}).evaluate(context);
    const auto window = ts::TradingWindowFilter(
        {ts::TradingTimeWindow{9 * 60 * 60, 18 * 60 * 60}}).evaluate(context);
    const auto amount = ts::MoneyAmount::from_decimal(
        "100.00", optionx::CurrencyType::USD);
    if (!amount || !symbol.is_allowed() || !window.is_allowed()) return 1;

    const auto sizing = ts::FixedStake(*amount).evaluate(context);
    if (!sizing.is_sized()) return 1;

    ts::TradeSeries series{1, signal.signal_id, 0, 0};
    ts::SeriesProgression progression(series);
    ts::ExecutionGroupAggregator group(
        1, {1, 1}, {1}, optionx::CurrencyType::USD);
    if (!progression.snapshot().series.valid() || !group.valid()) return 1;

    return 0;
}
