#include <gtest/gtest.h>
#include "../examples/trading_system/SyntheticHost.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>

namespace {
namespace ex = optionx_examples;
namespace ts = optionx::trading_system;
constexpr std::int64_t noon = 12 * 60 * 60 * 1000;

struct Database {
    static std::atomic<unsigned> next;
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("optionx_batch6_" + std::to_string(std::chrono::high_resolution_clock::now()
            .time_since_epoch().count()) + "_" + std::to_string(next++) + ".mdbx");
    ~Database() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-lck", error);
    }
    mdbxc::Config config() const {
        mdbxc::Config result;
        result.pathname = path.string();
        result.no_subdir = true;
        return result;
    }
};
std::atomic<unsigned> Database::next{0};

ex::SyntheticConfig configuration() {
    ex::SyntheticConfig config;
    config.accounts[1000] = {ts::RiskAvailability::SUPPORTED, ex::usd(1000), noon, 1};
    return config;
}

using Broker = ts::SimulatedExecutionAdapter;

std::size_t submissions(const std::shared_ptr<Broker::State>& broker) {
    std::size_t count = 0;
    for (const auto& entry : broker->submission_counts) count += entry.second;
    return count;
}

void expect_no_execution(ex::SyntheticHost& host, const std::shared_ptr<Broker::State>& broker) {
    EXPECT_TRUE(host.storage().reservations().values.empty());
    EXPECT_TRUE(host.storage().journal_events().values.empty());
    EXPECT_EQ(host.storage().intent(1).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(host.storage().roster(1).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(host.storage().physical_trade(1).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(host.series().series.state_revision, 0U);
    EXPECT_EQ(host.series().series.next_step, 0U);
    EXPECT_EQ(host.mm_state().state_revision, 0U);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(submissions(broker), 0U);
}

TEST(TradingSystemBatch6, SuccessfulBuyPersistsWinAndAdvancesOnce) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), configuration(), noon, broker);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    EXPECT_EQ(host.series().status, ts::SeriesStatus::WAITING_OUTCOME);
    EXPECT_EQ(host.series().series.state_revision, 1U);
    EXPECT_EQ(host.reserved(1000), 100);
    const auto intent = ex::read(host.storage().intent(1));
    EXPECT_EQ(intent.request.order_type, optionx::OrderType::BUY);
    EXPECT_EQ(intent.request.amount, 1.0);
    ASSERT_TRUE(host.submit(group).succeeded());
    host.broker_result(group, 0, 80);
    ASSERT_TRUE(host.observe(group).succeeded());
    EXPECT_EQ(host.series().series.next_step, 0U);
    ASSERT_TRUE(host.complete(group).succeeded());
    EXPECT_EQ(host.series().series.next_step, 1U);
    EXPECT_EQ(host.series().series.state_revision, 2U);
    EXPECT_EQ(host.series().current_outcome.kind, ts::OutcomeKind::WIN);
    EXPECT_EQ(host.mm_state().state_revision, 1U);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(host.complete(group).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(host.mm_state().state_revision, 1U);
    EXPECT_EQ(submissions(broker), 1U);
}

TEST(TradingSystemBatch6, SymbolRejectionHasNoExecutionSideEffects) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), configuration(), noon, broker);
    auto signal = ex::synthetic_buy();
    signal.symbol = "GBP/USD";
    EXPECT_FALSE(host.accept(signal).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, TimeWindowRejectionHasNoExecutionSideEffects) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), configuration(), 18 * 60 * 60 * 1000, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, InsufficientRiskHasNoPartialPreparation) {
    Database db;
    auto config = configuration();
    config.accounts[1000].available_balance = ex::usd(99);
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, MissingAccountSnapshotHasNoPartialPreparation) {
    Database db;
    auto config = configuration();
    config.accounts[1000].availability = ts::RiskAvailability::UNAVAILABLE;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, StaleAccountSnapshotHasNoPartialPreparation) {
    Database db;
    auto config = configuration();
    config.accounts[1000].captured_at_ms = noon - 30'001;
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, LaterScopeRejectionRollsBackEarlierOwnerAdmission) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 2000}};
    config.accounts[2000] = {ts::RiskAvailability::SUPPORTED, ex::usd(99), noon, 1};
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
    EXPECT_EQ(host.reserved(2000), 0);
}

