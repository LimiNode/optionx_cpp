#include <gtest/gtest.h>

#include "optionx_cpp/bridges/telegram.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class FakeMessageSource final
    : public optionx::bridges::telegram::TelegramMessageSource {
public:
    bool start(message_callback_t on_message, error_callback_t on_error) override {
        (void)on_error;
        m_on_message = std::move(on_message);
        started = true;
        return true;
    }

    void stop() noexcept override {
        stopped = true;
        m_on_message = {};
    }

    void emit(optionx::bridges::telegram::TelegramRawMessage message) {
        if (m_on_message) {
            m_on_message(message);
        }
    }

    bool started = false;
    bool stopped = false;

private:
    message_callback_t m_on_message;
};

optionx::bridges::telegram::TelegramRawMessage make_message() {
    optionx::bridges::telegram::TelegramRawMessage message;
    message.chat_id = "-10042";
    message.chat_title = "Signals";
    message.message_id = 123;
    message.date_ms = 1800000000000;
    message.text = "EURUSD BUY 5m";
    return message;
}

std::int64_t current_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

optionx::bridges::telegram::TelegramRawMessage make_message(
        std::int64_t message_id,
        std::string text) {
    auto message = make_message();
    message.message_id = message_id;
    message.text = std::move(text);
    return message;
}

std::unique_ptr<optionx::bridges::telegram::TelegramSignalBridgeConfig> config() {
    auto value = std::make_unique<
        optionx::bridges::telegram::TelegramSignalBridgeConfig>();
    value->bridge_id = 17;
    value->fixed_amount = 1.0;
    return value;
}

} // namespace

TEST(TelegramSignalBridge, PublishesParsedSignalAndReportsDuplicate) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    ASSERT_TRUE(bridge.configure(config()));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::vector<optionx::BridgeSignalReport> reports;
    std::int64_t next_signal_id = 100;
    bridge.on_signal_id() = [&] { return ++next_signal_id; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        signals.push_back(std::move(signal));
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    ASSERT_TRUE(source->started);
    source->emit(make_message());
    source->emit(make_message());

    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals.front()->signal_id, 101);
    EXPECT_EQ(signals.front()->bridge_id, 17);
    EXPECT_EQ(signals.front()->symbol, "EURUSD");
    EXPECT_EQ(signals.front()->duration, 300u);
    EXPECT_EQ(signals.front()->source_time_ms, 1800000000000);
    EXPECT_EQ(signals.front()->to_trade_request().source_time_ms, 1800000000000);
    const auto serialized_signal = nlohmann::json(*signals.front());
    const auto restored_signal = serialized_signal.get<optionx::TradeSignal>();
    EXPECT_EQ(restored_signal.source_time_ms, 1800000000000);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.front().status,
              optionx::BridgeSignalReportStatus::DUPLICATE);
    EXPECT_EQ(reports.front().reason_code, "duplicate_message");

    bridge.shutdown();
    EXPECT_TRUE(source->stopped);
}

TEST(TelegramSignalBridge, RejectsStaleSignalWithSourceTimestamp) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->max_signal_age_seconds = 1;
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::vector<optionx::BridgeSignalReport> reports;
    bridge.on_signal_id() = [] { return 101; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        signals.push_back(std::move(signal));
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    auto message = make_message();
    message.date_ms = current_time_ms() - 2000;
    source->emit(message);

    EXPECT_TRUE(signals.empty());
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.front().status, optionx::BridgeSignalReportStatus::REJECTED);
    EXPECT_EQ(reports.front().reason_code, "stale_signal");
    EXPECT_EQ(reports.front().source_time_ms, message.date_ms);
    EXPECT_GT(reports.front().received_time_ms, message.date_ms);

    bridge.shutdown();
}

TEST(TelegramSignalBridge, FirstSignalOnlyFiltersExplicitMartingaleSteps) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::FIRST_SIGNAL_ONLY;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::vector<optionx::BridgeSignalReport> reports;
    bridge.on_signal_id() = [] { return 101; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        signals.push_back(std::move(signal));
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-1"));

    EXPECT_TRUE(signals.empty());
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.front().status, optionx::BridgeSignalReportStatus::IGNORED);
    EXPECT_EQ(reports.front().reason_code, "martingale_step_filtered");

    bridge.shutdown();
}

