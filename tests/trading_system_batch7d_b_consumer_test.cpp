#include <optionx_cpp/trading_system_platform.hpp>
#include <type_traits>

static_assert(!std::is_copy_constructible<optionx::trading_system::detail::OfflinePlatformOwner>::value,
              "an execution owner must not be copied");

int main() { return 0; }