TEST(TradingSystemBatch6, OneLossStepSurvivesReopenAndWinResetsStake) {
    Database db;
    auto config = configuration();
    config.loss_step = true;
    auto broker = std::make_shared<Broker::State>();
    {
        ex::SyntheticHost host(db.config(), config, noon, broker);
        ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
        const auto group = host.series().current_group_id;
        ASSERT_TRUE(host.submit(group).succeeded());
        host.broker_result(group, 0, -100);
        ASSERT_TRUE(host.observe(group).succeeded());
        ASSERT_TRUE(host.complete(group).succeeded());
        EXPECT_TRUE(host.mm_state().payload.at("after_loss").get<bool>());
    }
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_EQ(host.mm_state().last_settlement_id, 1U);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    EXPECT_EQ(ex::read(host.storage().roster(group)).members.front().stake.units, 200);
    EXPECT_EQ(host.reserved(1000), 200);
    ASSERT_TRUE(host.submit(group).succeeded());
    host.broker_result(group, 0, 160);
    ASSERT_TRUE(host.observe(group).succeeded());
    ASSERT_TRUE(host.complete(group).succeeded());
    EXPECT_EQ(host.series().series.next_step, 2U);
    EXPECT_EQ(host.series().series.state_revision, 4U);
    EXPECT_EQ(host.mm_state().state_revision, 2U);
    EXPECT_FALSE(host.mm_state().payload.at("after_loss").get<bool>());
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    EXPECT_EQ(host.reserved(1000), 100);
}

TEST(TradingSystemBatch6, IncreasedStakeStillRequiresRiskAdmission) {
    Database db;
    auto config = configuration();
    config.loss_step = true;
    config.accounts[1000].available_balance = ex::usd(150);
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    ASSERT_TRUE(host.submit(group).succeeded());
    host.broker_result(group, 0, -100);
    ASSERT_TRUE(host.observe(group).succeeded());
    ASSERT_TRUE(host.complete(group).succeeded());
    const auto journal_size = host.storage().journal_events().values.size();
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    EXPECT_EQ(host.storage().journal_events().values.size(), journal_size);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(host.mm_state().state_revision, 1U);
    EXPECT_EQ(submissions(broker), 1U);
}

TEST(TradingSystemBatch6, LossStepPolicyRejectsInvalidMultiplierAndOverflow) {
    ts::TradingDecisionContext context;
    for (const auto multiplier : {-1, 0, 1}) {
        EXPECT_EQ(ts::LossStepStake(ex::usd(100), multiplier).evaluate(context, false).reason,
                  ts::SizingReason::INVALID_CONFIGURATION);
        EXPECT_FALSE(ts::LossStepStake(ex::usd(100), multiplier).evaluate(context, true).amount);
    }
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    EXPECT_EQ(ts::LossStepStake(ex::usd(maximum), 2).evaluate(context, true).reason,
              ts::SizingReason::NUMERIC_OVERFLOW);
    EXPECT_EQ(ts::LossStepStake(ex::usd(maximum), 2).evaluate(context, false).amount->units, maximum);
    EXPECT_EQ(ts::LossStepStake(ex::usd(maximum / 2), 2).evaluate(context, true).amount->units,
              (maximum / 2) * 2);
    EXPECT_FALSE(ts::LossStepStake(ex::usd(0), 2).evaluate(context, true).amount);
}

enum class CrashBoundary { PREPARED, RESPONSE_LOST, PHYSICAL_SETTLED, LOGICAL_COMMITTED };
class SyntheticRestart : public testing::TestWithParam<CrashBoundary> {};

