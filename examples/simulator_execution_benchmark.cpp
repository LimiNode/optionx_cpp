#include <optionx_cpp/simulator.hpp>
#include <chrono>
#include <iostream>

int main() {
    using namespace optionx;
    using namespace optionx::platforms;
    SimulatorPolicy policy;
    policy.symbols["EURUSD"] = {}; policy.allowed_expiry = {60, 60};
    policy.defaults.available = true;
    policy.defaults.payout_ppm = 750000;
    policy.defaults.min_stake_minor = 1; policy.defaults.max_stake_minor = 10000;
    const auto snapshot = SimulatorPolicySnapshot::create(policy);
    if (!snapshot) return 1;
    for (const std::size_t count : {128u, 512u, 2048u}) {
        SimulatorCore core({"benchmark", CurrencyType::USD, 2, 100000000}, {0, 0, 0, count},
            *snapshot, SimulatorQuoteFixture({{"EURUSD", 0, 100}, {"EURUSD", 60000, 101}}));
        std::size_t callbacks = 0;
        std::int64_t callback_profit = 0;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < count; ++i) {
            if (!core.enqueue({"EURUSD", OrderType::BUY, 1000, 60}, [&](const SimulatorTrade& trade) {
                ++callbacks; callback_profit += trade.net_profit_minor.value_or(0);
            })) return 2;
        }
        if (!core.advance_to(60000)) return 3;
        const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        const auto result = core.account();
        if (result.balance_minor != 100000000 + static_cast<std::int64_t>(count) * 750 ||
            result.reserved_minor || result.exposure_minor || callbacks != count ||
            callback_profit != static_cast<std::int64_t>(count) * 750) return 4;
        std::cout << "trades=" << count << " total_us=" << elapsed << " us_per_trade=" << elapsed / count
                  << " balance=" << result.balance_minor << '\n';
    }
}
