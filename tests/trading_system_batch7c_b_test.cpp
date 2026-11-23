#include <optionx_cpp/trading_system.hpp>
#include <gtest/gtest.h>
#include "support/trading_system/SyntheticHost.hpp"

#include <chrono>
#include <filesystem>
#include <type_traits>

namespace ts = optionx::trading_system;
namespace pr = ts::detail::platform_receipts;
namespace pm = ts::detail::platform_mapping;
namespace support = optionx::test_support;
using optionx::TradeState;
using optionx::CurrencyType;

namespace {
constexpr std::int64_t now = 9 * 3600 * 1000;

struct TestDatabase {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("optionx_batch7cb_" + std::to_string(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()) + ".mdbx");
    ~TestDatabase() { std::error_code error; std::filesystem::remove(path, error); }
    mdbxc::Config config() const {
        mdbxc::Config result;
        result.pathname = path.string();
        result.no_subdir = true;
        result.max_dbs = 18;
        return result;
    }
};

class TradingSystemBatch7CB : public testing::Test {
protected:
    TestDatabase database;
    support::SyntheticConfig config;
    std::shared_ptr<ts::SimulatedExecutionAdapter::State> broker =
        std::make_shared<ts::SimulatedExecutionAdapter::State>();
    std::unique_ptr<support::SyntheticHost> host;
    ts::FakeExecutionClock clock{now};
    ts::ExecutionRecoveryManager manager{clock};
    std::vector<pr::Binding> bindings;
    std::vector<ts::ExecutionOperationRecord> operations;

    void setup(bool multi = false, bool alias = false) {
        config.loss_step = true;
        config.accounts[1000] = {ts::RiskAvailability::SUPPORTED, support::usd(10000), now, 1};
        if (multi) {
            config.targets.push_back({12, alias ? 1000u : 2000u});
            config.accounts[2000] = {ts::RiskAvailability::SUPPORTED, support::usd(10000), now, 1};
        }
        reopen();
        auto signal = support::synthetic_buy();
        signal.option_type = optionx::OptionType::SPRINT;
        support::require(host->accept(signal));
        operations = host->operations(host->series().current_group_id);
        {
            auto unit = host->storage().begin();
            for (const auto& target : config.targets) {
                support::require(unit.put_account({target.account_id, "IntradeBar",
                    alias ? "account-11" : "account-" + std::to_string(target.account_id), "DEMO",
                    alias ? 11u : target.account_id, target.risk_scope_id}));
            }
            support::require(unit.commit());
        }
        for (const auto& operation : operations) {
            const auto captured = manager.capture_platform_binding(host->storage(), operation, CurrencyType::USD, 2);
            if (!captured.succeeded()) throw std::runtime_error(captured.message);
            bindings.push_back(*captured.binding);
        }
    }

    void reopen() {
        host.reset();
        host = std::make_unique<support::SyntheticHost>(database.config(), config, now, broker);
    }

    void start(std::size_t member = 0) {
        support::require(manager.mark_submission_started(host->storage(), operations.at(member)));
        ++operations.at(member).state_revision;
        operations.at(member).state = ts::ExecutionOperationState::SUBMISSION_STARTED;
    }

    optionx::TradeResult result(TradeState state, std::int64_t broker_id = 91, std::size_t member = 0) {
        auto value = *bindings.at(member).request().create_trade_result_unique();
        value.platform_type = optionx::PlatformType::INTRADE_BAR;
        value.option_id = broker_id;
        value.trade_state = state;
        value.profit = state == TradeState::WIN ? 0.8 : state == TradeState::LOSS ? -1.0 : 0;
        return value;
    }

    pr::ApplyResult apply(const optionx::TradeResult& value, const std::string& id, std::size_t member = 0) {
        auto applied = manager.apply_platform_receipt(host->storage(), bindings.at(member), value, id,
                                                      operations.at(member).state_revision);
        if (applied.succeeded() && applied.operation) operations.at(member) = *applied.operation;
        return applied;
    }

    ts::ReservationRecord risk(std::size_t member = 0) {
        const auto roster = support::read(host->storage().roster(operations.at(member).group_id));
        return support::read(host->storage().reservation(roster.reservation_bindings.at(member).reservation_id));
    }

