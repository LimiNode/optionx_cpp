#include <optionx_cpp/trading_system.hpp>
#include <gtest/gtest.h>
#include "support/trading_system/SyntheticHost.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

namespace ts = optionx::trading_system;
namespace handoff = optionx::trading_system::detail;
namespace support = optionx::test_support;

namespace {
constexpr std::int64_t now = 9 * 3600 * 1000;

struct TestDatabase {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("optionx_batch7da_" + std::to_string(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()) + ".mdbx");
    ~TestDatabase() { std::error_code error; std::filesystem::remove(path, error); }
    mdbxc::Config config() const {
        mdbxc::Config value;
        value.pathname = path.string();
        value.no_subdir = true;
        value.max_dbs = 18;
        return value;
    }
};

class TradingSystemBatch7DA : public testing::Test {
protected:
    TestDatabase database;
    support::SyntheticConfig config;
    std::shared_ptr<ts::SimulatedExecutionAdapter::State> broker =
        std::make_shared<ts::SimulatedExecutionAdapter::State>();
    std::unique_ptr<support::SyntheticHost> host;
    ts::FakeExecutionClock clock{now};
    ts::ExecutionRecoveryManager manager{clock};
    std::optional<ts::detail::platform_receipts::Binding> binding;
    ts::ExecutionOperationRecord operation;

    void setup() {
        config.loss_step = true;
        config.accounts[1000] = {ts::RiskAvailability::SUPPORTED, support::usd(10000), now, 1};
        host = std::make_unique<support::SyntheticHost>(database.config(), config, now, broker);
        auto signal = support::synthetic_buy();
        signal.option_type = optionx::OptionType::SPRINT;
        support::require(host->accept(signal));
        operation = host->operations(host->series().current_group_id).at(0);
        {
            auto unit = host->storage().begin();
            support::require(unit.put_account({11, "IntradeBar", "account-11", "DEMO", 11, 1000}));
            support::require(unit.commit());
        }
        auto captured = manager.capture_platform_binding(host->storage(), operation,
            optionx::CurrencyType::USD, 2);
        ASSERT_TRUE(captured.succeeded()) << captured.message;
        binding = *captured.binding;
        support::require(manager.mark_submission_started(host->storage(), operation));
        ++operation.state_revision;
        operation.state = ts::ExecutionOperationState::SUBMISSION_STARTED;
    }

    optionx::TradeResult result(optionx::TradeState state, std::int64_t broker_id = 91) {
        auto value = *binding->request().create_trade_result_unique();
        value.platform_type = optionx::PlatformType::INTRADE_BAR;
        value.option_id = broker_id;
        value.trade_state = state;
        value.profit = state == optionx::TradeState::WIN ? 0.8 :
                       state == optionx::TradeState::LOSS ? -1.0 : 0;
        return value;
    }

