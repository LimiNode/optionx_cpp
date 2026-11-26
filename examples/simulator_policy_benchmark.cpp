#include <optionx_cpp/simulator_policy.hpp>
#include <chrono>
#include <iostream>

int main() {
    using namespace optionx;
    using namespace optionx::platforms;
    SimulatorPolicy policy;
    policy.defaults.payout_ppm = 750000;
    policy.defaults.min_stake_minor = 1;
    policy.defaults.max_stake_minor = 100000;
    policy.allowed_expiry = {60, 300};
    for (int i = 0; i < 16; ++i) policy.symbols["S" + std::to_string(i)] = {};
    for (std::uint32_t i = 0; i < 64; ++i) {
        SimulatorRule rule;
        rule.id = "rule-" + std::to_string(i);
        rule.priority = static_cast<std::int32_t>(i);
        rule.selector.utc_window = SimulatorTimeWindow{i * 600, (i + 1) * 600};
        rule.selector.expiry = SimulatorExpiryRange{60 + (i % 4) * 60, 300};
        rule.overrides.payout_ppm = 500000 + i * 1000;
        policy.rules.push_back(std::move(rule));
    }
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const auto validated = SimulatorPolicySnapshot::create(policy);
    const auto created = clock::now();
    if (!validated) return 1;
    const auto request = [](std::size_t i) {
        return SimulatorPolicyRequest{"S" + std::to_string(i % 16), OrderType::BUY,
            static_cast<std::uint32_t>((i * 601) % 86400), 60 + static_cast<std::uint32_t>(i % 4) * 60};
    };
    std::int64_t reference_sum = 0, snapshot_sum = 0;
    constexpr std::size_t reference_count = 100;
    constexpr std::size_t snapshot_count = 10000;
    const auto reference_start = clock::now();
    for (std::size_t i = 0; i < reference_count; ++i) {
        const auto a = policy.resolve(request(i));
        reference_sum += a.values.payout_ppm.value_or(0);
    }
    const auto reference_end = clock::now();
    for (std::size_t i = 0; i < reference_count; ++i)
        snapshot_sum += validated->resolve(request(i)).values.payout_ppm.value_or(0);
    if (reference_sum != snapshot_sum) return 2;
    const auto fast_start = clock::now();
    std::int64_t fast_sum = 0;
    for (std::size_t i = 0; i < snapshot_count; ++i)
        fast_sum += validated->resolve(request(i)).values.payout_ppm.value_or(0);
    const auto fast_end = clock::now();
    const auto micros = [](auto a, auto b) { return std::chrono::duration<double, std::micro>(b - a).count(); };
    std::cout << "symbols=16 rules=64 time_cells=65 expiry_cells=4 reachable_cells=8320\n"
              << "snapshot_create_us=" << micros(start, created)
              << " reference_count=" << reference_count
              << " reference_us_per_resolve=" << micros(reference_start, reference_end) / reference_count
              << " snapshot_count=" << snapshot_count
              << " snapshot_us_per_resolve=" << micros(fast_start, fast_end) / snapshot_count
              << "\nreference_checksum=" << reference_sum << " matching_snapshot_checksum=" << snapshot_sum
              << " benchmark_checksum=" << fast_sum << "\n";
    return 0;
}
