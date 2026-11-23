#include <optionx_cpp/trading_system.hpp>

// The public aggregate owns the include closure; mapper names remain internal.
int main() {
    namespace pm = optionx::trading_system::detail::platform_mapping;
    const auto native = pm::normalize_money(
        1.125, optionx::CurrencyType::USD, {optionx::CurrencyType::USD, 2});
    if (!native.succeeded() || native.value->units != 112) return 1;
    const auto legacy = pm::stake_to_double(*native.value, {optionx::CurrencyType::USD, 2});
    if (!legacy.succeeded()) return 2;
    return pm::normalize_money(*legacy.value, optionx::CurrencyType::USD,
                              {optionx::CurrencyType::USD, 2}).value->units == 112 ? 0 : 3;
}