    std::size_t journal_size() const {
        const auto events = host->storage().journal_events();
        EXPECT_TRUE(events.succeeded());
        return events.values.size();
    }
};

TEST_F(TradingSystemBatch7DA, SynchronousCallbacksOnlyEnqueueAndSettleOnce) {
    setup();
    handoff::PlatformObservationHandoff inbox(host->storage(), manager, 4);
    ASSERT_TRUE(inbox.start().succeeded());
    auto sink = inbox.make_sink(*binding);
    const auto before = journal_size();
    EXPECT_EQ(sink(result(optionx::TradeState::OPEN_SUCCESS), "opened"),
              handoff::ObservationIngress::ACCEPTED);
    EXPECT_EQ(sink(result(optionx::TradeState::CHECK_ERROR), "check"),
              handoff::ObservationIngress::ACCEPTED);
    EXPECT_EQ(journal_size(), before);
    EXPECT_EQ(inbox.drain().processed, 2u);
    EXPECT_EQ(support::read(host->storage().reservation(
        support::read(host->storage().roster(operation.group_id)).reservation_bindings.at(0).reservation_id)).state,
        ts::RiskReservationState::RESERVED);

    EXPECT_EQ(sink(result(optionx::TradeState::WIN), "win"),
              handoff::ObservationIngress::ACCEPTED);
    EXPECT_EQ(inbox.drain().processed, 1u);
    EXPECT_EQ(support::read(host->storage().physical_trade(operation.trade_id)).lifecycle_state, "SETTLED");
    const auto settled = host->storage().settlements_for_group(operation.group_id);
    ASSERT_TRUE(settled.succeeded());
    EXPECT_EQ(settled.values.size(), 1u);
    EXPECT_EQ(sink(result(optionx::TradeState::WIN), "win"),
              handoff::ObservationIngress::ACCEPTED);
    EXPECT_EQ(inbox.drain().processed, 1u);
    EXPECT_EQ(host->storage().settlements_for_group(operation.group_id).values.size(), 1u);
    EXPECT_TRUE(broker->submission_counts.empty());
}

TEST_F(TradingSystemBatch7DA, OverflowBlocksProgressAndRetainsDurableRisk) {
    setup();
    handoff::PlatformObservationHandoff inbox(host->storage(), manager, 1);
    ASSERT_TRUE(inbox.start().succeeded());
    auto sink = inbox.make_sink(*binding);
    const auto before = journal_size();
    EXPECT_EQ(sink(result(optionx::TradeState::OPEN_SUCCESS), "opened"),
              handoff::ObservationIngress::ACCEPTED);
    EXPECT_EQ(sink(result(optionx::TradeState::WIN), "win"),
              handoff::ObservationIngress::QUEUE_FULL);
    EXPECT_FALSE(inbox.ready());
    EXPECT_EQ(journal_size(), before);
    const auto drained = inbox.drain();
    EXPECT_EQ(drained.processed, 1u);
    EXPECT_TRUE(drained.blocked);
    EXPECT_EQ(sink(result(optionx::TradeState::WIN), "late"),
              handoff::ObservationIngress::CLOSED);
    EXPECT_EQ(support::read(host->storage().physical_trade(operation.trade_id)).lifecycle_state,
              "BROKER_ID_PERSISTED");
    EXPECT_TRUE(broker->submission_counts.empty());
}

TEST_F(TradingSystemBatch7DA, ShutdownDrainsAcceptedWorkAndLateSinkIsSafe) {
    setup();
    handoff::PlatformObservationHandoff::Sink sink;
    {
        handoff::PlatformObservationHandoff inbox(host->storage(), manager, 2);
        ASSERT_TRUE(inbox.start().succeeded());
        sink = inbox.make_sink(*binding);
        EXPECT_EQ(sink(result(optionx::TradeState::OPEN_SUCCESS), "opened"),
                  handoff::ObservationIngress::ACCEPTED);
        EXPECT_EQ(inbox.shutdown_and_drain().processed, 1u);
        EXPECT_EQ(sink(result(optionx::TradeState::WIN), "too-late"),
                  handoff::ObservationIngress::CLOSED);
    }
    EXPECT_EQ(sink(result(optionx::TradeState::WIN), "after-destruction"),
              handoff::ObservationIngress::CLOSED);
    EXPECT_EQ(support::read(host->storage().physical_trade(operation.trade_id)).lifecycle_state,
              "BROKER_ID_PERSISTED");
}

TEST_F(TradingSystemBatch7DA, RestartRecoversRevisionAndDuplicateReceipt) {
    setup();
    {
        handoff::PlatformObservationHandoff inbox(host->storage(), manager, 2);
        ASSERT_TRUE(inbox.start().succeeded());
        auto sink = inbox.make_sink(*binding);
        ASSERT_EQ(sink(result(optionx::TradeState::OPEN_SUCCESS), "opened"),
                  handoff::ObservationIngress::ACCEPTED);
        ASSERT_EQ(inbox.drain().processed, 1u);
    }
    host.reset();
    host = std::make_unique<support::SyntheticHost>(database.config(), config, now, broker);
    auto restored = manager.restore_platform_binding(host->storage(), operation.operation_id);
    ASSERT_TRUE(restored.succeeded()) << restored.message;
    handoff::PlatformObservationHandoff inbox(host->storage(), manager, 2);
    ASSERT_TRUE(inbox.start().succeeded());
    auto sink = inbox.make_sink(*restored.binding);
    EXPECT_EQ(sink(result(optionx::TradeState::OPEN_SUCCESS), "opened"),
              handoff::ObservationIngress::ACCEPTED);
    EXPECT_EQ(sink(result(optionx::TradeState::WIN), "win"),
              handoff::ObservationIngress::ACCEPTED);
    EXPECT_EQ(inbox.drain().processed, 2u);
    EXPECT_EQ(host->storage().settlements_for_group(operation.group_id).values.size(), 1u);
    EXPECT_TRUE(broker->submission_counts.empty());
}

TEST_F(TradingSystemBatch7DA, MissingCallbackRemainsUnresolvedAcrossRestart) {
    setup();
    {
        handoff::PlatformObservationHandoff inbox(host->storage(), manager, 2);
        ASSERT_TRUE(inbox.start().succeeded());
        EXPECT_EQ(inbox.shutdown_and_drain().processed, 0u);
    }
    host.reset();
    host = std::make_unique<support::SyntheticHost>(database.config(), config, now, broker);
    const auto recovered = ts::ExecutionRecoveryManager::recover(host->storage());
    ASSERT_TRUE(recovered.succeeded());
    EXPECT_EQ(recovered.operations.at(operation.operation_id).state,
              ts::ExecutionOperationState::SUBMISSION_STARTED);
    const auto roster = support::read(host->storage().roster(operation.group_id));
    EXPECT_EQ(support::read(host->storage().reservation(
        roster.reservation_bindings.at(0).reservation_id)).state,
        ts::RiskReservationState::RESERVED);
    EXPECT_TRUE(broker->submission_counts.empty());
}

TEST_F(TradingSystemBatch7DA, ConflictingTerminalFactsBlockOwnerWithoutReplacingSettlement) {
    setup();
    handoff::PlatformObservationHandoff inbox(host->storage(), manager, 2);
    ASSERT_TRUE(inbox.start().succeeded());
    auto sink = inbox.make_sink(*binding);
    ASSERT_EQ(sink(result(optionx::TradeState::WIN), "terminal"),
              handoff::ObservationIngress::ACCEPTED);
    ASSERT_EQ(inbox.drain().processed, 1u);
    const auto before = journal_size();
    ASSERT_EQ(sink(result(optionx::TradeState::LOSS), "terminal"),
              handoff::ObservationIngress::ACCEPTED);
    const auto conflict = inbox.drain();
    EXPECT_EQ(conflict.kind, ts::StorageResultKind::CONFLICT);
    EXPECT_TRUE(conflict.blocked);
    EXPECT_FALSE(inbox.ready());
    EXPECT_EQ(journal_size(), before);
    EXPECT_EQ(host->storage().settlements_for_group(operation.group_id).values.size(), 1u);
}

TEST_F(TradingSystemBatch7DA, LateOpeningAfterSettlementDoesNotAdvanceState) {
    setup();
    handoff::PlatformObservationHandoff inbox(host->storage(), manager, 2);
    ASSERT_TRUE(inbox.start().succeeded());
    auto sink = inbox.make_sink(*binding);
    ASSERT_EQ(sink(result(optionx::TradeState::WIN), "terminal"),
              handoff::ObservationIngress::ACCEPTED);
    ASSERT_EQ(inbox.drain().processed, 1u);
    const auto settled = support::read(host->storage().physical_trade(operation.trade_id));
    ASSERT_EQ(sink(result(optionx::TradeState::OPEN_SUCCESS), "late-open"),
              handoff::ObservationIngress::ACCEPTED);
    EXPECT_EQ(inbox.drain().processed, 1u);
    EXPECT_EQ(support::read(host->storage().physical_trade(operation.trade_id)).lifecycle_state,
              settled.lifecycle_state);
    EXPECT_EQ(host->storage().settlements_for_group(operation.group_id).values.size(), 1u);
}

TEST_F(TradingSystemBatch7DA, EmptyReceiptIdentityBlocksBeforeStorage) {
    setup();
    handoff::PlatformObservationHandoff inbox(host->storage(), manager, 2);
    ASSERT_TRUE(inbox.start().succeeded());
    auto sink = inbox.make_sink(*binding);
    const auto before = journal_size();
    EXPECT_EQ(sink(result(optionx::TradeState::WIN), ""),
              handoff::ObservationIngress::INVALID_RECEIPT);
    EXPECT_FALSE(inbox.ready());
    EXPECT_EQ(inbox.drain().processed, 0u);
    EXPECT_EQ(journal_size(), before);
    EXPECT_EQ(host->storage().settlements_for_group(operation.group_id).values.size(), 0u);
}

TEST_F(TradingSystemBatch7DA, WorkerCallbackCannotRunOwnerDatabaseDrain) {
    setup();
    handoff::PlatformObservationHandoff inbox(host->storage(), manager, 2);
    ASSERT_TRUE(inbox.start().succeeded());
    auto sink = inbox.make_sink(*binding);
    auto value = result(optionx::TradeState::OPEN_SUCCESS);
    handoff::ObservationIngress accepted = handoff::ObservationIngress::CLOSED;
    handoff::ObservationDrainResult wrong_thread;
    std::thread worker([&] {
        accepted = sink(std::move(value), "worker-open");
        wrong_thread = inbox.drain();
    });
    worker.join();
    EXPECT_EQ(accepted, handoff::ObservationIngress::ACCEPTED);
    EXPECT_EQ(wrong_thread.kind, ts::StorageResultKind::INVALID_STATE);
    EXPECT_EQ(inbox.drain().processed, 1u);
}

TEST_F(TradingSystemBatch7DA, LogicalCompletionRemainsIdempotentAfterLateCallback) {
    setup();
    handoff::PlatformObservationHandoff inbox(host->storage(), manager, 2);
    ASSERT_TRUE(inbox.start().succeeded());
    auto sink = inbox.make_sink(*binding);
    ASSERT_EQ(sink(result(optionx::TradeState::WIN), "terminal"),
              handoff::ObservationIngress::ACCEPTED);
    ASSERT_EQ(inbox.drain().processed, 1u);
    ASSERT_EQ(host->complete(operation.group_id).kind, ts::StorageResultKind::APPLIED);
    const auto checkpoint = support::read(host->storage().series(1));
    const auto mm = support::read(host->storage().money_management("synthetic.mm"));
    ASSERT_EQ(sink(result(optionx::TradeState::WIN), "terminal"),
              handoff::ObservationIngress::ACCEPTED);
    EXPECT_EQ(inbox.drain().processed, 1u);
    EXPECT_EQ(host->complete(operation.group_id).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(support::read(host->storage().series(1)).series.state_revision,
              checkpoint.series.state_revision);
    EXPECT_EQ(support::read(host->storage().money_management("synthetic.mm")).state_revision,
              mm.state_revision);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
