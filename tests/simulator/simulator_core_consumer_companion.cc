#include <optionx_cpp/simulator.hpp>

std::int64_t simulator_companion_profit() {
    return *optionx::platforms::SimulatorCore::win_profit(1000, 750000);
}
