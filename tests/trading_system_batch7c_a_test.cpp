#include <optionx_cpp/trading_system.hpp>
#include <gtest/gtest.h>

#include "support/trading_system/LegacySettlementV1.hpp"
#include "support/trading_system/SyntheticHost.hpp"

#include <chrono>
#include <filesystem>
#include <limits>

namespace ts = optionx::trading_system;
namespace pm = optionx::trading_system::detail::platform_mapping;
namespace support = optionx::test_support;
namespace legacy = optionx::test_support::legacy_settlement_v1;
using optionx::CurrencyType;

namespace {

struct TestDatabase {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("optionx_batch7ca_" + std::to_string(
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

ts::MoneyAmount money(std::int64_t units) { return {units, 2, CurrencyType::USD}; }

support::SyntheticConfig configuration() {
    support::SyntheticConfig result;
    result.accounts[1000] = {ts::RiskAvailability::SUPPORTED, money(1000),
                             std::int64_t{9} * 3600 * 1000, 1};
    return result;
}

ts::TradeSettlement rejected(std::uint64_t id = 900) {
    return {id, 41, 80, 700, ts::OutcomeKind::REJECTED,
            ts::ExecutionEvidence::NOT_EXECUTED_PROVEN, std::nullopt};
}

ts::ReservationRecord reservation(std::uint64_t revision = 0) {
    return {500, 1000, ts::RiskScopeKind::ACCOUNT, 41, {80, 1},
            ts::ExecutionCardinality::EXACTLY_ONE, money(50),
            revision == 0 ? ts::RiskReservationState::RESERVED : ts::RiskReservationState::RELEASED,
            revision, 7, money(50), "risk-41"};
}

ts::SettlementTransition physical_transition(std::uint64_t id = 900) {
    ts::SettlementTransition result;
    result.journal = {1001, "EXECUTION_SETTLED", "trade:700", 0,
                      ts::trading_storage_schema_version, "settlement:" + std::to_string(id),
                      1000, ts::detail::settlement_to_json(rejected(id))};
    result.settlement = {rejected(id), money(0), std::nullopt, true};
    result.physical_trade = ts::PhysicalTradeRecord{700, 41, 80, 11, 1000, "REJECTED", "", 1};
    result.logical_currency = CurrencyType::USD;
    return result;
}

ts::SettlementTransition logical_transition() {
    auto result = physical_transition();
    ts::SeriesCheckpoint checkpoint;
    checkpoint.series = {12, 7, 0, 1};
    checkpoint.status = ts::SeriesStatus::ACTIVE;
    ts::ExecutionGroupAggregator group(41, {80, 1}, {700}, CurrencyType::USD);
    EXPECT_EQ(group.apply(rejected()), ts::ProgressionResult::APPLIED);
    checkpoint.current_outcome = group.outcome();
    checkpoint.receipts = {rejected()};
    checkpoint.used_intents = {41};
    checkpoint.used_intent_keys = {"intent-41"};
    result.series = checkpoint;
    result.reservation = reservation(1);
    result.money_management = ts::MoneyManagementStateRecord{
        "mm.alpha", "test-state", ts::trading_storage_schema_version, 1, 900, {{"step", 0}}};
    return result;
}

void seed(ts::TradingSystemStorage& storage) {
    auto unit = storage.begin();
    ts::SeriesCheckpoint waiting;
    waiting.series = {12, 7, 0, 0};
    waiting.status = ts::SeriesStatus::WAITING_OUTCOME;
    waiting.current_intent_id = 41;
    waiting.current_group_id = 80;
    waiting.used_intents = {41};
    waiting.used_intent_keys = {"intent-41"};
    ASSERT_EQ(unit.put_series(waiting, 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_reservation(reservation(), 0).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_physical_trade({700, 41, 80, 11, 1000, "ADMITTED", "", 0}, 0).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_roster({80, 41, 1, {{700, 11, 1000, money(50)}},
                              {{700, 500, 1000, money(50), money(50), "risk-41"}}}).kind,
              ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.put_money_management(
        {"mm.alpha", "test-state", ts::trading_storage_schema_version, 0, 0, {{"step", 0}}}, 0).kind,
        ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
}

TEST(TradingSystemBatch7CA, EvidenceOrdinalsAndOutcomeMatrixAreExplicit) {
    static_assert(static_cast<int>(ts::ExecutionEvidence::NOT_SUBMITTED) == 0);
    static_assert(static_cast<int>(ts::ExecutionEvidence::MAY_HAVE_EXECUTED) == 1);
    static_assert(static_cast<int>(ts::ExecutionEvidence::EXECUTED) == 2);
    static_assert(static_cast<int>(ts::ExecutionEvidence::NOT_EXECUTED_PROVEN) == 3);
    for (int kind = 0; kind <= 5; ++kind) {
        for (int evidence : {-1, 0, 1, 2, 3, 4, 99}) {
            auto value = rejected();
            value.kind = static_cast<ts::OutcomeKind>(kind);
            value.evidence = static_cast<ts::ExecutionEvidence>(evidence);
            if (kind <= 2) value.net_profit = money(kind == 0 ? 10 : kind == 1 ? -10 : 0);
            const bool valid = kind <= 2 ? evidence == 2 :
                kind == 3 ? (evidence == 0 || evidence == 3) :
                kind == 4 ? evidence == 0 : evidence == 1;
            EXPECT_EQ(value.valid(), valid) << kind << ":" << evidence;
        }
    }
}

TEST(TradingSystemBatch7CA, FailedOpenNeverCarriesFinancialProfitOrValuation) {
    auto value = physical_transition().settlement;
    ASSERT_TRUE(value.valid());
    value.native_profit.units = 1;
    EXPECT_FALSE(value.valid());
    value.native_profit.units = 0;
    value.settlement.net_profit = money(0);
    EXPECT_FALSE(value.valid());
    value.settlement.net_profit.reset();
    value.valuation = ts::ValuationFact{CurrencyType::USD, CurrencyType::EUR, 1, 1,
                                       1000, "fixture", "fx-1"};
    EXPECT_FALSE(value.valid());
}

TEST(TradingSystemBatch7CA, LegacyWireFactsRoundTripWithoutNewFields) {
    for (int kind = 0; kind <= 5; ++kind) {
        auto wire = nlohmann::json::parse(
            R"({"settlement_id":900,"intent_id":41,"execution_group_id":80,"trade_id":700,"kind":3,"evidence":0})");
        wire["kind"] = kind;
        wire["evidence"] = kind <= 2 ? 2 : kind == 5 ? 1 : 0;
        if (kind <= 2) wire["net_profit"] = ts::detail::money_to_json(money(kind == 0 ? 100 : kind == 1 ? -100 : 0));
        ASSERT_TRUE(legacy::decode(wire).valid());
        const auto restored = ts::detail::settlement_from_json(wire);
        ASSERT_TRUE(restored.valid());
        EXPECT_EQ(ts::detail::settlement_to_json(restored).dump(), wire.dump());
        EXPECT_FALSE(ts::detail::settlement_to_json(restored).contains("evidence_version"));
    }
}

TEST(TradingSystemBatch7CA, NewEvidenceRequiresVersionTwoAndFailsLegacyValidation) {
    const auto wire = ts::detail::settlement_to_json(rejected());
    EXPECT_EQ(wire.at("evidence_version"), 2);
    EXPECT_EQ(wire.at("evidence"), 3);
    EXPECT_EQ(ts::detail::settlement_from_json(wire), rejected());
    const auto old = legacy::decode(wire);
    EXPECT_EQ(static_cast<int>(old.evidence), 3);
    EXPECT_FALSE(old.valid()); // The old storage reader/writer validates this predicate.
    auto unversioned = wire;
    unversioned.erase("evidence_version");
    EXPECT_THROW(ts::detail::settlement_from_json(unversioned), std::invalid_argument);
}

TEST(TradingSystemBatch7CA, UnknownOrNarrowingNumericTagsFailClosed) {
    const auto original = ts::detail::settlement_to_json(rejected());
    for (const auto& invalid : std::vector<nlohmann::json>{-1, 0, 3, 99, 2.0, "2", nullptr,
             std::uint64_t{0x100000002ULL}, (std::numeric_limits<std::uint64_t>::max)()}) {
        auto wire = original;
        wire["evidence_version"] = invalid;
        EXPECT_THROW(ts::detail::settlement_from_json(wire), std::invalid_argument);
    }
    for (const auto& invalid : std::vector<nlohmann::json>{-1, 4, 99, 3.0, "3", nullptr,
             std::uint64_t{0x100000003ULL}, (std::numeric_limits<std::uint64_t>::max)()}) {
        auto wire = original;
        wire["evidence"] = invalid;
        EXPECT_THROW(ts::detail::settlement_from_json(wire), std::invalid_argument);
    }
    auto wire = original;
    wire["evidence"] = 0;
    EXPECT_THROW(ts::detail::settlement_from_json(wire), std::invalid_argument);
    wire = original;
    wire["kind"] = std::uint64_t{0x100000003ULL};
    EXPECT_THROW(ts::detail::settlement_from_json(wire), std::invalid_argument);
}

TEST(TradingSystemBatch7CA, GroupRejectsUnprovenFailureAndDeduplicatesProvenFailure) {
    ts::ExecutionGroupAggregator group(41, {80, 2}, {700, 701}, CurrencyType::USD);
    auto proof = rejected();
    proof.evidence = ts::ExecutionEvidence::MAY_HAVE_EXECUTED;
    EXPECT_EQ(group.apply(proof), ts::ProgressionResult::INVALID_OUTCOME);
    proof.evidence = ts::ExecutionEvidence::NOT_EXECUTED_PROVEN;
    EXPECT_EQ(group.apply(proof), ts::ProgressionResult::APPLIED);
    proof.settlement_id++;
    EXPECT_EQ(group.apply(proof), ts::ProgressionResult::DUPLICATE);
    EXPECT_EQ(group.outcome().completed_members, 1U);
    EXPECT_FALSE(group.outcome().final);
    proof.evidence = ts::ExecutionEvidence::NOT_SUBMITTED;
    EXPECT_EQ(group.apply(proof), ts::ProgressionResult::MEMBER_ALREADY_TERMINAL);
    ts::TradeSettlement win{902, 41, 80, 701, ts::OutcomeKind::WIN,
                             ts::ExecutionEvidence::EXECUTED, money(100)};
    EXPECT_EQ(group.apply(win), ts::ProgressionResult::APPLIED);
    EXPECT_EQ(group.outcome().kind, ts::OutcomeKind::WIN);
    ASSERT_TRUE(group.outcome().net_profit);
    EXPECT_EQ(group.outcome().net_profit->units, 100);
}

TEST(TradingSystemBatch7CA, EvidenceAndRiskReleaseRollbackOrCommitTogether) {
    for (int boundary = 0; boundary < 4; ++boundary) {
        TestDatabase database;
        {
            ts::TradingSystemStorage storage(database.config());
            seed(storage);
            auto unit = storage.begin();
            if (boundary < 2) {
                ASSERT_EQ(unit.record_physical_settlement(physical_transition()).kind,
                          ts::StorageResultKind::APPLIED);
                if (boundary == 1) {
                    ASSERT_EQ(unit.put_reservation(reservation(1), 0).kind, ts::StorageResultKind::APPLIED);
                }
                // Destruction simulates interruption before local commit.
            } else {
                ASSERT_EQ(unit.apply_settlement_atomic(logical_transition()).kind,
                          ts::StorageResultKind::APPLIED);
                if (boundary == 2) {
                    auto invalid = *logical_transition().money_management;
                    invalid.schema_version = 99;
                    EXPECT_EQ(unit.put_money_management(invalid, 1).kind,
                              ts::StorageResultKind::UNSUPPORTED_SCHEMA);
                    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
                } else {
                    ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
                }
            }
        }
        ts::TradingSystemStorage reopened(database.config());
        EXPECT_EQ(reopened.settlement(900).found(), boundary == 3);
        EXPECT_EQ(reopened.reservation(500).value->state, boundary == 3 ?
                  ts::RiskReservationState::RELEASED : ts::RiskReservationState::RESERVED);
        EXPECT_EQ(reopened.physical_trade(700).value->lifecycle_state, boundary == 3 ? "REJECTED" : "ADMITTED");
        EXPECT_EQ(reopened.series(12).value->series.state_revision, boundary == 3 ? 1U : 0U);
        EXPECT_EQ(reopened.series(12).value->series.next_step, 0U);
        EXPECT_EQ(reopened.money_management("mm.alpha").value->state_revision, boundary == 3 ? 1U : 0U);
        if (boundary == 3) {
            EXPECT_EQ(reopened.settlement(900).value->settlement.evidence,
                      ts::ExecutionEvidence::NOT_EXECUTED_PROVEN);
            EXPECT_EQ(reopened.settlements_for_group(80).values.size(), 1U);
        } else {
            EXPECT_TRUE(reopened.journal_events().values.empty());
        }
    }
}

TEST(TradingSystemBatch7CA, DuplicateFailedOpenWithDifferentReceiptIdCreatesNoSecondFact) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    seed(storage);
    {
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(logical_transition()).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    for (auto id : {900U, 901U}) {
        auto value = logical_transition();
        value.settlement.settlement.settlement_id = id;
        auto unit = storage.begin();
        EXPECT_EQ(unit.apply_settlement_atomic(value).kind, ts::StorageResultKind::DUPLICATE);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    EXPECT_EQ(storage.settlements_for_group(80).values.size(), 1U);
    EXPECT_EQ(storage.settlement(901).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(storage.series(12).value->series.state_revision, 1U);
    EXPECT_EQ(storage.money_management("mm.alpha").value->state_revision, 1U);
}

TEST(TradingSystemBatch7CA, ConflictingEvidenceCannotOverwriteProvenFailedOpen) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    seed(storage);
    {
        auto unit = storage.begin();
        ASSERT_EQ(unit.apply_settlement_atomic(logical_transition()).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    for (auto evidence : {ts::ExecutionEvidence::NOT_SUBMITTED, ts::ExecutionEvidence::EXECUTED,
                           ts::ExecutionEvidence::MAY_HAVE_EXECUTED}) {
        auto value = physical_transition(901);
        value.settlement.settlement.evidence = evidence;
        if (evidence == ts::ExecutionEvidence::EXECUTED) {
            value.settlement.settlement.kind = ts::OutcomeKind::WIN;
            value.settlement.settlement.net_profit = value.settlement.native_profit = money(100);
        } else if (evidence == ts::ExecutionEvidence::MAY_HAVE_EXECUTED) {
            value.settlement.settlement.kind = ts::OutcomeKind::UNKNOWN;
        }
        auto unit = storage.begin();
        EXPECT_EQ(unit.record_physical_settlement(value).kind, ts::StorageResultKind::CONFLICT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    EXPECT_EQ(storage.settlements_for_group(80).values.size(), 1U);
    EXPECT_EQ(storage.reservation(500).value->state, ts::RiskReservationState::RELEASED);
}

TEST(TradingSystemBatch7CA, UnknownFactDoesNotBecomeRejectionOrReleaseRisk) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    seed(storage);
    auto unknown = physical_transition();
    unknown.settlement.settlement.kind = ts::OutcomeKind::UNKNOWN;
    unknown.settlement.settlement.evidence = ts::ExecutionEvidence::MAY_HAVE_EXECUTED;
    unknown.physical_trade->lifecycle_state = "UNKNOWN";
    {
        auto unit = storage.begin();
        ASSERT_EQ(unit.record_physical_settlement(unknown).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        auto unit = storage.begin();
        auto completion = logical_transition();
        completion.settlement = unknown.settlement;
        completion.physical_trade = unknown.physical_trade;
        EXPECT_EQ(unit.apply_group_completion_atomic(completion).kind, ts::StorageResultKind::INVALID_STATE);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    EXPECT_EQ(storage.settlement(900).value->settlement.kind, ts::OutcomeKind::UNKNOWN);
    EXPECT_EQ(storage.reservation(500).value->state, ts::RiskReservationState::RESERVED);
}

TEST(TradingSystemBatch7CA, FailedOpenCannotReplacePriorLegacyOrFinancialTerminal) {
    for (bool executed : {false, true}) {
        TestDatabase database;
        ts::TradingSystemStorage storage(database.config());
        seed(storage);
        auto original = physical_transition();
        original.settlement.settlement.evidence = executed ?
            ts::ExecutionEvidence::EXECUTED : ts::ExecutionEvidence::NOT_SUBMITTED;
        if (executed) {
            original.settlement.settlement.kind = ts::OutcomeKind::WIN;
            original.settlement.settlement.net_profit = money(100);
            original.settlement.native_profit = money(100);
            original.physical_trade->lifecycle_state = "WIN";
        }
        {
            auto unit = storage.begin();
            ASSERT_EQ(unit.record_physical_settlement(original).kind, ts::StorageResultKind::APPLIED);
            ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
        }
        auto unit = storage.begin();
        EXPECT_EQ(unit.record_physical_settlement(physical_transition(901)).kind, ts::StorageResultKind::CONFLICT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
        EXPECT_EQ(storage.settlements_for_group(80).values.size(), 1U);
    }
}

TEST(TradingSystemBatch7CA, NewReceiptIdCannotHideConflictingPhysicalFact) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    seed(storage);
    {
        auto unit = storage.begin();
        ASSERT_EQ(unit.record_physical_settlement(physical_transition()).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    auto conflicting = physical_transition(901);
    conflicting.physical_trade->broker_trade_id = "unexpected-broker-id";
    auto unit = storage.begin();
    EXPECT_EQ(unit.record_physical_settlement(conflicting).kind, ts::StorageResultKind::CONFLICT);
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    EXPECT_EQ(storage.settlements_for_group(80).values.size(), 1U);
}

TEST(TradingSystemBatch7CA, WriterRejectsUnknownEvidenceAndSchemaWithoutCommitting) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    {
        auto invalid = physical_transition().settlement;
        invalid.settlement.evidence = static_cast<ts::ExecutionEvidence>(99);
        auto unit = storage.begin();
        EXPECT_EQ(unit.put_settlement(invalid).kind, ts::StorageResultKind::INVALID_ARGUMENT);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    {
        auto invalid = physical_transition().settlement;
        invalid.schema_version = 99;
        auto unit = storage.begin();
        EXPECT_EQ(unit.put_settlement(invalid).kind, ts::StorageResultKind::UNSUPPORTED_SCHEMA);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    EXPECT_EQ(storage.settlement(900).kind, ts::StorageResultKind::NOT_FOUND);
}

TEST(TradingSystemBatch7CA, ExternalFirstFactInitializesLocalSequenceButLegacyStillRequiresRebuild) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    auto record = physical_transition().settlement;
    record.settlement.settlement_id = ts::external_settlement_namespace_bit + 1;
    {
        auto unit = storage.begin();
        ASSERT_EQ(unit.put_settlement(record).kind, ts::StorageResultKind::APPLIED);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    {
        auto unit = storage.begin();
        const auto allocated = unit.next_id("settlement");
        ASSERT_EQ(allocated.kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(allocated.value, 1U);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
    TestDatabase legacy_database;
    ts::TradingSystemStorage old(legacy_database.config());
    mdbxc::KeyValueTable<std::string, std::string> facts(old.connection(), "ts_settlements");
    facts.insert_or_assign(std::to_string(record.settlement.settlement_id),
                           ts::detail::settlement_record_to_json(record).dump());
    {
        auto unit = old.begin();
        EXPECT_FALSE(unit.next_id("settlement").kind == ts::StorageResultKind::APPLIED);
        EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    }
    {
        auto unit = old.begin();
        ASSERT_EQ(unit.rebuild_settlement_sequence().kind, ts::StorageResultKind::APPLIED);
        EXPECT_EQ(unit.next_id("settlement").value, 1U);
        ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    }
}

TEST(TradingSystemBatch7CA, NativeRejectedMemberDoesNotWaitForFx) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    seed(storage);
    auto value = physical_transition();
    value.logical_currency = CurrencyType::EUR;
    auto unit = storage.begin();
    EXPECT_EQ(unit.record_physical_settlement(value).kind, ts::StorageResultKind::APPLIED);
    ASSERT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    EXPECT_TRUE(storage.pending_valuations().values.empty());
    EXPECT_TRUE(storage.settlement(900).value->logical_outcome_ready);
}

TEST(TradingSystemBatch7CA, StoredUnknownVersionAndEvidenceFailClosed) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    mdbxc::KeyValueTable<std::string, std::string> facts(storage.connection(), "ts_settlements");
    auto wire = ts::detail::settlement_record_to_json(physical_transition().settlement);
    wire["settlement"]["evidence_version"] = 99;
    facts.insert_or_assign("900", wire.dump());
    EXPECT_EQ(storage.settlement(900).kind, ts::StorageResultKind::STORAGE_ERROR);
    wire["settlement"]["evidence_version"] = 2;
    wire["settlement"]["evidence"] = 99;
    facts.insert_or_assign("900", wire.dump());
    EXPECT_EQ(storage.settlement(900).kind, ts::StorageResultKind::STORAGE_ERROR);
    wire["schema_version"] = 99;
    facts.insert_or_assign("900", wire.dump());
    EXPECT_EQ(storage.settlement(900).kind, ts::StorageResultKind::UNSUPPORTED_SCHEMA);
}

TEST(TradingSystemBatch7CA, LegacyMdbxFactIsReadWithoutReinterpretationOrRewrite) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    auto record = physical_transition().settlement;
    record.settlement.evidence = ts::ExecutionEvidence::NOT_SUBMITTED;
    const auto wire = ts::detail::settlement_record_to_json(record).dump();
    mdbxc::KeyValueTable<std::string, std::string> facts(storage.connection(), "ts_settlements");
    facts.insert_or_assign("900", wire);
    ASSERT_TRUE(storage.settlement(900).found());
    EXPECT_EQ(storage.settlement(900).value->settlement.evidence, ts::ExecutionEvidence::NOT_SUBMITTED);
    auto unit = storage.begin();
    EXPECT_EQ(unit.put_settlement(record).kind, ts::StorageResultKind::DUPLICATE);
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::APPLIED);
    EXPECT_EQ(*facts.find("900"), wire);
    EXPECT_TRUE(legacy::decode(nlohmann::json::parse(wire).at("settlement")).valid());
}

TEST(TradingSystemBatch7CA, CheckErrorCreatesNeitherTerminalEvidenceNorRiskRelease) {
    TestDatabase database;
    ts::TradingSystemStorage storage(database.config());
    seed(storage);
    ts::ExecutionOperationRecord operation{"operation-700", "client-700", 41, 80, 700, 11, 1000};
    optionx::TradeRequest request;
    request.trade_id = 700;
    request.account_id = 11;
    request.account_type = optionx::AccountType::DEMO;
    request.currency = CurrencyType::USD;
    auto raw = request.create_trade_result_unique();
    raw->platform_type = optionx::PlatformType::INTRADE_BAR;
    raw->trade_state = optionx::TradeState::CHECK_ERROR;
    raw->error_code = optionx::TradeErrorCode::CLIENT_FORCED_CLOSE;
    raw->profit = 100;
    raw->live_state = optionx::TradeState::WIN;
    const auto mapped = pm::map_observation(*raw, request, operation,
        {11, "IntradeBar", "external-11", "DEMO", 11, 1000}, {CurrencyType::USD, 2});
    ASSERT_EQ(mapped.error, pm::MappingError::NONE);
    EXPECT_EQ(mapped.kind, pm::ObservationKind::RESULT_CHECK_FAILED);
    EXPECT_FALSE(mapped.proves_opening);
    EXPECT_FALSE(mapped.outcome);
    auto unit = storage.begin();
    EXPECT_EQ(unit.apply_group_completion_atomic(logical_transition()).kind, ts::StorageResultKind::NOT_FOUND);
    EXPECT_EQ(unit.commit().kind, ts::StorageResultKind::INVALID_STATE);
    EXPECT_TRUE(storage.settlements_for_group(80).values.empty());
    EXPECT_EQ(storage.reservation(500).value->state, ts::RiskReservationState::RESERVED);
}

TEST(TradingSystemBatch7CA, SimulatedReconciliationRetainsProofAcrossReopenWithoutResending) {
    TestDatabase database;
    auto broker = std::make_shared<ts::SimulatedExecutionAdapter::State>();
    const auto now = std::int64_t{9} * 3600 * 1000;
    {
        support::SyntheticHost host(database.config(), configuration(), now, broker);
        ASSERT_TRUE(host.accept(support::synthetic_buy()).succeeded());
        const auto group = host.series().current_group_id;
        ASSERT_TRUE(host.submit(group, ts::SimulatedSubmissionPlan::TIMEOUT).succeeded());
        const auto operation = host.operations(group).front();
        host.broker().set_proven_not_executed(operation.operation_id, true);
        ASSERT_TRUE(host.observe(group).succeeded());
        auto facts = host.storage().settlements_for_group(group);
        ASSERT_EQ(facts.values.size(), 1U);
        EXPECT_EQ(facts.values.front().settlement.evidence, ts::ExecutionEvidence::NOT_EXECUTED_PROVEN);
        ts::FakeExecutionClock clock(now);
        ts::ExecutionRecoveryManager manager(clock);
        EXPECT_EQ(manager.reconcile(host.storage(), operation, host.broker()).kind,
                  ts::ReconciliationKind::NOT_EXECUTED_PROVEN);
        EXPECT_EQ(host.storage().settlements_for_group(group).values.size(), 1U);
        ASSERT_TRUE(host.complete(group).succeeded());
        EXPECT_EQ(host.complete(group).kind, ts::StorageResultKind::DUPLICATE);
        EXPECT_EQ(host.series().series.next_step, 0U);
        EXPECT_EQ(host.mm_state().state_revision, 1U);
        EXPECT_EQ(host.broker().submission_count(operation.operation_id), 1U);
    }
    support::SyntheticHost reopened(database.config(), configuration(), now, broker);
    EXPECT_EQ(reopened.series().series.next_step, 0U);
    EXPECT_EQ(reopened.series().current_outcome.kind, ts::OutcomeKind::REJECTED);
    EXPECT_EQ(reopened.series().receipts.front().evidence, ts::ExecutionEvidence::NOT_EXECUTED_PROVEN);
    EXPECT_EQ(reopened.reserved(1000), 0);
    EXPECT_EQ(reopened.mm_state().state_revision, 1U);
}

TEST(TradingSystemBatch7CA, SimulatedPreSubmitRejectionKeepsLegacyEvidence) {
    TestDatabase database;
    auto broker = std::make_shared<ts::SimulatedExecutionAdapter::State>();
    support::SyntheticHost host(database.config(), configuration(), std::int64_t{9} * 3600 * 1000, broker);
    ASSERT_TRUE(host.accept(support::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    ASSERT_TRUE(host.submit(group, ts::SimulatedSubmissionPlan::PRE_SUBMIT_REJECTION).succeeded());
    const auto facts = host.storage().settlements_for_group(group);
    ASSERT_EQ(facts.values.size(), 1U);
    EXPECT_EQ(facts.values.front().settlement.evidence, ts::ExecutionEvidence::NOT_SUBMITTED);
    EXPECT_TRUE(legacy::decode(ts::detail::settlement_to_json(facts.values.front().settlement)).valid());
}

TEST(TradingSystemBatch7CA, MixedExecutedAndFailedOpenGroupSettlesOnlyExecutedContribution) {
    TestDatabase database;
    auto broker = std::make_shared<ts::SimulatedExecutionAdapter::State>();
    auto config = configuration();
    config.targets = {{11, 1000}, {12, 1001}};
    config.accounts[1001] = config.accounts[1000];
    support::SyntheticHost host(database.config(), config, std::int64_t{9} * 3600 * 1000, broker);
    ASSERT_TRUE(host.accept(support::synthetic_buy()).succeeded());
    const auto group = host.series().current_group_id;
    ASSERT_TRUE(host.submit_plans(group, {ts::SimulatedSubmissionPlan::SUCCESS,
                                         ts::SimulatedSubmissionPlan::TIMEOUT}).succeeded());
    const auto second = host.operations(group).back();
    host.broker().set_proven_not_executed(second.operation_id, true);
    host.broker_result(group, 0, 80);
    const auto observed = host.observe(group);
    ASSERT_TRUE(observed.succeeded()) << observed.message;
    ASSERT_TRUE(host.complete(group).succeeded());
    EXPECT_EQ(host.series().current_outcome.kind, ts::OutcomeKind::WIN);
    ASSERT_TRUE(host.series().current_outcome.net_profit);
    EXPECT_EQ(host.series().current_outcome.net_profit->units, 80);
    EXPECT_EQ(host.series().series.next_step, 1U);
    EXPECT_EQ(host.reserved(1000), 0);
    EXPECT_EQ(host.reserved(1001), 0);
    EXPECT_EQ(host.storage().settlements_for_group(group).values.size(), 2U);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