TEST(TelegramSignalBridge, RequiresContiguousExplicitMartingaleSteps) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::vector<optionx::BridgeSignalReport> reports;
    std::int64_t next_signal_id = 100;
    bridge.on_signal_id() = [&] { return ++next_signal_id; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        signals.push_back(std::move(signal));
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));
    source->emit(make_message(125, "EURUSD BUY 5m COBRA MG-2"));
    source->emit(make_message(126, "EURUSD BUY 5m COBRA MG-1"));
    source->emit(make_message(127, "EURUSD BUY 5m COBRA MG-2"));
    source->emit(make_message(128, "EURUSD BUY 5m COBRA"));

    ASSERT_EQ(signals.size(), 3u);
    EXPECT_EQ(signals[0]->mm_step, 0);
    EXPECT_EQ(signals[1]->mm_step, 1);
    EXPECT_EQ(signals[2]->mm_step, 2);
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[0].reason_code, "martingale_step_out_of_sequence");
    EXPECT_EQ(reports[1].reason_code, "martingale_step_missing");

    bridge.shutdown();
}

TEST(TelegramSignalBridge, RequiresFreshInitialContiguousMartingaleStep) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->max_signal_age_seconds = 1;
    bridge_config->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::vector<optionx::BridgeSignalReport> reports;
    bridge.on_signal_id() = [] { return 101; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        signals.push_back(std::move(signal));
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    auto stale_initial = make_message(124, "EURUSD BUY 5m COBRA MG-0");
    stale_initial.date_ms = current_time_ms() - 2000;
    source->emit(stale_initial);

    auto fresh_initial = make_message(125, "EURUSD BUY 5m COBRA MG-0");
    fresh_initial.date_ms = current_time_ms();
    source->emit(fresh_initial);

    auto delayed_next = make_message(126, "EURUSD BUY 5m COBRA MG-1");
    delayed_next.date_ms = current_time_ms() - 2000;
    source->emit(delayed_next);

    ASSERT_EQ(signals.size(), 2u);
    EXPECT_EQ(signals[0]->mm_step, 0);
    EXPECT_EQ(signals[1]->mm_step, 1);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.front().reason_code, "stale_signal");

    bridge.shutdown();
}

TEST(TelegramSignalBridge, SerializesConcurrentContiguousMartingaleDispatch) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::promise<void> first_callback_started;
    const auto release_first_callback = std::make_shared<std::promise<void>>();
    const auto release_future = release_first_callback->get_future().share();
    std::atomic<int> later_steps_dispatched{0};
    bridge.on_signal_id() = [] { return 101; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        if (signal->mm_step == 0) {
            first_callback_started.set_value();
            release_future.wait();
            throw std::runtime_error("expected test failure");
        }
        ++later_steps_dispatched;
    };

    bridge.run();
    std::thread first([&] {
        source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));
    });
    first_callback_started.get_future().wait();
    std::thread second([&] {
        source->emit(make_message(125, "EURUSD BUY 5m COBRA MG-1"));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(later_steps_dispatched.load(), 0);
    release_first_callback->set_value();
    first.join();
    second.join();
    EXPECT_EQ(later_steps_dispatched.load(), 0);

    bridge.shutdown();
}

TEST(TelegramSignalBridge, RejectsReentrantMartingaleStepUntilPredecessorCommits) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::vector<optionx::BridgeSignalReport> reports;
    bridge.on_signal_id() = [] { return 101; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        if (signal->mm_step == 0) {
            source->emit(make_message(125, "EURUSD BUY 5m COBRA MG-1"));
        }
        signals.push_back(std::move(signal));
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));

    ASSERT_EQ(signals.size(), 1u);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.front().reason_code, "martingale_step_pending");

    source->emit(make_message(126, "EURUSD BUY 5m COBRA MG-1"));
    ASSERT_EQ(signals.size(), 2u);
    EXPECT_EQ(signals.back()->mm_step, 1);

    bridge.shutdown();
}

