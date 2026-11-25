#include <optionx_cpp/trading_system_platform.hpp>
#include <gtest/gtest.h>
#include "support/trading_system/SyntheticHost.hpp"

#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <new>

// Narrow, thread-local fault injection: the next DTO allocation, not storage.
namespace { thread_local bool fail_next_allocation = false; }
void* operator new(std::size_t size) {
    if (fail_next_allocation) { fail_next_allocation = false; throw std::bad_alloc(); }
    if (auto* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace ts = optionx::trading_system;
namespace detail = ts::detail;
namespace support = optionx::test_support;
using optionx::TradeState;

namespace {
constexpr std::int64_t now = 9 * 3600 * 1000;

struct Database {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("optionx_batch7db_" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    ~Database() { std::error_code error; std::filesystem::remove(path, error); }
    mdbxc::Config config() const {
        mdbxc::Config value; value.pathname = path.string(); value.no_subdir = true; value.max_dbs = 18; return value;
    }
};

class FakePlatform : public optionx::platforms::BaseTradingPlatform {
public:
    FakePlatform() : BaseTradingPlatform(nullptr) {}
    std::unique_ptr<optionx::TradeRequest> request;
    std::vector<TradeState> immediate;
    std::function<void()> during_call;
    TradeState query_state = TradeState::WIN;
    int sends = 0;
    int queries = 0;
    std::int64_t broker_id = 91;
    bool accept = true;
    optionx::PlatformType platform_type() const override { return optionx::PlatformType::INTRADE_BAR; }
    bool place_trade(std::unique_ptr<optionx::TradeRequest> value) override {
        ++sends; request = std::move(value);
        if (during_call) during_call();
        for (auto state : immediate) emit(state);
        return accept;
    }
    void emit(TradeState state) {
        auto result = request->create_trade_result_unique();
        result->trade_state = state;
        result->platform_type = platform_type();
        result->option_id = state == TradeState::OPEN_ERROR ? 0 : broker_id;
        result->profit = state == TradeState::WIN ? 0.8 : state == TradeState::LOSS ? -1 : 0;
        request->dispatch_callbacks(request, result);
    }
    bool fetch_trade_result(optionx::TradeResultQuery query, std::unique_ptr<optionx::TradeResult> value,
                            trade_result_check_callback_t callback) override {
        ++queries;
        EXPECT_EQ(query.option_id, broker_id);
        value->trade_state = query_state;
        value->profit = query_state == TradeState::WIN ? 0.8 : 0;
        callback(std::move(value));
        return true;
    }
};

class Batch7DB : public testing::Test {
protected:
    Database db;
    support::SyntheticConfig config;
    std::shared_ptr<ts::SimulatedExecutionAdapter::State> broker = std::make_shared<ts::SimulatedExecutionAdapter::State>();
    std::unique_ptr<support::SyntheticHost> host;
    ts::FakeExecutionClock clock{now};
    ts::ExecutionRecoveryManager manager{clock};
    std::vector<ts::ExecutionOperationRecord> operations;
    std::vector<detail::platform_receipts::Binding> bindings;
    std::vector<std::shared_ptr<FakePlatform>> platforms;
    std::unique_ptr<detail::OfflinePlatformOwner> owner;
    int completions = 0;

    void setup(bool multi = false) {
        config.loss_step = true;
        config.accounts[1000] = {ts::RiskAvailability::SUPPORTED, support::usd(10000), now, 1};
        if (multi) {
            config.targets.push_back({12, 2000});
            config.accounts[2000] = {ts::RiskAvailability::SUPPORTED, support::usd(10000), now, 1};
        }
        reopen();
        auto signal = support::synthetic_buy(); signal.option_type = optionx::OptionType::SPRINT;
        support::require(host->accept(signal));
        operations = host->operations(host->series().current_group_id);
        {
            auto unit = host->storage().begin();
            for (const auto& op : operations) support::require(unit.put_account(
                {op.account_id, "IntradeBar", "account-" + std::to_string(op.account_id), "DEMO", op.account_id, op.risk_scope_id}));
            support::require(unit.commit());
        }
        for (const auto& op : operations) {
            auto captured = manager.capture_platform_binding(host->storage(), op, optionx::CurrencyType::USD, 2);
            ASSERT_TRUE(captured.succeeded()) << captured.message;
            bindings.push_back(*captured.binding);
        }
    }
    void reopen() {
        host.reset(); host = std::make_unique<support::SyntheticHost>(db.config(), config, now, broker);
    }
    void start(std::size_t capacity = 64, std::size_t bytes = 1024 * 1024) {
        std::vector<detail::OfflinePlatformOwner::Endpoint> endpoints;
        platforms.clear();
        for (const auto& op : operations) {
            auto platform = std::make_shared<FakePlatform>(); platform->broker_id += static_cast<std::int64_t>(platforms.size());
            endpoints.push_back({platform, support::read(host->storage().account(op.account_id))});
            platforms.push_back(platform);
        }
        owner = std::make_unique<detail::OfflinePlatformOwner>(host->storage(), manager, std::move(endpoints),
            [this](auto group) { ++completions; return host->complete(group); }, capacity, bytes);
        ASSERT_TRUE(owner->start().succeeded());
    }
    ts::RiskReservationState risk(std::size_t member = 0) {
        const auto roster = support::read(host->storage().roster(operations[member].group_id));
        return support::read(host->storage().reservation(roster.reservation_bindings[member].reservation_id)).state;
    }
    void TearDown() override { owner.reset(); }
};

TEST_F(Batch7DB, SynchronousFinalCompletesPhysicalGroupSeriesAndMmOnce) {
    setup(); start();
    platforms[0]->immediate = {TradeState::OPEN_SUCCESS, TradeState::CHECK_ERROR, TradeState::WIN};
    platforms[0]->during_call = [&] { auto unit = host->storage().begin(); support::require(unit.commit()); };
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded());
    EXPECT_EQ(platforms[0]->sends, 0);
    ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(platforms[0]->sends, 1);
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(risk(), ts::RiskReservationState::SETTLED);
    const auto revision = host->mm_state().state_revision;
    platforms[0]->emit(TradeState::WIN); platforms[0]->emit(TradeState::OPEN_SUCCESS);
    ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(host->mm_state().state_revision, revision);
    EXPECT_EQ(completions, 1);
}

TEST_F(Batch7DB, FinancialOutcomesAndFailedOpeningUseExistingCompletion) {
    setup(); start();
    platforms[0]->immediate = {TradeState::OPEN_ERROR};
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded());
    ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(risk(), ts::RiskReservationState::RELEASED);
    EXPECT_EQ(completions, 1);
    EXPECT_FALSE(host->mm_state().payload.at("after_loss").get<bool>());
}

TEST_F(Batch7DB, NewlyPreparedOperationAfterStartupUsesSameOwnerWithoutJournalRescan) {
    setup(); start(); platforms[0]->immediate = {TradeState::WIN};
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded()); ASSERT_TRUE(owner->process().succeeded());
    auto signal = support::synthetic_buy(); signal.option_type = optionx::OptionType::SPRINT;
    support::require(host->accept(signal));
    const auto fresh = host->operations(host->series().current_group_id).at(0);
    const auto captured = manager.capture_platform_binding(host->storage(), fresh, optionx::CurrencyType::USD, 2);
    ASSERT_TRUE(captured.succeeded());
    platforms[0]->broker_id = 92;
    ASSERT_TRUE(owner->submit(fresh, *captured.binding).succeeded()); ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(platforms[0]->sends, 2); EXPECT_EQ(completions, 2);
    EXPECT_EQ(host->mm_state().state_revision, 2u);
}

TEST_F(Batch7DB, PartialMultiAccountGroupDoesNotAdvanceUntilLastTerminal) {
    setup(true); start();
    platforms[0]->immediate = {TradeState::LOSS};
    platforms[1]->immediate = {TradeState::OPEN_SUCCESS, TradeState::CHECK_ERROR};
    for (std::size_t i = 0; i < 2; ++i) ASSERT_TRUE(owner->submit(operations[i], bindings[i]).succeeded());
    ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(completions, 0);
    EXPECT_EQ(risk(1), ts::RiskReservationState::RESERVED);
    platforms[1]->emit(TradeState::STANDOFF);
    ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(completions, 1);
    EXPECT_TRUE(host->mm_state().payload.at("after_loss").get<bool>());
}

TEST_F(Batch7DB, AcceptedTaskCancelledAtShutdownRemainsUncertainAndNeverResends) {
    setup(); start();
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded());
    EXPECT_FALSE(owner->shutdown().succeeded());
    EXPECT_EQ(platforms[0]->sends, 0);
    EXPECT_EQ(risk(), ts::RiskReservationState::RESERVED);
    EXPECT_EQ(ts::ExecutionRecoveryManager::recover(host->storage()).operations.at(operations[0].operation_id).state,
              ts::ExecutionOperationState::SUBMISSION_STARTED);
    owner.reset(); reopen(); start();
    EXPECT_FALSE(owner->submit(operations[0], bindings[0]).succeeded());
    ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(platforms[0]->sends, 0);
}

