#include <optionx_cpp/simulator_market_data.hpp>
int main() {
    auto quotes = std::make_shared<optionx::platforms::SimulatorQuoteBook>();
    return quotes->selector()("EURUSD", 0) ? 1 : 0;
}