TEST(TelegramSignalBridge, RollsBackMartingaleStepWhenSignalIdAllocationFails) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::vector<optionx::BridgeSignalReport> reports;
    bool fail_first_allocation = true;
    bridge.on_signal_id() = [&] {
        if (fail_first_allocation) {
            fail_first_allocation = false;
            throw std::runtime_error("expected test failure");
        }
        return 101;
    };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        signals.push_back(std::move(signal));
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));
    source->emit(make_message(125, "EURUSD BUY 5m COBRA MG-0"));
    source->emit(make_message(126, "EURUSD BUY 5m COBRA MG-1"));

    ASSERT_EQ(signals.size(), 2u);
    EXPECT_EQ(signals[0]->mm_step, 0);
    EXPECT_EQ(signals[1]->mm_step, 1);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.front().reason_code, "signal_id_allocation_failed");

    bridge.shutdown();
}

TEST(TelegramSignalBridge, AntiMartingaleUsesConfirmedBrokerResults) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->anti_martingale_enabled = true;
    bridge_config->anti_martingale_multiplier = 2.0;
    bridge_config->anti_martingale_max_steps = 2;
    bridge_config->anti_martingale_max_amount = 4.0;
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::vector<optionx::BridgeSignalReport> reports;
    std::int64_t next_signal_id = 100;
    bridge.on_signal_id() = [&] { return ++next_signal_id; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        signals.push_back(std::move(signal));
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA"));
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0]->amount, 1.0);
    EXPECT_EQ(signals[0]->mm_type, optionx::MmSystemType::ANTI_MARTINGALE_SIGNAL);
    EXPECT_EQ(signals[0]->mm_step, 0);

    source->emit(make_message(125, "EURUSD BUY 5m COBRA"));
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.back().reason_code, "anti_martingale_pending_result");

    auto first_request = signals[0]->to_trade_request();
    optionx::TradeResult first_result;
    first_result.trade_state = optionx::TradeState::OPEN_SUCCESS;
    bridge.update_trade_result(first_request, first_result);
    source->emit(make_message(126, "EURUSD BUY 5m COBRA"));
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports.back().reason_code, "anti_martingale_pending_result");

    first_result.trade_state = optionx::TradeState::WIN;
    bridge.update_trade_result(first_request, first_result);
    source->emit(make_message(127, "EURUSD BUY 5m COBRA"));
    ASSERT_EQ(signals.size(), 2u);
    EXPECT_EQ(signals[1]->amount, 2.0);
    EXPECT_EQ(signals[1]->mm_step, 1);

    bridge.update_trade_result(first_request, first_result);
    auto second_request = signals[1]->to_trade_request();
    optionx::TradeResult second_result;
    second_result.trade_state = optionx::TradeState::WIN;
    bridge.update_trade_result(second_request, second_result);
    source->emit(make_message(128, "EURUSD BUY 5m COBRA"));
    ASSERT_EQ(signals.size(), 3u);
    EXPECT_EQ(signals[2]->amount, 4.0);
    EXPECT_EQ(signals[2]->mm_step, 2);

    auto third_request = signals[2]->to_trade_request();
    optionx::TradeResult third_result;
    third_result.trade_state = optionx::TradeState::WIN;
    bridge.update_trade_result(third_request, third_result);
    source->emit(make_message(129, "EURUSD BUY 5m COBRA"));
    ASSERT_EQ(signals.size(), 4u);
    EXPECT_EQ(signals[3]->amount, 1.0);
    EXPECT_EQ(signals[3]->mm_step, 0);

    auto fourth_request = signals[3]->to_trade_request();
    optionx::TradeResult fourth_result;
    fourth_result.trade_state = optionx::TradeState::LOSS;
    bridge.update_trade_result(fourth_request, fourth_result);
    source->emit(make_message(130, "EURUSD BUY 5m COBRA"));
    ASSERT_EQ(signals.size(), 5u);
    EXPECT_EQ(signals[4]->amount, 1.0);
    EXPECT_EQ(signals[4]->mm_step, 0);

    bridge.shutdown();
}

