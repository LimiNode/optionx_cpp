#include <optionx_cpp/trading_system.hpp>
#include <type_traits>

int main() {
    namespace ts = optionx::trading_system;
    namespace pr = ts::detail::platform_receipts;
    static_assert(!std::is_default_constructible_v<pr::Binding>);
    static_assert(std::is_copy_constructible_v<pr::Binding>);
    static_assert(std::has_virtual_destructor_v<optionx::platforms::BaseTradingApi>);
    ts::FakeExecutionClock clock{1};
    ts::ExecutionRecoveryManager owner(clock);
    (void)owner;
    return pr::receipt_key("a:b", "c") == pr::receipt_key("a", "b:c") ? 1 : 0;
}