    void alter_journal(const std::string& key, const std::function<void(nlohmann::json&)>& edit) {
        mdbxc::KeyValueTable<std::string, std::string> table(host->storage().connection(), "ts_journal");
        auto wire = nlohmann::json::parse(*table.find(key));
        edit(wire);
        table.insert_or_assign(key, wire.dump());
    }
};

TEST_F(TradingSystemBatch7CB, BindingIsSealedImmutableAndPrecedesSubmission) {
    static_assert(!std::is_default_constructible_v<pr::Binding>);
    static_assert(!std::is_constructible_v<pr::Binding, std::uint64_t, nlohmann::json>);
    setup();
    EXPECT_EQ(manager.capture_platform_binding(host->storage(), operations[0], CurrencyType::USD, 2).kind,
              ts::StorageResultKind::DUPLICATE);
    EXPECT_FALSE(manager.capture_platform_binding(host->storage(), operations[0], CurrencyType::USD, 3).succeeded());
    EXPECT_FALSE(apply(result(TradeState::OPEN_ERROR), "early").succeeded());
    start();
    EXPECT_FALSE(manager.capture_platform_binding(host->storage(), operations[0], CurrencyType::USD, 2).succeeded());
    const auto restored = manager.restore_platform_binding(host->storage(), operations[0].operation_id);
    ASSERT_TRUE(restored.succeeded());
    EXPECT_EQ(restored.binding->document(), bindings[0].document());
}

TEST_F(TradingSystemBatch7CB, FailedOpenAtomicallyRejectsAndReleasesWithoutFinancialOutcome) {
    setup(); start();
    auto raw = result(TradeState::OPEN_ERROR, 0);
    raw.error_desc = "Trade open failed. Server returned an empty response; instrument may be closed or unavailable.";
    const auto applied = apply(raw, "failed-open");
    ASSERT_EQ(applied.kind, ts::StorageResultKind::APPLIED) << applied.message;
    EXPECT_EQ(operations[0].state, ts::ExecutionOperationState::REJECTED);
    const auto settlement = support::read(host->storage().settlement(applied.settlement_id));
    EXPECT_EQ(settlement.settlement.kind, ts::OutcomeKind::REJECTED);
    EXPECT_EQ(settlement.settlement.evidence, ts::ExecutionEvidence::NOT_EXECUTED_PROVEN);
    EXPECT_FALSE(settlement.settlement.net_profit);
    EXPECT_EQ(risk().state, ts::RiskReservationState::RELEASED);
    reopen();
    EXPECT_EQ(host->reserved(1000), 0);
    ASSERT_TRUE(host->complete(operations[0].group_id).succeeded());
    EXPECT_EQ(host->mm_state().state_revision, 1u);
    EXPECT_FALSE(host->mm_state().payload.at("after_loss").get<bool>());
    EXPECT_EQ(apply(raw, "failed-open-again").kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(host->mm_state().state_revision, 1u);
}

TEST_F(TradingSystemBatch7CB, CheckErrorNeverConfirmsOpeningOrReleasesRiskAndPreservesAcceptance) {
    setup(); start();
    ASSERT_TRUE(apply(result(TradeState::CHECK_ERROR), "check-before").succeeded());
    EXPECT_EQ(operations[0].state, ts::ExecutionOperationState::SUBMISSION_STARTED);
    EXPECT_TRUE(operations[0].broker_trade_id.empty());
    ASSERT_TRUE(apply(result(TradeState::OPEN_SUCCESS), "opened").succeeded());
    const auto confirmed = operations[0];
    ASSERT_TRUE(apply(result(TradeState::CHECK_ERROR), "check-after").succeeded());
    EXPECT_EQ(nlohmann::json(operations[0]), nlohmann::json(confirmed));
    EXPECT_EQ(risk().state, ts::RiskReservationState::RESERVED);
    EXPECT_TRUE(host->storage().settlements_for_group(confirmed.group_id).values.empty());
    EXPECT_EQ(apply(result(TradeState::OPEN_ERROR, 0), "contradiction").kind, ts::StorageResultKind::CONFLICT);
    reopen();
    EXPECT_EQ(host->reserved(1000), 100);
    EXPECT_EQ(ts::ExecutionRecoveryManager::recover(host->storage()).operations.at(confirmed.operation_id).state,
              ts::ExecutionOperationState::BROKER_ID_PERSISTED);
}

TEST_F(TradingSystemBatch7CB, UnknownCancellationAndNoCallbackKeepUnresolvedExposure) {
    setup(); start(); reopen();
    EXPECT_EQ(host->reserved(1000), 100);
    for (const auto state : {TradeState::UNKNOWN, TradeState::WAITING_OPEN, TradeState::CANCELED_TRADE}) {
        ASSERT_TRUE(apply(result(state, 0), "state-" + std::to_string(static_cast<int>(state))).succeeded());
        EXPECT_EQ(operations[0].state, ts::ExecutionOperationState::SUBMISSION_STARTED);
        EXPECT_EQ(risk().state, ts::RiskReservationState::RESERVED);
    }
    EXPECT_TRUE(host->storage().settlements_for_group(operations[0].group_id).values.empty());
    EXPECT_EQ(broker->submission_counts.size(), 0u);
}

TEST_F(TradingSystemBatch7CB, AcceptanceWithoutIdCanAcquireIdAfterRestart) {
    setup(); start();
    ASSERT_TRUE(apply(result(TradeState::OPEN_SUCCESS, 0), "accepted").succeeded());
    EXPECT_EQ(operations[0].state, ts::ExecutionOperationState::BROKER_ACCEPTED);
    reopen();
    ASSERT_TRUE(apply(result(TradeState::IN_PROGRESS), "identified").succeeded());
    EXPECT_EQ(operations[0].broker_trade_id, "91");
    reopen();
    EXPECT_TRUE(apply(result(TradeState::WIN), "final").succeeded());
}

TEST_F(TradingSystemBatch7CB, FinalFirstUsesLegalAcceptanceThenSettlementAndCompletesOnce) {
    setup(); start();
    const auto final = result(TradeState::LOSS);
    const auto applied = apply(final, "final-first");
    ASSERT_TRUE(applied.succeeded()) << applied.message;
    EXPECT_EQ(operations[0].state_revision, 3u);
    EXPECT_EQ(operations[0].state, ts::ExecutionOperationState::SETTLED);
    EXPECT_TRUE(ts::ExecutionRecoveryManager::recover(host->storage()).succeeded());
    EXPECT_EQ(risk().state, ts::RiskReservationState::RESERVED);
    EXPECT_EQ(host->mm_state().state_revision, 0u);
    reopen();
    ASSERT_TRUE(host->complete(operations[0].group_id).succeeded());
    EXPECT_EQ(host->mm_state().state_revision, 1u);
    EXPECT_TRUE(host->mm_state().payload.at("after_loss").get<bool>());
    EXPECT_EQ(host->reserved(1000), 0);
    reopen();
    EXPECT_EQ(apply(final, "different-delivery").kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(host->complete(operations[0].group_id).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(host->mm_state().state_revision, 1u);
    EXPECT_EQ(host->storage().settlements_for_group(operations[0].group_id).values.size(), 1u);
}

TEST_F(TradingSystemBatch7CB, ExactReplayBypassesStaleRevisionButChangedFactsConflict) {
    setup(); start();
    const auto raw = result(TradeState::WIN);
    ASSERT_TRUE(apply(raw, "one").succeeded());
    const auto duplicate = manager.apply_platform_receipt(host->storage(), bindings[0], raw, "one", 1);
    EXPECT_EQ(duplicate.kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_FALSE(duplicate.operation);
    auto changed = raw; changed.profit = 0.9;
    EXPECT_EQ(apply(changed, "one").kind, ts::StorageResultKind::CONFLICT);
    EXPECT_EQ(apply(changed, "another").kind, ts::StorageResultKind::CONFLICT);
    EXPECT_EQ(apply(result(TradeState::CHECK_ERROR), "late-check").kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(apply(result(TradeState::OPEN_ERROR, 0), "late-failure").kind, ts::StorageResultKind::CONFLICT);
}

TEST_F(TradingSystemBatch7CB, StaleRevisionCannotApplyNewReceiptOrLeakAnyWrites) {
    setup(); start();
    ASSERT_TRUE(apply(result(TradeState::OPEN_SUCCESS), "accepted").succeeded());
    auto unit = host->storage().begin();
    const auto stale = manager.apply_platform_receipt(unit, bindings[0], result(TradeState::LOSS), "stale", 1);
    EXPECT_EQ(stale.kind, ts::StorageResultKind::CONFLICT);
    EXPECT_FALSE(stale.operation);
    EXPECT_FALSE(unit.commit().succeeded());
    EXPECT_FALSE(host->storage().journal_by_key(pr::receipt_key(operations[0].operation_id, "stale")).found());
}

TEST_F(TradingSystemBatch7CB, IdentityRealmCurrencyTradeAndPlatformMismatchesFailClosed) {
    setup(); start();
    const auto good = result(TradeState::OPEN_ERROR, 0);
    for (int mutation = 0; mutation < 5; ++mutation) {
        auto raw = good;
        switch (mutation) {
            case 0: ++raw.trade_id; break;
            case 1: raw.account_type = optionx::AccountType::REAL; break;
            case 2: raw.currency = CurrencyType::EUR; break;
            case 3: raw.platform_type = optionx::PlatformType::UNKNOWN; break;
            case 4: raw.option_id = -1; break;
        }
        EXPECT_FALSE(apply(raw, "invalid-" + std::to_string(mutation)).succeeded());
    }
    EXPECT_EQ(risk().state, ts::RiskReservationState::RESERVED);
    EXPECT_TRUE(host->storage().settlements_for_group(operations[0].group_id).values.empty());
}

TEST_F(TradingSystemBatch7CB, ReceiptRejectsIdentityChangingEditButDeactivationAllowsRecovery) {
    setup(); start();
    const auto original = support::read(host->storage().account(11));
    {
        auto unit = host->storage().begin();
        auto changed = original;
        changed.external_identity = "other";
        changed.identity_revision++;
        support::require(unit.put_account(changed));
        EXPECT_FALSE(manager.apply_platform_receipt(unit, bindings[0], result(TradeState::OPEN_ERROR, 0), "changed", 1).succeeded());
        EXPECT_FALSE(unit.commit().succeeded());
    }
    {
        auto unit = host->storage().begin();
        auto inactive = original;
        inactive.active = false;
        inactive.identity_revision++;
        support::require(unit.put_account(inactive));
        support::require(unit.commit());
    }
    reopen();
    EXPECT_TRUE(apply(result(TradeState::WIN), "after-deactivation").succeeded());
}

TEST_F(TradingSystemBatch7CB, NumericBrokerIdCannotBelongToTwoAliasMembers) {
    setup(true, true); start(0); start(1);
    ASSERT_TRUE(apply(result(TradeState::OPEN_SUCCESS), "opened").succeeded());
    const auto collision = apply(result(TradeState::WIN, 91, 1), "collision", 1);
    EXPECT_EQ(collision.kind, ts::StorageResultKind::CONFLICT);
    EXPECT_EQ(risk(1).state, ts::RiskReservationState::RESERVED);
    EXPECT_TRUE(apply(result(TradeState::WIN, 92, 1), "separate", 1).succeeded());
}

TEST_F(TradingSystemBatch7CB, DifferentFinancialAccountsCanUseSameBrokerNumber) {
    setup(true); start(0); start(1);
    ASSERT_TRUE(apply(result(TradeState::WIN), "first").succeeded());
    ASSERT_TRUE(apply(result(TradeState::LOSS, 91, 1), "second", 1).succeeded());
    reopen();
    ASSERT_TRUE(host->complete(operations[0].group_id).succeeded());
    EXPECT_EQ(host->mm_state().state_revision, 1u);
    EXPECT_TRUE(host->mm_state().payload.at("after_loss").get<bool>());
    EXPECT_EQ(host->reserved(1000), 0);
    EXPECT_EQ(host->reserved(2000), 0);
}

TEST_F(TradingSystemBatch7CB, MixedGroupWaitsForEveryMemberAndKeepsPhysicalAccountIdentity) {
    setup(true); start(0); start(1);
    ASSERT_TRUE(apply(result(TradeState::OPEN_ERROR, 0), "rejected").succeeded());
    reopen();
    EXPECT_FALSE(host->complete(operations[0].group_id).succeeded());
    EXPECT_EQ(host->reserved(1000), 0);
    EXPECT_EQ(host->reserved(2000), 100);
    ASSERT_TRUE(apply(result(TradeState::WIN, 92, 1), "win", 1).succeeded());
    reopen();
    ASSERT_TRUE(host->complete(operations[0].group_id).succeeded());
    EXPECT_EQ(support::read(host->storage().physical_trade(operations[0].trade_id)).account_id, 11u);
    EXPECT_EQ(support::read(host->storage().physical_trade(operations[1].trade_id)).account_id, 12u);
}

TEST_F(TradingSystemBatch7CB, DroppedUnitRollsBackAcceptanceTerminalEvidenceAndPhysicalSettlement) {
    setup(); start();
    {
        auto unit = host->storage().begin();
        const auto staged = manager.apply_platform_receipt(unit, bindings[0], result(TradeState::LOSS), "dropped", 1);
        ASSERT_TRUE(staged.succeeded()) << staged.message;
        EXPECT_EQ(unit.physical_trade(operations[0].trade_id).value->lifecycle_state, "SETTLED");
    }
    reopen();
    EXPECT_EQ(host->reserved(1000), 100);
    EXPECT_TRUE(host->storage().settlements_for_group(operations[0].group_id).values.empty());
    EXPECT_FALSE(host->storage().journal_by_key(pr::receipt_key(operations[0].operation_id, "dropped")).found());
    EXPECT_EQ(ts::ExecutionRecoveryManager::recover(host->storage()).operations.at(operations[0].operation_id).state,
              ts::ExecutionOperationState::SUBMISSION_STARTED);
    EXPECT_TRUE(apply(result(TradeState::LOSS), "dropped").succeeded());
}

TEST_F(TradingSystemBatch7CB, LaterFailurePoisonsEarlierStagedReceiptAndRiskRelease) {
    setup(); start();
    {
        auto unit = host->storage().begin();
        const auto staged = manager.apply_platform_receipt(unit, bindings[0], result(TradeState::OPEN_ERROR, 0), "staged", 1);
        ASSERT_TRUE(staged.succeeded());
        auto changed = result(TradeState::OPEN_ERROR, 0); changed.error_desc = "different";
        EXPECT_FALSE(manager.apply_platform_receipt(unit, bindings[0], changed, "staged", 1).succeeded());
        EXPECT_FALSE(unit.commit().succeeded());
    }
    reopen();
    EXPECT_EQ(host->reserved(1000), 100);
    EXPECT_TRUE(host->storage().settlements_for_group(operations[0].group_id).values.empty());
}

TEST_F(TradingSystemBatch7CB, StoresOriginalBinary64ProfileAndNormalizedResultAcrossReopen) {
    setup(); start();
    auto raw = result(TradeState::WIN); raw.profit = 0.125;
    const auto applied = apply(raw, "precise");
    ASSERT_TRUE(applied.succeeded());
    reopen();
    const auto restored = manager.restore_platform_binding(host->storage(), operations[0].operation_id);
    ASSERT_TRUE(restored.succeeded());
    const auto fact = manager.platform_receipt_fact(host->storage(), *restored.binding, "precise");
    ASSERT_TRUE(fact.found()) << fact.message;
    const auto& input = fact.value->payload.at("input");
    EXPECT_EQ(input.at("normalization"), pm::money_normalization_rule);
    EXPECT_EQ(input.at("profit_bits"), pr::binary64_bits(0.125));
    EXPECT_EQ(input.at("result_amount_bits"), pr::binary64_bits(raw.amount));
    EXPECT_EQ(fact.value->payload.at("normalized_profit").at("units"), 12);
    EXPECT_EQ(support::read(host->storage().settlement(applied.settlement_id)).native_profit.units, 12);
    EXPECT_EQ(ts::trading_storage_schema_version, 1u);
}

TEST_F(TradingSystemBatch7CB, UnsupportedOrCorruptReceiptCodecIsNotRecomputedOnReplay) {
    setup(); start();
    const auto raw = result(TradeState::WIN);
    ASSERT_TRUE(apply(raw, "one").succeeded());
    const auto key = pr::receipt_key(operations[0].operation_id, "one");
    const auto original = support::read(host->storage().journal_by_key(key));
    for (int mutation = 0; mutation < 10; ++mutation) {
        alter_journal(key, [&](auto& wire) {
            wire = ts::detail::journal_to_json(original);
            switch (mutation) {
                case 0: wire["payload"]["version"] = 2; break;
                case 1: wire["payload"]["input"]["normalization"] = "FUTURE_V2"; break;
                case 2: wire["payload"]["input"]["precision"] = 258; break;
                case 3: wire["payload"]["input"]["profit_bits"] = "bad"; break;
                case 4: wire["payload"]["normalized_profit"]["currency"] = 999; break;
                case 5: wire["payload"]["input"]["binding_event_id"] = 999u; break;
                case 6: wire["payload"]["normalized_profit"]["precision"] = 258; break;
                case 7: wire["payload"]["normalized_profit"]["units"] = 99; break;
                case 8: wire["payload"]["settlement_id"] = 0u; break;
                case 9: wire["payload"]["normalized_profit"] = nullptr; break;
            }
        });
        EXPECT_FALSE(manager.platform_receipt_fact(host->storage(), bindings[0], "one").found()) << mutation;
        EXPECT_FALSE(apply(raw, "one").succeeded()) << mutation;
    }
}

TEST_F(TradingSystemBatch7CB, StandoffAndZeroRefundUseExistingDrawEvidence) {
    setup(); start();
    for (const auto state : {TradeState::STANDOFF, TradeState::REFUND}) {
        auto unit = host->storage().begin();
        const auto staged = manager.apply_platform_receipt(unit, bindings[0], result(state), "draw", 1);
        ASSERT_TRUE(staged.succeeded()) << staged.message;
        const auto evidence = unit.settlement(staged.settlement_id);
        ASSERT_TRUE(evidence.found());
        EXPECT_EQ(evidence.value->settlement.kind, ts::OutcomeKind::DRAW);
        EXPECT_EQ(evidence.value->native_profit.units, 0);
    }
    auto nonzero_refund = result(TradeState::REFUND);
    nonzero_refund.profit = 0.1;
    EXPECT_FALSE(apply(nonzero_refund, "nonzero-refund").succeeded());
    ASSERT_TRUE(apply(result(TradeState::REFUND), "refund").succeeded());
    reopen();
    EXPECT_EQ(apply(result(TradeState::STANDOFF), "equivalent-draw").kind, ts::StorageResultKind::DUPLICATE);
}

TEST_F(TradingSystemBatch7CB, MissingOrChangedDurableBindingCannotProveFailedOpening) {
    setup(); start();
    alter_journal(pr::binding_key(operations[0].operation_id), [](auto& wire) {
        wire["payload"]["account"]["realm"] = "REAL";
    });
    EXPECT_FALSE(apply(result(TradeState::OPEN_ERROR, 0), "forged").succeeded());
    EXPECT_EQ(risk().state, ts::RiskReservationState::RESERVED);
}

TEST_F(TradingSystemBatch7CB, AllocationFailureRollsBackEarlierAcceptanceAndBrokerClaim) {
    setup(); start();
    {
        mdbxc::KeyValueTable<std::string, std::string> sequence(host->storage().connection(), "ts_sequences");
        sequence.insert_or_assign("settlement", std::to_string(ts::external_settlement_namespace_bit));
    }
    const auto rejected = apply(result(TradeState::WIN), "failed-write");
    EXPECT_FALSE(rejected.succeeded());
    EXPECT_FALSE(rejected.operation);
    EXPECT_EQ(rejected.settlement_id, 0u);
    reopen();
    EXPECT_FALSE(host->storage().journal_by_key(pr::broker_key(bindings[0], "91")).found());
    EXPECT_TRUE(host->storage().settlements_for_group(operations[0].group_id).values.empty());
    EXPECT_EQ(ts::ExecutionRecoveryManager::recover(host->storage()).operations.at(operations[0].operation_id).state,
              ts::ExecutionOperationState::SUBMISSION_STARTED);
    EXPECT_EQ(host->reserved(1000), 100);
}

TEST_F(TradingSystemBatch7CB, IncorrectFinancialFactsOrBrokerIdCannotReplaceAcceptedIdentity) {
    setup(); start();
    ASSERT_TRUE(apply(result(TradeState::OPEN_SUCCESS), "accepted").succeeded());
    for (int mutation = 0; mutation < 5; ++mutation) {
        auto raw = result(TradeState::WIN);
        switch (mutation) {
            case 0: raw.option_id = 92; break;
            case 1: raw.amount = 2; break;
            case 2: raw.profit = (std::numeric_limits<double>::infinity)(); break;
            case 3: raw.profit = -0.8; break;
            case 4: raw.profit = 0; break;
        }
        EXPECT_FALSE(apply(raw, "bad-final-" + std::to_string(mutation)).succeeded());
    }
    EXPECT_TRUE(host->storage().settlements_for_group(operations[0].group_id).values.empty());
}

TEST_F(TradingSystemBatch7CB, UnknownOperationCanBeResolvedByBoundFailedOpenWithoutResend) {
    setup(); start();
    support::require(manager.mark_unknown(host->storage(), operations[0]));
    operations[0].state = ts::ExecutionOperationState::EXECUTION_UNKNOWN;
    ++operations[0].state_revision;
    reopen();
    ASSERT_TRUE(apply(result(TradeState::OPEN_ERROR, 0), "authoritative-rejection").succeeded());
    EXPECT_EQ(risk().state, ts::RiskReservationState::RELEASED);
    EXPECT_TRUE(broker->submission_counts.empty());
}

TEST_F(TradingSystemBatch7CB, UnknownBindingAndOperationEnvelopeVersionsFailClosed) {
    setup(); start();
    const auto key = pr::binding_key(operations[0].operation_id);
    const auto original = support::read(host->storage().journal_by_key(key));
    for (const auto& version : std::vector<nlohmann::json>{2, 1.0, "1", 0x100000001ULL}) {
        alter_journal(key, [&](auto& wire) { wire = ts::detail::journal_to_json(original); wire["payload"]["version"] = version; });
        EXPECT_FALSE(manager.restore_platform_binding(host->storage(), operations[0].operation_id).succeeded());
        EXPECT_FALSE(apply(result(TradeState::OPEN_ERROR, 0), "unsupported").succeeded());
    }
    alter_journal(key, [&](auto& wire) { wire = ts::detail::journal_to_json(original); });
    ASSERT_TRUE(apply(result(TradeState::WIN), "settled").succeeded());
    alter_journal(ts::ExecutionRecoveryManager::operation_key(operations[0]), [](auto& wire) {
        wire["payload"]["platform_receipt_version"] = 2;
    });
    EXPECT_FALSE(ts::ExecutionRecoveryManager::recover(host->storage()).succeeded());
}

class FakeApi : public optionx::platforms::BaseTradingApi {
public:
    int sends = 0;
    int queries = 0;
    bool schedule = true;
    bool defer = false;
    optionx::TradeResultQuery last;
    optionx::TradeState state = TradeState::CHECK_ERROR;
    trade_result_check_callback_t pending;
    std::unique_ptr<optionx::TradeResult> seed;
    bool place_trade(std::unique_ptr<optionx::TradeRequest>) override { ++sends; return false; }
    bool fetch_trade_result(optionx::TradeResultQuery query, std::unique_ptr<optionx::TradeResult> result,
                            trade_result_check_callback_t callback) override {
        ++queries; last = query;
        if (!schedule) return false;
        seed = std::move(result);
        pending = std::move(callback);
        if (!defer) deliver();
        return true;
    }
    void deliver() {
        seed->trade_state = state;
        seed->profit = state == TradeState::WIN ? 0.8 : 0;
        auto callback = std::move(pending);
        callback(std::move(seed));
    }
};

TEST_F(TradingSystemBatch7CB, KnownIdQueryUsesExistingApiOutsideUowAndDoesNotResend) {
    setup(); start();
    ASSERT_TRUE(apply(result(TradeState::OPEN_SUCCESS), "accepted").succeeded());
    FakeApi api;
    const auto endpoint = support::read(host->storage().account(11));
    int callbacks = 0;
    auto query = [&](const std::string& receipt) {
        return manager.request_known_result(host->storage(), bindings[0], operations[0].state_revision, api,
            endpoint, [&](auto value) { ++callbacks; EXPECT_TRUE(apply(value, receipt).succeeded()); });
    };
    ASSERT_TRUE(query("check-error").succeeded());
    EXPECT_EQ(operations[0].state, ts::ExecutionOperationState::BROKER_ID_PERSISTED);
    reopen();
    api.state = TradeState::WIN;
    ASSERT_TRUE(query("checked-final").succeeded());
    EXPECT_EQ(api.last.option_id, 91);
    EXPECT_EQ(api.last.trade_id, operations[0].trade_id);
    EXPECT_EQ(api.last.retry_attempts, optionx::TradeResultQuery{}.retry_attempts);
    EXPECT_EQ(callbacks, 2);
    EXPECT_EQ(api.sends, 0);
}

TEST_F(TradingSystemBatch7CB, QueryWithoutIdWrongEndpointStaleRevisionAndSchedulingFailureDoNotSend) {
    setup(); start();
    FakeApi api;
    const auto endpoint = support::read(host->storage().account(11));
    auto request = [&](const auto& account, std::uint64_t revision) {
        return manager.request_known_result(host->storage(), bindings[0], revision, api, account, [](auto) {});
    };
    EXPECT_FALSE(request(endpoint, 1).succeeded());
    ASSERT_TRUE(apply(result(TradeState::OPEN_SUCCESS, 0), "no-id").succeeded());
    EXPECT_FALSE(request(endpoint, operations[0].state_revision).succeeded());
    ASSERT_TRUE(apply(result(TradeState::OPEN_SUCCESS), "id").succeeded());
    auto wrong = endpoint; wrong.realm = "REAL";
    EXPECT_FALSE(request(wrong, operations[0].state_revision).succeeded());
    EXPECT_FALSE(request(endpoint, 1).succeeded());
    EXPECT_EQ(api.queries, 0);
    api.schedule = false;
    EXPECT_FALSE(request(endpoint, operations[0].state_revision).succeeded());
    EXPECT_EQ(api.queries, 1);
    EXPECT_EQ(api.sends, 0);
    EXPECT_EQ(risk().state, ts::RiskReservationState::RESERVED);
}

TEST_F(TradingSystemBatch7CB, DeferredCallbackOutlivesManagerAndStorageWithoutOwnerCapture) {
    setup(); start();
    ASSERT_TRUE(apply(result(TradeState::OPEN_SUCCESS), "accepted").succeeded());
    FakeApi api; api.defer = true;
    std::vector<optionx::TradeResult> queue;
    {
        ts::FakeExecutionClock temporary_clock{now};
        ts::ExecutionRecoveryManager temporary(temporary_clock);
        ASSERT_TRUE(temporary.request_known_result(host->storage(), bindings[0], operations[0].state_revision,
            api, support::read(host->storage().account(11)), [&](auto value) { queue.push_back(value); }).succeeded());
    }
    host.reset();
    api.state = TradeState::WIN;
    api.deliver();
    ASSERT_EQ(queue.size(), 1u);
    reopen();
    ASSERT_TRUE(apply(queue[0], "queued").succeeded());
    EXPECT_EQ(api.sends, 0);
}

TEST_F(TradingSystemBatch7CB, ReceiptCostIsIndependentOfUnrelatedJournalHistory) {
    setup(); start();
    const auto small = apply(result(TradeState::CHECK_ERROR, 0), "small");
    ASSERT_TRUE(small.succeeded());
    {
        auto unit = host->storage().begin();
        for (int i = 0; i < 4096; ++i) {
            support::require(pr::append(unit, "UNRELATED_FIXTURE", "unrelated:" + std::to_string(i),
                "unrelated", 0, now, {{"fixture", i}}));
        }
        support::require(unit.commit());
    }
    const auto large = apply(result(TradeState::CHECK_ERROR, 0), "large");
    ASSERT_TRUE(large.succeeded());
    EXPECT_EQ(small.journal_point_reads, large.journal_point_reads);
    EXPECT_LE(large.journal_point_reads, 6u);
    EXPECT_EQ(large.terminal_index_visits, 0u);
    const auto final = apply(result(TradeState::WIN), "final");
    ASSERT_TRUE(final.succeeded());
    EXPECT_LE(final.journal_point_reads, 6u);
    EXPECT_LE(final.terminal_index_visits, 1u);
    const auto repeat = apply(result(TradeState::WIN), "final");
    EXPECT_EQ(repeat.journal_point_reads, 2u);
    EXPECT_EQ(repeat.terminal_index_visits, 0u);
    RecordProperty("diagnostic_journal_reads", large.journal_point_reads);
    RecordProperty("financial_journal_reads", final.journal_point_reads);
    RecordProperty("financial_member_index_visits", final.terminal_index_visits);
}
} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