TEST(TelegramSignalBridge, AntiMartingaleHandlesReentrantTerminalResultDuringDispatch) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->anti_martingale_enabled = true;
    bridge_config->anti_martingale_max_amount = 2.0;
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::int64_t next_signal_id = 100;
    bridge.on_signal_id() = [&] { return ++next_signal_id; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        const auto request = signal->to_trade_request();
        optionx::TradeResult result;
        result.trade_state = optionx::TradeState::WIN;
        bridge.update_trade_result(request, result);
        signals.push_back(std::move(signal));
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA"));
    source->emit(make_message(125, "EURUSD BUY 5m COBRA"));

    ASSERT_EQ(signals.size(), 2u);
    EXPECT_EQ(signals[0]->amount, 1.0);
    EXPECT_EQ(signals[0]->mm_step, 0);
    EXPECT_EQ(signals[1]->amount, 2.0);
    EXPECT_EQ(signals[1]->mm_step, 1);

    bridge.shutdown();
}

TEST(TelegramSignalBridge, KeepsAntiMartingaleStateFailClosedWhenCallbackThrows) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->anti_martingale_enabled = true;
    bridge_config->anti_martingale_max_amount = 2.0;
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::vector<optionx::BridgeSignalReport> reports;
    bool fail_first_callback = true;
    bridge.on_signal_id() = [] { return 101; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        if (fail_first_callback) {
            fail_first_callback = false;
            throw std::runtime_error("expected test failure");
        }
        signals.push_back(std::move(signal));
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA"));
    source->emit(make_message(124, "EURUSD BUY 5m COBRA"));
    source->emit(make_message(125, "EURUSD BUY 5m COBRA"));

    EXPECT_TRUE(signals.empty());
    ASSERT_EQ(reports.size(), 3u);
    EXPECT_EQ(reports[0].reason_code, "ambiguous_dispatch_failure");
    EXPECT_EQ(reports[1].reason_code, "duplicate_message");
    EXPECT_EQ(reports[2].reason_code, "anti_martingale_pending_result");

    bridge.shutdown();
}

TEST(TelegramSignalBridge, KeepsMartingaleReservationFailClosedWhenCallbackThrows) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::vector<optionx::BridgeSignalReport> reports;
    bridge.on_signal_id() = [] { return 101; };
    bridge.on_trade_signal() = [](std::unique_ptr<optionx::TradeSignal>) {
        throw std::runtime_error("expected test failure");
    };
    bridge.on_signal_report() = [&](const auto& report) {
        reports.push_back(report);
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));
    source->emit(make_message(125, "EURUSD BUY 5m COBRA MG-1"));

    ASSERT_EQ(reports.size(), 3u);
    EXPECT_EQ(reports[0].reason_code, "ambiguous_dispatch_failure");
    EXPECT_EQ(reports[1].reason_code, "duplicate_message");
    EXPECT_EQ(reports[2].reason_code, "martingale_step_pending");

    bridge.shutdown();
}

TEST(TelegramSignalBridge, ReportsMissingReplyCorrelatedSourceStep) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    bridge_config->source_chain_rules = {
        {"cobra", "COBRA", optionx::bridges::telegram::TelegramOutcomeResult::LOSS,
         2, 1, optionx::bridges::telegram::TelegramSourceChainAction::REPORT_ONLY},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::promise<optionx::BridgeSignalReport> missing_step;
    bridge.on_signal_id() = [] { return 101; };
    bridge.on_trade_signal() = [](std::unique_ptr<optionx::TradeSignal>) {};
    bridge.on_signal_report() = [&](const auto& report) {
        if (report.reason_code == "expected_source_step_missing") {
            missing_step.set_value(report);
        }
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));
    auto outcome = make_message(125, "EURUSD LOSS");
    outcome.reply_to_message_id = 124;
    source->emit(outcome);

    auto future = missing_step.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    const auto report = future.get();
    EXPECT_EQ(report.status, optionx::BridgeSignalReportStatus::SUSPICIOUS);
    EXPECT_EQ(report.context.at("expected_source_step"), 1);
    EXPECT_EQ(report.context.at("timeout_seconds"), 1);

    bridge.shutdown();
}

