#include <optionx_cpp/trading_system.hpp>

int main() {
    namespace ts = optionx::trading_system;
    ts::TradeSettlement value{1, 2, 3, 4, ts::OutcomeKind::REJECTED,
                               ts::ExecutionEvidence::NOT_EXECUTED_PROVEN, std::nullopt};
    const auto wire = ts::detail::settlement_to_json(value);
    return value.valid() && ts::detail::settlement_from_json(wire) == value ? 0 : 1;
}