TEST_F(Batch7DB, SchedulerRejectionRetainsSubmissionBoundary) {
    setup(); start(); platforms[0]->shutdown();
    EXPECT_FALSE(owner->submit(operations[0], bindings[0]).succeeded());
    EXPECT_FALSE(owner->ready()); EXPECT_EQ(risk(), ts::RiskReservationState::RESERVED);
}

TEST_F(Batch7DB, MissingCallbackNeverCompletesOrResends) {
    setup(); start();
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded());
    ASSERT_TRUE(owner->process().succeeded()); ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(platforms[0]->sends, 1); EXPECT_EQ(completions, 0);
    ASSERT_TRUE(owner->shutdown().succeeded());
    EXPECT_EQ(risk(), ts::RiskReservationState::RESERVED);
    EXPECT_EQ(ts::ExecutionRecoveryManager::recover(host->storage()).operations.at(operations[0].operation_id).state,
              ts::ExecutionOperationState::SUBMISSION_STARTED);
}

TEST_F(Batch7DB, LocalApiFalseReturnDoesNotInventBrokerRejection) {
    setup(); start(); platforms[0]->accept = false;
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded());
    EXPECT_FALSE(owner->process().succeeded()); EXPECT_FALSE(owner->ready());
    EXPECT_EQ(platforms[0]->sends, 1); EXPECT_EQ(completions, 0);
    EXPECT_EQ(risk(), ts::RiskReservationState::RESERVED);
}