TEST(TelegramSignalBridge, EmitsAssumedContiguousSourceStepAfterTimeout) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    bridge_config->source_chain_rules = {
        {"cobra", "COBRA", optionx::bridges::telegram::TelegramOutcomeResult::LOSS,
         2, 1, optionx::bridges::telegram::TelegramSourceChainAction::EMIT_ASSUMED_SIGNAL},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::mutex signals_mutex;
    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::promise<void> assumed_signal_ready;
    std::int64_t next_signal_id = 100;
    bridge.on_signal_id() = [&] { return ++next_signal_id; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        const auto assumed = signal->is_assumed;
        {
            std::lock_guard<std::mutex> lock(signals_mutex);
            signals.push_back(std::move(signal));
        }
        if (assumed) {
            assumed_signal_ready.set_value();
        }
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));
    auto outcome = make_message(125, "EURUSD LOSS");
    outcome.reply_to_message_id = 124;
    source->emit(outcome);

    const auto future = assumed_signal_ready.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    std::lock_guard<std::mutex> lock(signals_mutex);
    ASSERT_EQ(signals.size(), 2u);
    EXPECT_TRUE(signals[1]->is_assumed);
    EXPECT_EQ(signals[1]->assumed_reason, "expected_source_step_missing");
    EXPECT_EQ(signals[1]->assumed_source_identity, "telegram:-10042:0:124");
    EXPECT_EQ(signals[1]->assumed_source_step, 1);
    EXPECT_EQ(signals[1]->mm_step, 1);
    const auto assumed_request = signals[1]->to_trade_request();
    EXPECT_TRUE(assumed_request.is_assumed);
    EXPECT_EQ(assumed_request.assumed_source_identity, "telegram:-10042:0:124");
    EXPECT_EQ(assumed_request.assumed_source_step, 1);

    bridge.shutdown();
}

