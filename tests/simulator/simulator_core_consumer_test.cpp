#include <optionx_cpp/simulator.hpp>

std::int64_t simulator_companion_profit();

int main() {
    using namespace optionx;
    using namespace optionx::platforms;
    SimulatorPolicy policy;
    policy.defaults.payout_ppm = 750000;
    policy.defaults.min_stake_minor = 1;
    policy.defaults.max_stake_minor = 10000;
    policy.symbols["EURUSD"] = {};
    const auto validated = SimulatorPolicySnapshot::create(policy);
    if (!validated) return 1;
    SimulatorCore core({"consumer", CurrencyType::USD, 2, 10000}, {150, 100, 0, 4}, *validated,
        SimulatorQuoteFixture({{"EURUSD", 150, 100}, {"EURUSD", 60150, 101}}));
    const auto id = core.enqueue({"EURUSD", OrderType::BUY, 1000, 60});
    if (!id || !core.advance_to(100000)) return 2;
    const auto result = core.query(*id);
    if (!result || result->net_profit_minor != 750 || result->callback_ms != 60250 ||
        core.account().balance_minor != 10750 || simulator_companion_profit() != 750) return 3;
    core.shutdown();
    return 0;
}