TEST_F(Batch7DB, CrashLosesQueueThenKnownIdQueryRecoversWithoutReopening) {
    setup(); start(); platforms[0]->immediate = {TradeState::OPEN_SUCCESS};
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded());
    ASSERT_TRUE(owner->process().succeeded());
    platforms[0]->emit(TradeState::WIN); // Not drained: simulate process interruption.
    auto old_platform = platforms[0];
    owner.reset();
    old_platform->emit(TradeState::WIN); // Stale generation after destruction is harmless.
    reopen(); start();
    EXPECT_EQ(risk(), ts::RiskReservationState::RESERVED);
    auto recovered = ts::ExecutionRecoveryManager::recover(host->storage());
    auto current = recovered.operations.at(operations[0].operation_id);
    EXPECT_FALSE(owner->query(bindings[0], current.state_revision - 1).succeeded());
    ASSERT_TRUE(owner->query(bindings[0], current.state_revision).succeeded());
    ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(platforms[0]->queries, 1); EXPECT_EQ(platforms[0]->sends, 0);
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(risk(), ts::RiskReservationState::SETTLED);
}

TEST_F(Batch7DB, RestartAfterPhysicalCommitAutomaticallyCompletesLogicalGroup) {
    setup();
    support::require(manager.mark_submission_started(host->storage(), operations[0]));
    auto result = bindings[0].request().create_trade_result_unique();
    result->platform_type = optionx::PlatformType::INTRADE_BAR;
    result->trade_state = TradeState::WIN; result->option_id = 91; result->profit = 0.8;
    ASSERT_TRUE(manager.apply_platform_receipt(host->storage(), bindings[0], *result, "before-crash", 1).succeeded());
    reopen(); start(); ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(completions, 1); EXPECT_EQ(risk(), ts::RiskReservationState::SETTLED);
    owner.reset(); reopen(); start(); ASSERT_TRUE(owner->process().succeeded());
    EXPECT_EQ(completions, 2); // The second call is a durable DUPLICATE, not another MM update.
    EXPECT_EQ(host->mm_state().state_revision, 1u);
}