TEST(TelegramSignalBridge, WatchdogCallbackShutdownJoinsBeforeStoppedAndRestart) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    bridge_config->source_chain_rules = {
        {"cobra", "COBRA", optionx::bridges::telegram::TelegramOutcomeResult::LOSS,
         2, 1, optionx::bridges::telegram::TelegramSourceChainAction::EMIT_ASSUMED_SIGNAL},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::atomic<optionx::SignalId> next_signal_id{100};
    std::atomic<int> started_count{0};
    std::atomic<int> stopped_count{0};
    std::atomic<bool> callback_active{false};
    std::atomic<bool> stopped_during_callback{false};
    std::promise<void> self_shutdown_ready;
    auto self_shutdown_future = self_shutdown_ready.get_future();
    auto release_callback = std::make_shared<std::promise<void>>();
    auto callback_released = release_callback->get_future().share();
    std::promise<void> stopped_ready;
    auto stopped_future = stopped_ready.get_future();
    bridge.on_signal_id() = [&] { return next_signal_id.fetch_add(1); };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        if (!signal->is_assumed) {
            return;
        }
        callback_active.store(true);
        bridge.shutdown();
        self_shutdown_ready.set_value();
        callback_released.wait();
        if (stopped_count.load() != 0) {
            stopped_during_callback.store(true);
        }
        callback_active.store(false);
    };
    bridge.on_status_update() = [&](const optionx::BridgeStatusUpdate& update) {
        if (update.status == optionx::BridgeStatus::SERVER_STARTED) {
            started_count.fetch_add(1);
        }
        if (update.status == optionx::BridgeStatus::SERVER_STOPPED) {
            if (callback_active.load()) {
                stopped_during_callback.store(true);
            }
            if (stopped_count.fetch_add(1) == 0) {
                stopped_ready.set_value();
            }
        }
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));
    auto outcome = make_message(125, "EURUSD LOSS");
    outcome.reply_to_message_id = 124;
    source->emit(outcome);

    ASSERT_EQ(self_shutdown_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    bridge.shutdown();

    auto restart = std::async(std::launch::async, [&bridge] {
        bridge.run();
    });
    EXPECT_EQ(restart.wait_for(std::chrono::milliseconds(100)),
              std::future_status::timeout);
    EXPECT_EQ(started_count.load(), 1);

    release_callback->set_value();
    ASSERT_EQ(stopped_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    ASSERT_EQ(restart.wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    restart.get();
    EXPECT_FALSE(stopped_during_callback.load());

    for (int attempt = 0; attempt < 100 && started_count.load() < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(started_count.load(), 2);
    bridge.shutdown();
}

TEST(TelegramSignalBridge, IgnoresLateSourceStepAfterAssumedContinuation) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    bridge_config->source_chain_rules = {
        {"cobra", "COBRA", optionx::bridges::telegram::TelegramOutcomeResult::LOSS,
         2, 1, optionx::bridges::telegram::TelegramSourceChainAction::EMIT_ASSUMED_SIGNAL},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::mutex signals_mutex;
    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::promise<void> assumed_signal_ready;
    std::promise<optionx::BridgeSignalReport> ignored_step;
    bridge.on_signal_id() = [] { return 101; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        const auto assumed = signal->is_assumed;
        {
            std::lock_guard<std::mutex> lock(signals_mutex);
            signals.push_back(std::move(signal));
        }
        if (assumed) {
            assumed_signal_ready.set_value();
        }
    };
    bridge.on_signal_report() = [&](const auto& report) {
        if (report.reason_code == "source_chain_assumed_step") {
            ignored_step.set_value(report);
        }
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));
    auto outcome = make_message(125, "EURUSD LOSS");
    outcome.reply_to_message_id = 124;
    source->emit(outcome);

    ASSERT_EQ(assumed_signal_ready.get_future().wait_for(std::chrono::seconds(3)),
              std::future_status::ready);
    source->emit(make_message(126, "EURUSD BUY 5m COBRA MG-1"));
    auto future = ignored_step.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get().status, optionx::BridgeSignalReportStatus::IGNORED);
    std::lock_guard<std::mutex> lock(signals_mutex);
    EXPECT_EQ(signals.size(), 2u);

    bridge.shutdown();
}

TEST(TelegramSignalBridge, WakesForAnEarlierSourceChainDeadline) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    bridge_config->source_chain_rules = {
        {"alpha", "ALPHA", optionx::bridges::telegram::TelegramOutcomeResult::LOSS,
         2, 3, optionx::bridges::telegram::TelegramSourceChainAction::REPORT_ONLY},
        {"beta", "BETA", optionx::bridges::telegram::TelegramOutcomeResult::LOSS,
         2, 1, optionx::bridges::telegram::TelegramSourceChainAction::REPORT_ONLY},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::promise<optionx::BridgeSignalReport> beta_missing_step;
    bridge.on_signal_id() = [] { return 101; };
    bridge.on_trade_signal() = [](std::unique_ptr<optionx::TradeSignal>) {};
    bridge.on_signal_report() = [&](const auto& report) {
        if (report.reason_code == "expected_source_step_missing" &&
            report.signal_name == "BETA") {
            beta_missing_step.set_value(report);
        }
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m ALPHA MG-0"));
    auto alpha_outcome = make_message(125, "EURUSD LOSS");
    alpha_outcome.reply_to_message_id = 124;
    source->emit(alpha_outcome);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    source->emit(make_message(126, "EURUSD BUY 5m BETA MG-0"));
    auto beta_outcome = make_message(127, "EURUSD LOSS");
    beta_outcome.reply_to_message_id = 126;
    source->emit(beta_outcome);

    auto future = beta_missing_step.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(future.get().signal_name, "BETA");

    bridge.shutdown();
}

TEST(TelegramSignalBridge, EmitsAssumedSourceStepWithLocalAntiMartingaleSizing) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto bridge_config = config();
    bridge_config->anti_martingale_enabled = true;
    bridge_config->anti_martingale_max_amount = 2.0;
    bridge_config->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    bridge_config->source_chain_rules = {
        {"cobra", "COBRA", optionx::bridges::telegram::TelegramOutcomeResult::LOSS,
         2, 1, optionx::bridges::telegram::TelegramSourceChainAction::EMIT_ASSUMED_SIGNAL},
    };
    ASSERT_TRUE(bridge.configure(std::move(bridge_config)));

    std::mutex signals_mutex;
    std::vector<std::unique_ptr<optionx::TradeSignal>> signals;
    std::promise<void> assumed_signal_ready;
    std::promise<optionx::BridgeSignalReport> ignored_late_source_step;
    std::int64_t next_signal_id = 100;
    bridge.on_signal_id() = [&] { return ++next_signal_id; };
    bridge.on_trade_signal() = [&](std::unique_ptr<optionx::TradeSignal> signal) {
        const auto assumed = signal->is_assumed;
        {
            std::lock_guard<std::mutex> lock(signals_mutex);
            signals.push_back(std::move(signal));
        }
        if (assumed) {
            assumed_signal_ready.set_value();
        }
    };
    bridge.on_signal_report() = [&](const auto& report) {
        if (report.reason_code == "source_chain_assumed_step") {
            ignored_late_source_step.set_value(report);
        }
    };

    bridge.run();
    source->emit(make_message(124, "EURUSD BUY 5m COBRA MG-0"));
    {
        std::lock_guard<std::mutex> lock(signals_mutex);
        ASSERT_EQ(signals.size(), 1u);
        auto request = signals.front()->to_trade_request();
        optionx::TradeResult result;
        result.trade_state = optionx::TradeState::WIN;
        bridge.update_trade_result(request, result);
    }
    auto outcome = make_message(125, "EURUSD LOSS");
    outcome.reply_to_message_id = 124;
    source->emit(outcome);

    auto future = assumed_signal_ready.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    optionx::TradeRequest assumed_request;
    {
        std::lock_guard<std::mutex> lock(signals_mutex);
        ASSERT_EQ(signals.size(), 2u);
        EXPECT_TRUE(signals[1]->is_assumed);
        EXPECT_EQ(signals[1]->assumed_source_step, 1);
        EXPECT_EQ(signals[1]->mm_type, optionx::MmSystemType::ANTI_MARTINGALE_SIGNAL);
        EXPECT_EQ(signals[1]->mm_step, 1);
        EXPECT_EQ(signals[1]->amount, 2.0);
        assumed_request = signals[1]->to_trade_request();
    }
    optionx::TradeResult assumed_result;
    assumed_result.trade_state = optionx::TradeState::LOSS;
    bridge.update_trade_result(assumed_request, assumed_result);
    source->emit(make_message(126, "EURUSD BUY 5m COBRA MG-1"));
    auto ignored_future = ignored_late_source_step.get_future();
    ASSERT_EQ(ignored_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(ignored_future.get().status, optionx::BridgeSignalReportStatus::IGNORED);
    {
        std::lock_guard<std::mutex> lock(signals_mutex);
        EXPECT_EQ(signals.size(), 2u);
    }

    bridge.shutdown();
}

TEST(TelegramSignalBridge, RejectsInvalidConfigurationBeforeStartingSource) {
    auto source = std::make_shared<FakeMessageSource>();
    optionx::bridges::telegram::TelegramSignalBridge bridge(source);
    auto invalid = config();
    invalid->fixed_amount = 0.0;

    EXPECT_FALSE(bridge.configure(std::move(invalid)));
    bridge.run();
    EXPECT_FALSE(source->started);
}

TEST(TelegramSignalBridgeConfig, RoundTripsParserRecognitionSettings) {
    auto original = config();
    nlohmann::json serialized;
    original->to_json(serialized);

    optionx::bridges::telegram::TelegramSignalBridgeConfig restored;
    restored.from_json(serialized);

    EXPECT_EQ(restored.parser.symbol_pattern, original->parser.symbol_pattern);
    EXPECT_EQ(restored.parser.otc_symbol_suffix, original->parser.otc_symbol_suffix);
    EXPECT_EQ(restored.parser.expiry_policy.mode,
              original->parser.expiry_policy.mode);
    EXPECT_EQ(restored.parser.expiry_policy.custom_duration_seconds,
              original->parser.expiry_policy.custom_duration_seconds);
    ASSERT_EQ(restored.parser.direction_rules.size(),
              original->parser.direction_rules.size());
    ASSERT_EQ(restored.parser.outcome_rules.size(),
              original->parser.outcome_rules.size());
    EXPECT_EQ(restored.parser.direction_rules.front().pattern,
              original->parser.direction_rules.front().pattern);
    EXPECT_EQ(restored.parser.outcome_rules.back().fixed_result,
              optionx::bridges::telegram::TelegramOutcomeResult::LOSS);
    EXPECT_EQ(restored.parser.outcome_rules.back().direction_group,
              original->parser.outcome_rules.back().direction_group);
}

TEST(TelegramSignalBridgeConfig, RoundTripsTimingAndMartingaleSettings) {
    auto original = config();
    original->max_signal_age_seconds = 5;
    original->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    original->parser.martingale_rules = {
        {"explicit-mg", R"(\bMG[ -]?(\d+)\b)", 1},
    };
    nlohmann::json serialized;
    original->to_json(serialized);

    optionx::bridges::telegram::TelegramSignalBridgeConfig restored;
    restored.from_json(serialized);

    EXPECT_EQ(restored.max_signal_age_seconds, 5u);
    EXPECT_EQ(restored.martingale_policy,
              optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS);
    ASSERT_EQ(restored.parser.martingale_rules.size(), 1u);
    EXPECT_EQ(restored.parser.martingale_rules[0].pattern, R"(\bMG[ -]?(\d+)\b)");
    EXPECT_EQ(restored.parser.martingale_rules[0].step_group, 1u);
}

TEST(TelegramSignalBridgeConfig, RoundTripsAntiMartingaleSettings) {
    auto original = config();
    original->anti_martingale_enabled = true;
    original->anti_martingale_multiplier = 1.5;
    original->anti_martingale_max_steps = 3;
    original->anti_martingale_max_amount = 12.0;
    nlohmann::json serialized;
    original->to_json(serialized);

    optionx::bridges::telegram::TelegramSignalBridgeConfig restored;
    restored.from_json(serialized);

    EXPECT_TRUE(restored.anti_martingale_enabled);
    EXPECT_EQ(restored.anti_martingale_multiplier, 1.5);
    EXPECT_EQ(restored.anti_martingale_max_steps, 3u);
    EXPECT_EQ(restored.anti_martingale_max_amount, 12.0);
    EXPECT_TRUE(restored.validate().first);
}

TEST(TelegramSignalBridgeConfig, RoundTripsSourceChainRules) {
    auto original = config();
    original->source_chain_rules = {
        {"cobra", "COBRA", optionx::bridges::telegram::TelegramOutcomeResult::LOSS,
         7, 15, optionx::bridges::telegram::TelegramSourceChainAction::REPORT_ONLY},
    };
    nlohmann::json serialized;
    original->to_json(serialized);

    optionx::bridges::telegram::TelegramSignalBridgeConfig restored;
    restored.from_json(serialized);

    ASSERT_EQ(restored.source_chain_rules.size(), 1u);
    EXPECT_EQ(restored.source_chain_rules[0].name, "cobra");
    EXPECT_EQ(restored.source_chain_rules[0].signal_name, "COBRA");
    EXPECT_EQ(restored.source_chain_rules[0].max_step, 7u);
    EXPECT_EQ(restored.source_chain_rules[0].timeout_seconds, 15u);
    EXPECT_TRUE(restored.validate().first);
}

TEST(TelegramSignalBridgeConfig, RejectsInvalidAntiMartingaleSettings) {
    auto invalid = config();
    invalid->anti_martingale_enabled = true;
    EXPECT_FALSE(invalid->validate().first);

    invalid->anti_martingale_max_amount = 2.0;
    invalid->martingale_policy =
        optionx::bridges::telegram::TelegramMartingalePolicy::CONTIGUOUS_STEPS;
    const auto validation = invalid->validate();
    EXPECT_FALSE(validation.first);
    EXPECT_EQ(validation.second,
              "Telegram anti-martingale cannot use CONTIGUOUS_STEPS martingale_policy.");
}

TEST(TelegramSignalBridgeConfig, RejectsUnknownMartingalePolicy) {
    auto original = config();
    nlohmann::json serialized;
    original->to_json(serialized);
    serialized["martingale_policy"] = "FIRST_SIGNAL_ONY";

    optionx::bridges::telegram::TelegramSignalBridgeConfig restored;
    restored.from_json(serialized);

    EXPECT_EQ(restored.martingale_policy,
              optionx::bridges::telegram::TelegramMartingalePolicy::UNKNOWN);
    const auto validation = restored.validate();
    EXPECT_FALSE(validation.first);
    EXPECT_EQ(validation.second, "Telegram martingale_policy is unsupported.");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