TEST_P(SyntheticRestart, ReopenHydratesNewOwnersAndNeverResendsPossibleExecution) {
    Database db;
    auto broker = std::make_shared<Broker::State>();
    ts::ExecutionGroupId group = 0;
    std::uint64_t journal_revision = 0;
    std::string operation_id;
    {
        ex::SyntheticHost host(db.config(), configuration(), noon, broker);
        ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
        group = host.series().current_group_id;
        operation_id = host.operations(group).front().operation_id;
        if (GetParam() != CrashBoundary::PREPARED) {
            ASSERT_TRUE(host.submit(group, ts::SimulatedSubmissionPlan::ACCEPTED_RESPONSE_LOST).succeeded());
            host.broker_result(group, 0, 80);
        }
        if (GetParam() == CrashBoundary::PHYSICAL_SETTLED || GetParam() == CrashBoundary::LOGICAL_COMMITTED) {
            ASSERT_TRUE(host.observe(group).succeeded());
        }
        if (GetParam() == CrashBoundary::LOGICAL_COMMITTED) ASSERT_TRUE(host.complete(group).succeeded());
        journal_revision = host.operations(group).front().state_revision;
    } // Every OptionX owner, manager and MDBX connection is destroyed here.
    ex::SyntheticHost host(db.config(), configuration(), noon, broker);
    ASSERT_TRUE(host.storage().admission_scope_index_verified());
    auto operation = host.operations(group).front();
    EXPECT_EQ(operation.operation_id, operation_id);
    EXPECT_EQ(operation.account_id, 11U);
    EXPECT_EQ(operation.state_revision, journal_revision);
    const auto roster = ex::read(host.storage().roster(group));
    EXPECT_EQ(roster.members.front().trade_id, operation.trade_id);
    EXPECT_EQ(roster.members.front().account_id, operation.account_id);
    EXPECT_EQ(host.reserved(1000), GetParam() == CrashBoundary::LOGICAL_COMMITTED ? 0 : 100);
    if (GetParam() == CrashBoundary::PREPARED) {
        EXPECT_EQ(submissions(broker), 0U);
        ASSERT_TRUE(host.submit(group).succeeded());
        host.broker_result(group, 0, 80);
    } else {
        EXPECT_FALSE(host.submit(group).succeeded());
        EXPECT_EQ(submissions(broker), 1U);
    }
    ASSERT_TRUE(host.observe(group).succeeded());
    const auto completed = host.complete(group);
    ASSERT_TRUE(completed.succeeded()) << completed.message;
    EXPECT_EQ(host.series().series.next_step, 1U);
    EXPECT_EQ(host.series().series.state_revision, 2U);
    EXPECT_EQ(host.mm_state().state_revision, 1U);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(host.storage().settlements_for_group(group).values.size(), 1U);
    EXPECT_EQ(host.complete(group).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(host.storage().journal_events("GROUP_COMPLETION").values.size(), 1U);
    EXPECT_EQ(submissions(broker), 1U);
    EXPECT_GE(host.operations(group).front().state_revision, journal_revision);
}

INSTANTIATE_TEST_SUITE_P(CrashBoundaries, SyntheticRestart, testing::Values(
    CrashBoundary::PREPARED, CrashBoundary::RESPONSE_LOST,
    CrashBoundary::PHYSICAL_SETTLED, CrashBoundary::LOGICAL_COMMITTED));

TEST(TradingSystemBatch6, MultiAccountSharedScopeWaitsForLateMemberAcrossRestart) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 1000}, {13, 2000}};
    config.accounts[2000] = config.accounts[1000];
    auto broker = std::make_shared<Broker::State>();
    ts::ExecutionGroupId group;
    {
        ex::SyntheticHost host(db.config(), config, noon, broker);
        ASSERT_TRUE(host.accept(ex::synthetic_buy()).succeeded());
        group = host.series().current_group_id;
        EXPECT_EQ(host.reserved(1000), 200);
        EXPECT_EQ(host.reserved(2000), 100);
        ASSERT_TRUE(host.submit(group).succeeded());
        host.broker_result(group, 0, 80);
        host.broker_result(group, 1, -100);
        ASSERT_TRUE(host.observe(group).succeeded());
        EXPECT_FALSE(host.complete(group).succeeded());
        EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
        EXPECT_EQ(host.series().series.next_step, 0U);
        EXPECT_EQ(host.mm_state().state_revision, 0U);
        EXPECT_EQ(host.reserved(1000), 200);
    }
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_EQ(host.reserved(1000), 200);
    EXPECT_EQ(host.reserved(2000), 100);
    host.broker_result(group, 2, 80);
    ASSERT_TRUE(host.observe(group).succeeded());
    const auto result = host.complete(group);
    ASSERT_TRUE(result.succeeded()) << result.message;
    EXPECT_EQ(host.series().series.next_step, 1U);
    EXPECT_EQ(host.series().series.state_revision, 2U);
    EXPECT_EQ(host.series().current_outcome.net_profit->units, 60);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(host.reserved(2000), 0);
    EXPECT_EQ(host.complete(group).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(submissions(broker), 3U);
    for (const auto& reservation : host.storage().reservations().values) {
        EXPECT_EQ(reservation.state, ts::RiskReservationState::SETTLED);
        EXPECT_EQ(reservation.state_revision, 1U);
    }
    const auto roster = ex::read(host.storage().roster(group));
    for (const auto& member : roster.members) {
        EXPECT_EQ(ex::read(host.storage().physical_trade(member.trade_id)).account_id, member.account_id);
    }
}

TEST(TradingSystemBatch6, SharedScopeBudgetIsSumNotOneMemberStake) {
    Database db;
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 1000}};
    config.accounts[1000].available_balance = ex::usd(150);
    auto broker = std::make_shared<Broker::State>();
    ex::SyntheticHost host(db.config(), config, noon, broker);
    EXPECT_FALSE(host.accept(ex::synthetic_buy()).succeeded());
    expect_no_execution(host, broker);
}

TEST(TradingSystemBatch6, ChangedDurablePolicyOrAccountBindingFailsClosed) {
    Database db;
    auto config = configuration();
    auto broker = std::make_shared<Broker::State>();
    { ex::SyntheticHost host(db.config(), config, noon, broker); }
    auto changed = config;
    changed.loss_multiplier = 3;
    EXPECT_THROW(ex::SyntheticHost(db.config(), changed, noon, broker), std::runtime_error);
    changed = config;
    changed.targets.front().account_id = 99;
    EXPECT_THROW(ex::SyntheticHost(db.config(), changed, noon, broker), std::runtime_error);
    EXPECT_EQ(submissions(broker), 0U);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