TEST_F(Batch7DB, WrongBrokerConflictBlocksAndLeavesCanonicalResult) {
    setup(); start(); platforms[0]->immediate = {TradeState::OPEN_SUCCESS};
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded()); ASSERT_TRUE(owner->process().succeeded());
    platforms[0]->broker_id = 123; platforms[0]->emit(TradeState::WIN);
    EXPECT_FALSE(owner->process().succeeded()); EXPECT_FALSE(owner->ready());
    EXPECT_EQ(completions, 0); EXPECT_EQ(risk(), ts::RiskReservationState::RESERVED);
}

TEST_F(Batch7DB, CheckErrorWithoutOpeningCannotAuthorizeKnownIdQuery) {
    setup(); start(); platforms[0]->immediate = {TradeState::CHECK_ERROR};
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded());
    ASSERT_TRUE(owner->process().succeeded());
    const auto recovered = ts::ExecutionRecoveryManager::recover(host->storage());
    const auto revision = recovered.operations.at(operations[0].operation_id).state_revision;
    EXPECT_FALSE(owner->query(bindings[0], revision).succeeded());
    EXPECT_EQ(platforms[0]->queries, 0); EXPECT_EQ(completions, 0);
    EXPECT_EQ(risk(), ts::RiskReservationState::RESERVED);
}

TEST_F(Batch7DB, ChangedAccountRealmFailsBeforeAnySubmission) {
    setup(); start();
    auto account = support::read(host->storage().account(operations[0].account_id));
    account.realm = "REAL"; ++account.identity_revision;
    {
        auto unit = host->storage().begin(); support::require(unit.put_account(account)); support::require(unit.commit());
    }
    EXPECT_FALSE(owner->submit(operations[0], bindings[0]).succeeded());
    EXPECT_EQ(platforms[0]->sends, 0);
    EXPECT_EQ(ts::ExecutionRecoveryManager::recover(host->storage()).operations.at(operations[0].operation_id).state,
              ts::ExecutionOperationState::PREPARED);
}

TEST_F(Batch7DB, ConflictingLateSettlementNeverAdvancesMmTwice) {
    setup(); start(); platforms[0]->immediate = {TradeState::WIN};
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded()); ASSERT_TRUE(owner->process().succeeded());
    const auto revision = host->mm_state().state_revision;
    platforms[0]->emit(TradeState::LOSS);
    EXPECT_FALSE(owner->process().succeeded()); EXPECT_FALSE(owner->ready());
    EXPECT_EQ(host->mm_state().state_revision, revision);
    EXPECT_EQ(host->storage().settlements_for_group(operations[0].group_id).values.size(), 1u);
}

TEST_F(Batch7DB, OverflowAndByteBudgetFailClosed) {
    setup(); start(1);
    platforms[0]->immediate = {TradeState::OPEN_SUCCESS, TradeState::WIN};
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded());
    EXPECT_FALSE(owner->process().succeeded()); EXPECT_FALSE(owner->ready());
    EXPECT_EQ(completions, 0); EXPECT_EQ(risk(), ts::RiskReservationState::RESERVED);
}

