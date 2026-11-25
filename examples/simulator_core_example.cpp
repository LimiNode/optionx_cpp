#include <optionx_cpp/simulator.hpp>
#include <iostream>

int main() {
    using namespace optionx;
    using namespace optionx::platforms;
    SimulatorPolicy policy;
    policy.defaults.payout_ppm = 750000;
    policy.defaults.min_stake_minor = 100;
    policy.defaults.max_stake_minor = 10000;
    policy.defaults.max_active_global = 2;
    policy.defaults.max_active_symbol = 1;
    policy.allowed_expiry = {60, 300};
    policy.excluded_expiries = {120};
    policy.symbols["EURUSD"] = {};
    const auto validated = SimulatorPolicySnapshot::create(policy);
    if (!validated) return 1;
    SimulatorCore core({"paper-example", CurrencyType::USD, 2, 100000}, {150, 100, 0, 8}, *validated,
        SimulatorQuoteFixture({{"EURUSD", 150, 110000}, {"EURUSD", 60150, 110010},
                               {"EURUSD", 60400, 110010}, {"EURUSD", 120400, 110020}}));
    const auto print = [&core](const char* label) {
        const auto a = core.account();
        std::cout << core.now_ms() << " " << label << " balance=" << a.balance_minor
                  << " available=" << a.available_minor << " reserved=" << a.reserved_minor
                  << " exposure=" << a.exposure_minor << "\n";
    };
    const auto on_result = [](const SimulatorTrade& trade) {
        std::cout << trade.virtual_trade_id << " " << to_str(trade.outcome)
                  << " net_profit_minor=" << trade.net_profit_minor.value_or(0)
                  << " callback_ms=" << *trade.callback_ms << "\n";
    };
    core.enqueue({"EURUSD", OrderType::BUY, 1000, 60}, on_result);
    print("queued");
    core.process(); print("reserved");
    core.advance_to(150); print("opened");
    core.advance_to(60150); print("WIN-settled");
    core.advance_to(60250); print("WIN-notified");
    core.enqueue({"EURUSD", OrderType::SELL, 1000, 60}, on_result);
    core.process(); print("reserved-SELL");
    core.advance_to(60400); print("opened-SELL");
    core.advance_to(120400); print("LOSS-settled");
    core.advance_to(120500); print("LOSS-notified");
    core.shutdown(); print("shutdown");
    return core.account().balance_minor == 99750 && core.account().reserved_minor == 0 ? 0 : 2;
}
