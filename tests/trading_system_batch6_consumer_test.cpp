#include "../examples/trading_system/SyntheticHost.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

int main() {
    namespace ex = optionx_examples;
    namespace ts = optionx::trading_system;
    const auto path = std::filesystem::temp_directory_path() /
        ("optionx_synthetic_consumer_" + std::to_string(std::chrono::high_resolution_clock::now()
            .time_since_epoch().count()) + ".mdbx");
    int result = 0;
    try {
        mdbxc::Config database;
        database.pathname = path.string();
        database.no_subdir = true;
        ex::SyntheticConfig config;
        constexpr std::int64_t noon = 43'200'000;
        config.accounts[1000] = {ts::RiskAvailability::SUPPORTED, ex::usd(1000), noon, 1};
        auto broker = std::make_shared<ts::SimulatedExecutionAdapter::State>();
        ts::ExecutionGroupId group;
        {
            ex::SyntheticHost host(database, config, noon, broker);
            ex::require(host.accept(ex::synthetic_buy()));
            group = host.series().current_group_id;
            ex::require(host.submit(group, ts::SimulatedSubmissionPlan::ACCEPTED_RESPONSE_LOST));
            host.broker_result(group, 0, 80);
        }
        ex::SyntheticHost recovered(database, config, noon, broker);
        ex::require(recovered.observe(group));
        ex::require(recovered.complete(group));
        ex::require(recovered.complete(group));
        if (recovered.series().series.next_step != 1 || recovered.reserved(1000) != 0 ||
            recovered.mm_state().state_revision != 1 || broker->history.size() != 1 ||
            broker->submission_counts.begin()->second != 1) result = 1;
        std::cout << "Synthetic C++17 slice: WIN, restart, one submission, one progression\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-lck", error);
    return result;
}