TEST_F(Batch7DB, CallbackFromWorkerIsQueuedAndShutdownDrainsBeforeStorageDestruction) {
    setup(); start();
    ASSERT_TRUE(owner->submit(operations[0], bindings[0]).succeeded()); ASSERT_TRUE(owner->process().succeeded());
    std::thread worker([&] { platforms[0]->emit(TradeState::WIN); }); worker.join();
    EXPECT_EQ(completions, 0);
    ASSERT_TRUE(owner->shutdown().succeeded()); EXPECT_EQ(completions, 1);
    platforms[0]->emit(TradeState::LOSS);
    EXPECT_EQ(host->mm_state().state_revision, 1u);
}

TEST_F(Batch7DB, StringBoundsAndAllocationFailureNeverTouchStorage) {
    setup();
    detail::PlatformObservationHandoff inbox(host->storage(), manager, 4);
    ASSERT_TRUE(inbox.start().succeeded()); auto sink = inbox.make_sink(bindings[0]);
    auto result = bindings[0].request().create_trade_result_unique();
    result->error_desc.assign(4097, 'x');
    EXPECT_EQ(sink(*result, "large"), detail::ObservationIngress::TOO_LARGE);
    EXPECT_FALSE(inbox.ready());
    detail::PlatformObservationHandoff allocating(host->storage(), manager, 4);
    ASSERT_TRUE(allocating.start().succeeded()); auto fail = allocating.make_sink(bindings[0]);
    result->error_desc.clear(); const std::string receipt = "allocation";
    fail_next_allocation = true;
    const auto status = fail(*result, receipt);
    fail_next_allocation = false;
    EXPECT_EQ(status, detail::ObservationIngress::QUEUE_FULL); EXPECT_FALSE(allocating.ready());
    detail::PlatformObservationHandoff tiny(host->storage(), manager, 4, 1);
    ASSERT_TRUE(tiny.start().succeeded());
    EXPECT_EQ(tiny.make_sink(bindings[0])(*result, "tiny"), detail::ObservationIngress::QUEUE_FULL);
    EXPECT_EQ(host->storage().settlements_for_group(operations[0].group_id).values.size(), 0u);
}

TEST_F(Batch7DB, MultipleCallbackThreadsTransferOwnedBoundedObservations) {
    setup(); support::require(manager.mark_submission_started(host->storage(), operations[0]));
    detail::PlatformObservationHandoff inbox(host->storage(), manager, 64);
    ASSERT_TRUE(inbox.start().succeeded()); auto sink = inbox.make_sink(bindings[0]);
    auto result = bindings[0].request().create_trade_result_unique();
    result->trade_state = TradeState::OPEN_SUCCESS; result->platform_type = optionx::PlatformType::INTRADE_BAR;
    result->option_id = 91;
    const auto before = host->storage().journal_events().values.size();
    std::atomic<int> accepted{0}; std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&, i] {
        for (int n = 0; n < 8; ++n) if (sink(*result, std::to_string(i) + ":" + std::to_string(n)) ==
            detail::ObservationIngress::ACCEPTED) ++accepted;
    });
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(accepted.load(), 32); EXPECT_EQ(host->storage().journal_events().values.size(), before);
    EXPECT_EQ(inbox.drain().processed, 32u); EXPECT_TRUE(inbox.ready());
    EXPECT_EQ(risk(), ts::RiskReservationState::RESERVED);
}

TEST_F(Batch7DB, BrokerHashAndReceiptIdentityBoundsRejectBeforeCopy) {
    setup();
    auto result = bindings[0].request().create_trade_result_unique();
    detail::PlatformObservationHandoff hashes(host->storage(), manager, 4);
    ASSERT_TRUE(hashes.start().succeeded());
    result->option_hash.assign(257, 'h');
    EXPECT_EQ(hashes.make_sink(bindings[0])(*result, "hash"), detail::ObservationIngress::TOO_LARGE);
    detail::PlatformObservationHandoff receipts(host->storage(), manager, 4);
    ASSERT_TRUE(receipts.start().succeeded()); result->option_hash.clear();
    EXPECT_EQ(receipts.make_sink(bindings[0])(*result, std::string(257, 'r')), detail::ObservationIngress::TOO_LARGE);
}
} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS();
}
