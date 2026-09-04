#define SQLITE_THREADSAFE 1
//#define LOGIT_BASE_PATH "E:\\_repoz\\optionx_cpp"
#define OPTIONX_LOG_UNIQUE_FILE_INDEX 2

#ifndef OPTIONX_TIMESTAMP_MS
#define OPTIONX_TIMESTAMP_MS time_shield::timestamp_ms()
#endif

#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>
#include "optionx_cpp/components.hpp" // Main header including all components
#include "optionx_cpp/platforms.hpp"

namespace optionx::components {

    /// \class AuthData
    /// \brief Represents authorization data for the Intrade Bar platform.
    class AuthData : public IAuthData {
    public:

        AuthData() {}

        virtual ~AuthData() = default;

        void to_json(nlohmann::json& j) const override {};

        void from_json(const nlohmann::json &j) override {};

        std::pair<bool, std::string> validate() const override {
            return { true, std::string() };
        }

        /// \brief Clones the authorization data instance to a unique pointer.
        /// \return Unique pointer to a cloned IAuthData instance.
        std::unique_ptr<IAuthData> clone_unique() const override {
            return std::make_unique<AuthData>(*this);
        }

        /// \brief Clones the authorization data instance to a shared pointer.
        /// \return Shared pointer to a cloned IAuthData instance.
        std::shared_ptr<IAuthData> clone_shared() const override {
            return std::make_shared<AuthData>(*this);
        }

        /// \brief Retrieves the API type associated with this authorization data.
        /// \return The type of API used.
        PlatformType platform_type() const override {
            return PlatformType::SIMULATOR;
        }

        AccountType account_type = AccountType::DEMO;
        CurrencyType currency    = CurrencyType::USD;
    }; // AuthData

        /// \class AccountInfoData
    /// \brief Account information data for Intrade Bar platform.
    class AccountInfoData : public BaseAccountInfoData {
    public:
        // Public member variables
        int64_t         user_id         = 0;                        ///< User ID
        double          balance         = 0.0;                      ///< Account balance
        CurrencyType    currency        = CurrencyType::UNKNOWN;    ///< Account currency
        AccountType     account_type    = AccountType::UNKNOWN;     ///< Account type (DEMO or REAL)
        bool            connect         = false;                    ///< Connection status
        int64_t         open_trades     = 0;                        ///< Number of open trades for the account

        // --- Limits ----------------------------------------------------------

        double min_usd_amount           = 1.0;                      ///< Minimum trade amount for USD accounts
        double max_usd_amount           = 700.0;                    ///< Maximum trade amount for USD accounts
        double max_usd_limit_amount     = 50.0;                     ///< Maximum trade amount during restricted periods for USD
        double high_payout_usd_amount   = 80.0;                     ///< Trade amount threshold for increased payout percentage in USD
        double min_rub_amount           = 100.0;                    ///< Minimum trade amount for RUB accounts
        double max_rub_amount           = 50000.0;                  ///< Maximum trade amount for RUB accounts
        double max_rub_limit_amount     = 3500.0;                   ///< Maximum trade amount during restricted periods for RUB
        double high_payout_rub_amount   = 5000.0;                   ///< Trade amount threshold for increased payout percentage in RUB

        int64_t min_duration            = 5;                        ///< Minimum binary option duration (in seconds)
        int64_t min_btc_duration        = 5;                        ///< Minimum duration for BTCUSDT (in seconds)
        int64_t max_duration            = 30000;                    ///< Maximum duration (in seconds)
        int64_t max_trades              = 5;                        ///< Maximum number of trades
        int64_t max_limit_trades        = 2;                        ///< Maximum number of trades during restricted periods
        int64_t order_queue_timeout     = 10;                       ///< Timeout for pending orders in the queue
        int64_t responce_timeout        = 10;                       ///< Timeout for server response related to opening or closing a trade
        int64_t order_interval_ms       = 1000;                     ///< Minimum time interval required between consecutive orders

        // Trading time for BTCUSDT
        int64_t start_btc_time          = 0;                        ///< Start of trading time for BTCUSDT
        int64_t end_btc_time            = 86400;                    ///< End of trading time for BTCUSDT
        // Trading time for standard pairs
        int64_t start_time              = 0;                        ///< Start of trading time for standard pairs
        int64_t end_time                = 84600;                    ///< End of trading time for standard pairs

        /// \brief Retrieves the API type associated with this account data.
        /// \return The type of API used.
        const PlatformType platform_type() const override final {
            return PlatformType::SIMULATOR;
        }

        /// \brief Creates a unique pointer to a clone of this account info data instance.
        /// \return Unique pointer to a cloned BaseAccountInfoData instance.
        std::unique_ptr<BaseAccountInfoData> clone_unique() const override final {
            return std::make_unique<AccountInfoData>(*this);
        }

        /// \brief Creates a shared pointer to a clone of this account info data instance.
        /// \return Shared pointer to a cloned BaseAccountInfoData instance.
        std::shared_ptr<BaseAccountInfoData> clone_shared() const override final {
            return std::make_shared<AccountInfoData>(*this);
        }

    protected:

        /// \brief Retrieves a boolean account information based on the request type.
        /// \param request Specifies the type of account information requested.
        /// \return Boolean account information.
        bool get_info_bool(const AccountInfoRequest& request) const override final {
            switch (request.type) {
            case AccountInfoType::CONNECTION_STATUS:
                return connect;
            case AccountInfoType::SYMBOL_AVAILABILITY: {
                static const std::set<std::string> symbols = {
                    "AUDCAD","AUDCHF","AUDJPY",
                    "AUDNZD","AUDUSD","CADJPY",
                    "EURAUD","EURCAD","EURCHF",
                    "EURGBP","EURJPY","EURUSD",
                    "GBPAUD","GBPCHF","GBPJPY",
                    "GBPNZD","NZDJPY","NZDUSD",
                    "USDCAD","USDCHF","USDJPY",
                    "BTCUSDT"
                };
                return (symbols.find(request.symbol) != symbols.end());
            }
            case AccountInfoType::OPTION_TYPE_AVAILABILITY:
                if (request.option_type == OptionType::CLASSIC &&
                    (request.symbol == "BTCUSDT" || request.symbol == "BTCUSD")) return false;
                return (request.option_type == OptionType::CLASSIC || request.option_type == OptionType::SPRINT);
            case AccountInfoType::ORDER_TYPE_AVAILABILITY:
                return (request.order_type == OrderType::BUY || request.order_type == OrderType::SELL);
            case AccountInfoType::ACCOUNT_TYPE_AVAILABILITY:
                return (account_type != AccountType::UNKNOWN && request.account_type == account_type);
            case AccountInfoType::CURRENCY_AVAILABILITY:
                return (currency != CurrencyType::UNKNOWN && request.currency == currency);
            case AccountInfoType::TRADE_LIMIT_NOT_EXCEEDED:
                return (open_trades < max_trades);
            case AccountInfoType::AMOUNT_BELOW_MAX:
                return request.amount <= get_max_amount(request);
            case AccountInfoType::AMOUNT_ABOVE_MIN:
                return request.amount >= get_min_amount(request);
            case AccountInfoType::REFUND_BELOW_MAX:
                return true;
            case AccountInfoType::REFUND_ABOVE_MIN:
                return true;
            case AccountInfoType::DURATION_AVAILABLE: {
                if (request.option_type == OptionType::CLASSIC) return true;
                const int64_t req_min_duration = (request.symbol == "BTCUSD" || request.symbol == "BTCUSDT") ? min_btc_duration : min_duration;
                return (request.duration >= req_min_duration && request.duration <= max_duration);
            }
            case AccountInfoType::EXPIRATION_DATE_AVAILABLE: {
                if (request.option_type == OptionType::SPRINT) return true;
                const int64_t sec_of_day = time_shield::sec_of_day(request.expiry_time);
                if (sec_of_day < start_time || sec_of_day > end_time) return false;
                if (sec_of_day % (5 * time_shield::SEC_PER_MIN) != 0) return false;
                const int64_t sec_close = request.expiry_time - request.timestamp;
                const int64_t min_sec = 300;
                return (sec_close > min_sec);
            }
            case AccountInfoType::PAYOUT_ABOVE_MIN:
                if (request.min_payout == 0.0) return true;
                return (get_payout(request) >= request.min_payout);
            case AccountInfoType::AMOUNT_BELOW_BALANCE:
                return (request.amount <= balance);
            default:
                break;
            }
            return false;
        }

        /// \brief Retrieves integer account information based on the request type.
        /// \param request Specifies the type of account information requested.
        /// \return Integer account information.
        int64_t get_info_int64(const AccountInfoRequest& request) const override final {
            switch (request.type) {
                case AccountInfoType::USER_ID: return user_id;
                case AccountInfoType::CONNECTION_STATUS: return static_cast<int64_t>(connect);
                case AccountInfoType::BALANCE: return static_cast<int64_t>(balance);
                case AccountInfoType::PLATFORM_TYPE: return static_cast<int64_t>(platform_type());
                case AccountInfoType::ACCOUNT_TYPE: return static_cast<int64_t>(account_type);
                case AccountInfoType::CURRENCY: return static_cast<int64_t>(currency);
                case AccountInfoType::OPEN_TRADES: return open_trades;
                case AccountInfoType::MAX_TRADES:
                    return check_amount_limits(time_shield::sec_of_day(request.timestamp)) ? max_limit_trades : max_trades;
                case AccountInfoType::PAYOUT:
                    return static_cast<int64_t>(get_payout(request) * 100.0);
                case AccountInfoType::MIN_AMOUNT: return static_cast<int64_t>(get_min_amount(request));
                case AccountInfoType::MAX_AMOUNT: return static_cast<int64_t>(get_max_amount(request));
                case AccountInfoType::MIN_DURATION:
                    return (request.symbol == "BTCUSD" || request.symbol == "BTCUSDT") ? min_btc_duration : min_duration;
                case AccountInfoType::MAX_DURATION:
                    return std::min(time_shield::start_of_min(end_time - time_shield::sec_of_day(request.timestamp)), max_duration);
                case AccountInfoType::START_TIME: return time_shield::start_of_day(request.timestamp) + start_time;
                case AccountInfoType::END_TIME: return time_shield::start_of_day(request.timestamp) + end_time;
                case AccountInfoType::ORDER_QUEUE_TIMEOUT: return order_queue_timeout;
                case AccountInfoType::RESPONSE_TIMEOUT: return responce_timeout;
                case AccountInfoType::ORDER_INTERVAL_MS: return order_interval_ms;
                default: break;
            }
            return 0;
        }

        /// \brief Retrieves floating-point account information based on the request type.
        /// \param request Specifies the type of account information requested.
        /// \return Floating-point account information.
        double get_info_f64(const AccountInfoRequest& request) const override final {
            switch (request.type) {
                case AccountInfoType::BALANCE: return balance;
                case AccountInfoType::PAYOUT: return get_payout(request);
                case AccountInfoType::MIN_AMOUNT: return get_min_amount(request);
                case AccountInfoType::MAX_AMOUNT: return get_max_amount(request);
                default: break;
            }
            return 0;
        }

        /// \brief Retrieves string account information based on the request type.
        /// \param request Specifies the type of account information requested.
        /// \return String account information.
        std::string get_info_str(const AccountInfoRequest& request) const override final {
            switch (request.type) {
                case AccountInfoType::USER_ID: return std::to_string(user_id);
                case AccountInfoType::BALANCE: return utils::format("%.2f", balance);
                default: break;
            }
            return std::string();
        }

        CurrencyType get_info_currency(const AccountInfoRequest& request) const override final {
            return currency;
        }

        AccountType get_info_account_type(const AccountInfoRequest& request) const override final {
            return account_type;
        }

        double get_min_amount(const AccountInfoRequest& request) const {
            return request.currency == CurrencyType::USD ?
                min_usd_amount : (request.currency == CurrencyType::RUB ?
                min_rub_amount : (currency == CurrencyType::USD ?
                min_usd_amount : (currency == CurrencyType::RUB ? min_rub_amount : 0.0)));
        }

        double get_max_amount(const AccountInfoRequest& request) const {
            if (check_amount_limits(time_shield::sec_of_day(request.timestamp))) {
                return request.currency == CurrencyType::USD ?
                    max_usd_limit_amount : (request.currency == CurrencyType::RUB ?
                    max_rub_limit_amount : (currency == CurrencyType::USD ?
                    max_usd_limit_amount : (currency == CurrencyType::RUB ? max_rub_limit_amount : 0.0)));
            }
            return request.currency == CurrencyType::USD ?
                    max_usd_amount : (request.currency == CurrencyType::RUB ?
                    max_rub_amount : (currency == CurrencyType::USD ?
                    max_usd_amount : (currency == CurrencyType::RUB ? max_rub_amount : 0.0)));
        }

        /// \brief Checks if the amount limits apply based on the time of day.
        /// \param second_day The time in seconds since the start of the day.
        /// \return True if amount limits are in effect, false otherwise.
        bool check_amount_limits(const int second_day) const {
            // Check if the given time of day falls within specified high-risk periods
            return false;
        }

        double get_payout(const AccountInfoRequest& request) const {
            return 0.8;
        }

        int64_t get_classic_bo_expiration(int64_t timestamp, int64_t closing_timestamp) const {
            const int64_t min_exp = 5 * time_shield::SEC_PER_MIN;
            if ((closing_timestamp % min_exp) != 0) return 0;
            const int64_t diff = closing_timestamp - timestamp;
            const int64_t min_diff = 3 * time_shield::SEC_PER_MIN;
            if (diff <= min_diff) return 0;
            return ((((diff - 1) / time_shield::SEC_PER_MIN - 3) / 5) * 5 + 5) * time_shield::SEC_PER_MIN;
        }

        int64_t get_classic_bo_closing_timestamp(int64_t timestamp, int64_t expiration) const {
            if ((expiration % 5) != 0 || expiration < 5) return 0;
            const int64_t timestamp_future = timestamp + (expiration + 3) * time_shield::SEC_PER_MIN;
            return (timestamp_future - timestamp_future % (5 * time_shield::SEC_PER_MIN));
        }
    }; // AccountInfoData

    /// \class TradeManagerTest
    /// \brief Test implementation of BaseTradeExecutionComponent for unit testing.
    class TradeManagerTest : public BaseTradeExecutionComponent {
    public:
        TradeManagerTest(utils::EventBus& bus, std::shared_ptr<BaseAccountInfoData> account_info)
            : BaseTradeExecutionComponent(bus, std::move(account_info)) {
            subscribe<events::AuthDataEvent>();
        }

        /// \brief Overrides API type retrieval for the test.
        /// \return Dummy ApiType for testing.
        PlatformType platform_type() const override {
            return PlatformType::SIMULATOR;
        }
    };

    /// \class TradeTestMediator
    /// \brief Mediator component for handling test events.
    class TradeTestMediator : public utils::EventMediator {
    public:
        explicit TradeTestMediator(utils::EventBus& bus) 
                : utils::EventMediator(bus) {
            subscribe<events::TradeStatusEvent>();
            subscribe<events::TradeRequestEvent>();
            subscribe<events::OpenTradesEvent>();
        }

        void on_event(const utils::Event* const event) {
            if (auto status_event = dynamic_cast<const events::TradeStatusEvent*>(event)) {
                LOGIT_0DEBUG();
                auto request = status_event->request;
                auto result = status_event->result;
                m_task_manager.add_delayed_task(1000, [this, request, result](std::shared_ptr<utils::Task>){
                    LOGIT_0DEBUG();
                    result->trade_state = result->live_state = TradeState::WIN;
                    result->close_price = 1.12360;
                });
            } else
            if (auto request_event = dynamic_cast<const events::TradeRequestEvent*>(event)) {
                LOGIT_0DEBUG();
                auto request = request_event->request;
                auto result = request_event->result;
                m_task_manager.add_delayed_task(1000, [this, request, result](std::shared_ptr<utils::Task>){
                    LOGIT_0DEBUG();
                    result->trade_state = result->live_state = TradeState::OPEN_SUCCESS;
                    result->open_price = result->close_price = 1.12335;
                    result->open_date = OPTIONX_TIMESTAMP_MS;
                    result->close_date = result->open_date + time_shield::sec_to_ms(request->duration);
                });
            } else
            if (auto trades_event = dynamic_cast<const events::OpenTradesEvent*>(event)) {
                LOGIT_INFO(trades_event->open_trades);
            }
        }

        void process() {
            m_task_manager.process();
        }

        void shutdown() {
            m_task_manager.shutdown();
        }

    private:
        utils::TaskManager m_task_manager;
    };

    class OpenTradesCapture : public utils::EventMediator {
    public:
        OpenTradesCapture(
                utils::EventBus& bus,
                std::shared_ptr<AccountInfoData> account_info)
            : utils::EventMediator(bus),
              m_account_info(std::move(account_info)) {
            subscribe<events::OpenTradesEvent>();
        }

        void on_event(const utils::Event* const event) override {
            if (auto trades_event = dynamic_cast<const events::OpenTradesEvent*>(event)) {
                counts.push_back(trades_event->open_trades);
                if (m_account_info) {
                    m_account_info->open_trades = trades_event->open_trades;
                }
            }
        }

        int64_t last_count() const {
            return counts.empty() ? -1 : counts.back();
        }

        std::vector<int64_t> counts;

    private:
        std::shared_ptr<AccountInfoData> m_account_info;
    };

    class SnapshotRefreshCapture : public utils::EventMediator {
    public:
        explicit SnapshotRefreshCapture(utils::EventBus& bus)
            : utils::EventMediator(bus) {
            subscribe<events::OpenTradesSnapshotRefreshRequestEvent>();
        }

        void on_event(const utils::Event* const event) override {
            if (auto refresh_event =
                    dynamic_cast<const events::OpenTradesSnapshotRefreshRequestEvent*>(event)) {
                reasons.push_back(refresh_event->reason);
            }
        }

        std::size_t count() const {
            return reasons.size();
        }

        std::vector<std::string> reasons;
    };

    class TradeRequestCapture : public utils::EventMediator {
    public:
        explicit TradeRequestCapture(utils::EventBus& bus)
            : utils::EventMediator(bus) {
            subscribe<events::TradeRequestEvent>();
        }

        void on_event(const utils::Event* const event) override {
            if (auto request_event = dynamic_cast<const events::TradeRequestEvent*>(event)) {
                results.push_back(request_event->result);
            }
        }

        std::vector<std::shared_ptr<TradeResult>> results;
    };

    /// \brief Outputs the details of a TradeRequest.
    /// \param request The TradeRequest instance to output.
    void print_trade_request(const TradeRequest& request) {
        LOGIT_STREAM_TRACE()
            << "TradeRequest Details:\n"
            << "---------------------\n"
            << "Symbol: " << request.symbol << "\n"
            << "Signal Name: " << request.signal_name << "\n"
            << "User Data: " << request.user_data << "\n"
            << "Comment: " << request.comment << "\n"
            << "Unique Hash: " << request.unique_hash << "\n"
            << "Unique ID: " << request.unique_id << "\n"
            << "Account ID: " << request.account_id << "\n"
            << "Option Type: " << to_str(request.option_type) << "\n"
            << "Order Type: " << to_str(request.order_type) << "\n"
            << "Account Type: " << to_str(request.account_type) << "\n"
            << "Currency: " << to_str(request.currency) << "\n"
            << "Amount: " << request.amount << "\n"
            << "Refund: " << request.refund << "\n"
            << "Min Payout: " << request.min_payout << "\n"
            << "Duration: " << request.duration << " seconds\n"
            << "Expiry Time: " << request.expiry_time << " (Unix timestamp)\n";
    }

    /// \brief Outputs the details of a TradeResult.
    /// \param result The TradeResult instance to output.
    void print_trade_result(const TradeResult& result) {
        LOGIT_STREAM_TRACE()
            << "TradeResult Details:\n"
            << "--------------------\n"
            << "Error Code: " << to_str(result.error_code) << "\n"
            << "Error Description: " << result.error_desc << "\n"
            << "Option Hash: " << result.option_hash << "\n"
            << "Option ID: " << result.option_id << "\n"
            << "Amount: " << result.amount << "\n"
            << "Payout: " << result.payout << "\n"
            << "Profit: " << result.profit << "\n"
            << "Balance: " << result.balance << "\n"
            << "Open Price: " << result.open_price << "\n"
            << "Close Price: " << result.close_price << "\n"
            << "Delay: " << result.delay << " ms\n"
            << "Ping: " << result.ping << " ms\n"
            << "Place Date: " << result.place_date << " (Unix ms)\n"
            << "Send Date: " << result.send_date << " (Unix ms)\n"
            << "Open Date: " << result.open_date << " (Unix ms)\n"
            << "Close Date: " << result.close_date << " (Unix ms)\n"
            << "State: " << to_str(result.trade_state) << "\n"
            << "Current State: " << to_str(result.live_state) << "\n"
            << "Account Type: " << to_str(result.account_type) << "\n"
            << "Currency: " << to_str(result.currency) << "\n"
            << "API Type: " << to_str(result.platform_type) << "\n";
    }

} // namespace optionx::components

using namespace optionx;
using namespace optionx::components;

/// \brief Test fixture for TradeManager tests.
class TradeManagerTestFixture : public ::testing::Test {
protected:
    /// \brief Set up the test environment.
    void SetUp() override {

    }

    /// \brief Tear down the test environment.
    void TearDown() override {

    }
};

bool wait_for_open_trades_count(
        TradeManagerTest& trade_manager,
        utils::EventBus& bus,
        OpenTradesCapture& capture,
        int64_t expected_count,
        int64_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        trade_manager.process();
        bus.process();
        if (capture.last_count() == expected_count) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    trade_manager.process();
    bus.process();
    return capture.last_count() == expected_count;
}

std::unique_ptr<TradeRequest> make_valid_sprint_trade_request() {
    auto trade_request = std::make_unique<TradeRequest>();
    trade_request->symbol = "EURUSD";
    trade_request->amount = 100.0;
    trade_request->option_type = OptionType::SPRINT;
    trade_request->order_type = OrderType::BUY;
    trade_request->duration = 10;
    return trade_request;
}

std::unique_ptr<events::PriceUpdateEvent> make_price_update_event(
        const std::string& symbol,
        const Tick& tick,
        std::uint32_t price_digits) {
    std::vector<events::TickUpdateBatch> batches;
    batches.push_back(events::PriceUpdateEvent::make_tick_batch(
        tick,
        symbol,
        "test",
        price_digits,
        0));
    return std::make_unique<events::PriceUpdateEvent>(std::move(batches));
}

TEST(AccountInfoDataTest, PayoutAboveMinAcceptsBrokerPayoutGreaterThanMinimum) {
    optionx::platforms::intrade_bar::AccountInfoData account_info;
    account_info.currency = CurrencyType::USD;
    account_info.account_type = AccountType::DEMO;

    auto request = std::make_shared<TradeRequest>();
    request->symbol = "EURUSD";
    request->amount = 10.0;
    request->currency = CurrencyType::USD;
    request->account_type = AccountType::DEMO;
    request->option_type = OptionType::SPRINT;
    request->order_type = OrderType::BUY;
    request->duration = 180;
    const int64_t timestamp = 1719302400;

    request->min_payout = 0.81;
    EXPECT_TRUE(account_info.get_for_trade<bool>(
        AccountInfoType::PAYOUT_ABOVE_MIN,
        request,
        timestamp));

    request->min_payout = 0.82;
    EXPECT_TRUE(account_info.get_for_trade<bool>(
        AccountInfoType::PAYOUT_ABOVE_MIN,
        request,
        timestamp));

    request->min_payout = 0.83;
    EXPECT_FALSE(account_info.get_for_trade<bool>(
        AccountInfoType::PAYOUT_ABOVE_MIN,
        request,
        timestamp));
}

TEST(TradeStateManagerTest, DetermineTradeStateUsesNormalizedClosePrice) {
    auto account_info = std::make_shared<AccountInfoData>();
    AccountInfoProvider account_info_provider(account_info);
    TradeStateManager manager(account_info_provider);

    auto result = std::make_shared<TradeResult>();
    auto request = std::make_shared<TradeRequest>();
    result->open_price = 1.12335;
    request->order_type = OrderType::BUY;

    Tick tick;
    tick.bid = 1.1233548;
    tick.ask = 1.1233550;
    tick.set_flag(MarketDataFlags::INITIALIZED);

    ASSERT_GT(tick.mid_price(), result->open_price);
    const double close_price = tick.mid_price(5);
    EXPECT_DOUBLE_EQ(close_price, 1.12335);
    EXPECT_EQ(
        manager.determine_trade_state(result, request, close_price),
        TradeState::STANDOFF);
}

TEST_F(TradeManagerTestFixture, OrderIntervalDelaysQueuedTrades) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;
    account_info->order_interval_ms = 80;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);

    ASSERT_TRUE(trade_manager.place_trade(make_valid_sprint_trade_request()));
    ASSERT_TRUE(trade_manager.place_trade(make_valid_sprint_trade_request()));

    trade_manager.process();
    bus.process();

    ASSERT_EQ(capture.last_count(), 1);
    ASSERT_EQ(capture.counts.size(), 1u);
    const std::size_t updates_after_first_trade = capture.counts.size();

    trade_manager.process();
    bus.process();

    EXPECT_EQ(capture.last_count(), 1);
    EXPECT_EQ(capture.counts.size(), updates_after_first_trade);

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_TRUE(wait_for_open_trades_count(trade_manager, bus, capture, 2, 1000));

    trade_manager.shutdown();
}

TEST_F(TradeManagerTestFixture, OpenBalanceUsesTrustedIdleBalanceDuringTradeStorm) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;
    account_info->order_interval_ms = 0;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture open_trades_capture(bus, account_info);
    TradeRequestCapture request_capture(bus);

    ASSERT_TRUE(trade_manager.place_trade(make_valid_sprint_trade_request()));
    trade_manager.process();
    bus.process();

    ASSERT_EQ(request_capture.results.size(), 1u);
    ASSERT_TRUE(request_capture.results[0]->has_balance());
    ASSERT_TRUE(request_capture.results[0]->has_open_balance());
    EXPECT_DOUBLE_EQ(request_capture.results[0]->balance, 1000.0);
    EXPECT_DOUBLE_EQ(request_capture.results[0]->open_balance, 1000.0);

    account_info->balance = 900.0;
    ASSERT_TRUE(trade_manager.place_trade(make_valid_sprint_trade_request()));
    trade_manager.process();
    bus.process();

    ASSERT_EQ(request_capture.results.size(), 2u);
    ASSERT_TRUE(request_capture.results[1]->has_balance());
    ASSERT_TRUE(request_capture.results[1]->has_open_balance());
    EXPECT_DOUBLE_EQ(request_capture.results[1]->balance, 900.0);
    EXPECT_DOUBLE_EQ(request_capture.results[1]->open_balance, 1000.0);

    request_capture.results[0]->profit = 8.0;
    request_capture.results[0]->trade_state = TradeState::WIN;
    request_capture.results[0]->live_state = TradeState::WIN;
    trade_manager.process();
    bus.process();

    account_info->balance = 800.0;
    ASSERT_TRUE(trade_manager.place_trade(make_valid_sprint_trade_request()));
    trade_manager.process();
    bus.process();

    ASSERT_EQ(request_capture.results.size(), 3u);
    ASSERT_TRUE(request_capture.results[2]->has_balance());
    ASSERT_TRUE(request_capture.results[2]->has_open_balance());
    EXPECT_DOUBLE_EQ(request_capture.results[2]->balance, 800.0);
    EXPECT_DOUBLE_EQ(request_capture.results[2]->open_balance, 1008.0);

    EXPECT_EQ(open_trades_capture.last_count(), 2);
    trade_manager.shutdown();
}

TEST_F(TradeManagerTestFixture, BrokerSnapshotOpenTradesCountDownByCloseTime) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;
    account_info->order_interval_ms = 0;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);

    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    events::OpenTradesSnapshotEvent snapshot(
        3,
        {
            now_ms + 100,
            now_ms + 300,
            now_ms + 500
        },
        0);
    bus.notify(snapshot);

    ASSERT_EQ(capture.last_count(), 3);
    EXPECT_TRUE(wait_for_open_trades_count(trade_manager, bus, capture, 2, 1000));
    EXPECT_TRUE(wait_for_open_trades_count(trade_manager, bus, capture, 1, 1000));
    EXPECT_TRUE(wait_for_open_trades_count(trade_manager, bus, capture, 0, 1000));
    EXPECT_EQ(account_info->open_trades, 0);

    trade_manager.shutdown();
}

TEST_F(TradeManagerTestFixture, BrokerSnapshotWithMissingCloseTimesRequestsRefresh) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;
    account_info->order_interval_ms = 0;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);
    SnapshotRefreshCapture refresh_capture(bus);

    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    events::OpenTradesSnapshotEvent snapshot(
        3,
        { now_ms + 100 },
        0);
    bus.notify(snapshot);

    ASSERT_EQ(capture.last_count(), 3);
    ASSERT_EQ(refresh_capture.count(), 1u);
    EXPECT_EQ(refresh_capture.reasons.back(), "unknown-close-times");

    EXPECT_TRUE(wait_for_open_trades_count(trade_manager, bus, capture, 2, 1000));
    EXPECT_EQ(account_info->open_trades, 2);
    EXPECT_EQ(refresh_capture.count(), 1u);

    trade_manager.shutdown();
}

TEST_F(TradeManagerTestFixture, BrokerSnapshotReplacementUpdatesCount) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);

    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    bus.notify(events::OpenTradesSnapshotEvent(
        3,
        {
            now_ms + time_shield::MS_PER_MIN,
            now_ms + time_shield::MS_PER_MIN * 2,
            now_ms + time_shield::MS_PER_MIN * 3
        },
        0));
    ASSERT_EQ(capture.last_count(), 3);

    bus.notify(events::OpenTradesSnapshotEvent(
        1,
        { now_ms + time_shield::MS_PER_MIN },
        0));
    EXPECT_EQ(capture.last_count(), 1);
    EXPECT_EQ(account_info->open_trades, 1);

    trade_manager.shutdown();
}

TEST_F(TradeManagerTestFixture, BrokerSnapshotWithZeroTradesClearsPreviousCount) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);

    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    bus.notify(events::OpenTradesSnapshotEvent(
        2,
        {
            now_ms + time_shield::MS_PER_MIN,
            now_ms + time_shield::MS_PER_MIN * 2
        },
        0));
    ASSERT_EQ(capture.last_count(), 2);

    bus.notify(events::OpenTradesSnapshotEvent(0, {}, 0));
    EXPECT_EQ(capture.last_count(), 0);
    EXPECT_EQ(account_info->open_trades, 0);

    trade_manager.shutdown();
}

TEST_F(TradeManagerTestFixture, BrokerSnapshotClearsOnDisconnect) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);

    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    bus.notify(events::OpenTradesSnapshotEvent(
        2,
        {
            now_ms + time_shield::MS_PER_MIN,
            now_ms + time_shield::MS_PER_MIN * 2
        },
        0));
    ASSERT_EQ(capture.last_count(), 2);

    events::DisconnectRequestEvent disconnect([](const ConnectionResult&) {});
    bus.notify(disconnect);

    EXPECT_EQ(capture.last_count(), 0);
    EXPECT_EQ(account_info->open_trades, 0);
}

TEST_F(TradeManagerTestFixture, BrokerSnapshotTrimsExtraCloseTimes) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);

    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    bus.notify(events::OpenTradesSnapshotEvent(
        1,
        {
            now_ms + 100,
            now_ms + 200,
            now_ms + 300
        },
        0));
    ASSERT_EQ(capture.last_count(), 1);

    EXPECT_TRUE(wait_for_open_trades_count(trade_manager, bus, capture, 0, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    trade_manager.process();
    bus.process();
    EXPECT_EQ(capture.last_count(), 0);

    trade_manager.shutdown();
}

TEST_F(TradeManagerTestFixture, BrokerSnapshotCloseBufferDelaysCountDown) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);

    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    bus.notify(events::OpenTradesSnapshotEvent(
        1,
        { now_ms + 100 },
        250));
    ASSERT_EQ(capture.last_count(), 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(175));
    trade_manager.process();
    bus.process();
    EXPECT_EQ(capture.last_count(), 1);

    EXPECT_TRUE(wait_for_open_trades_count(trade_manager, bus, capture, 0, 1000));

    trade_manager.shutdown();
}

TEST_F(TradeManagerTestFixture, BrokerSnapshotCloseBufferSaturatesOnOverflow) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);

    bus.notify(events::OpenTradesSnapshotEvent(
        1,
        { std::numeric_limits<int64_t>::max() - 1 },
        1000));
    ASSERT_EQ(capture.last_count(), 1);

    trade_manager.process();
    bus.process();
    EXPECT_EQ(capture.last_count(), 1);
    EXPECT_EQ(account_info->open_trades, 1);

    trade_manager.shutdown();
}

TEST_F(TradeManagerTestFixture, BrokerSnapshotCombinesWithLocalOpenTrades) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;
    account_info->order_interval_ms = 0;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);

    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    events::OpenTradesSnapshotEvent snapshot(
        2,
        {
            now_ms + time_shield::MS_PER_MIN,
            now_ms + time_shield::MS_PER_MIN * 2
        },
        0);
    bus.notify(snapshot);
    ASSERT_EQ(capture.last_count(), 2);
    ASSERT_EQ(account_info->open_trades, 2);

    auto trade_request = std::make_unique<TradeRequest>();
    trade_request->symbol = "EURUSD";
    trade_request->amount = 100.0;
    trade_request->option_type = OptionType::SPRINT;
    trade_request->order_type = OrderType::BUY;
    trade_request->duration = 10;

    ASSERT_TRUE(trade_manager.place_trade(std::move(trade_request)));
    trade_manager.process();
    bus.process();

    EXPECT_EQ(capture.last_count(), 3);
    EXPECT_EQ(account_info->open_trades, 3);

    trade_manager.shutdown();
}

TEST_F(TradeManagerTestFixture, BrokerSnapshotIgnoredWhileLocalQueueIsActive) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->user_id = 12345;
    account_info->balance = 1000.0;
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;
    account_info->order_interval_ms = 0;

    TradeManagerTest trade_manager(bus, account_info);
    OpenTradesCapture capture(bus, account_info);
    SnapshotRefreshCapture refresh_capture(bus);

    auto trade_request = std::make_unique<TradeRequest>();
    trade_request->symbol = "EURUSD";
    trade_request->amount = 100.0;
    trade_request->option_type = OptionType::SPRINT;
    trade_request->order_type = OrderType::BUY;
    trade_request->duration = 10;

    ASSERT_TRUE(trade_manager.place_trade(std::move(trade_request)));
    trade_manager.process();
    bus.process();
    ASSERT_EQ(capture.last_count(), 1);
    ASSERT_EQ(account_info->open_trades, 1);

    const int64_t now_ms = OPTIONX_TIMESTAMP_MS;
    events::OpenTradesSnapshotEvent snapshot(
        4,
        {
            now_ms - 100,
            now_ms - 100,
            now_ms - 100,
            now_ms - 100
        },
        0);
    bus.notify(snapshot);
    trade_manager.process();
    bus.process();

    EXPECT_EQ(capture.last_count(), 1);
    EXPECT_EQ(account_info->open_trades, 1);
    ASSERT_EQ(refresh_capture.count(), 1u);
    EXPECT_EQ(refresh_capture.reasons.back(), "local-queue-active");

    trade_manager.shutdown();
}

/// \brief Tests that a valid trade request is processed successfully.
///
/// The test places a BUY trade for "EURUSD" and then simulates a price update
/// event. The immediate mediator updates the trade state so that the final state
/// is expected to be WIN.
TEST_F(TradeManagerTestFixture, ValidTradeTest) {
    // Initialize the event bus.
    utils::EventBus bus;

    // Create shared account info.
    auto account_info = std::make_shared<AccountInfoData>();
    // Set dummy account info.
    account_info->user_id      = 12345;
    account_info->balance      = 1000.0;
    account_info->currency     = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect      = true;

    // Create the test TradeManager.
    TradeManagerTest trade_manager(bus, account_info);

    // Create the test mediator to handle events
    TradeTestMediator test_mediator(bus);

    // Atomic flag to control the processing loop
    std::atomic<bool> processing{true};

    // Start processing in a separate thread
    std::thread processor([&processing, &bus, &trade_manager, &test_mediator]() {
        while (processing) {
            trade_manager.process();
            test_mediator.process();
            bus.process();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Finalize all trades
        LOGIT_STREAM_TRACE() << "Finalizing all trades...\n";
        trade_manager.shutdown();
        test_mediator.shutdown();
    });

    std::atomic<int>  callback_step{0};

    // Create a valid trade request.
    auto trade_request = std::make_unique<TradeRequest>();
    trade_request->symbol      = "EURUSD";
    trade_request->amount      = 100.0;
    trade_request->option_type = OptionType::SPRINT;
    trade_request->order_type  = OrderType::BUY;
    trade_request->duration    = 10;
    trade_request->add_callback([&callback_step](
            std::unique_ptr<TradeRequest> req,
            std::unique_ptr<TradeResult> res) {
        LOGIT_STREAM_TRACE() << "State: " << to_str(res->trade_state);
        switch (res->trade_state) {
        case TradeState::WAITING_OPEN:
            callback_step = 1;
            break;
        case TradeState::OPEN_SUCCESS:
            if (callback_step == 1) callback_step = 2;
            break;
        case TradeState::IN_PROGRESS:
            if (callback_step == 2) callback_step = 3;
            break;
        case TradeState::WAITING_CLOSE:
            if (callback_step == 3) callback_step = 4;
            break;
        case TradeState::WIN:
            if (callback_step == 4) callback_step = 5;
            break;
        default:
            callback_step = 0;
            break;
        }
    });

    // Place the trade.
    bool placed = trade_manager.place_trade(std::move(trade_request));
    ASSERT_TRUE(placed);

    std::this_thread::sleep_for(std::chrono::milliseconds(5000));

    // Simulate a price update event to trigger closing.
    Tick tick;
    // Set tick price data so that mid_price > open_price.
    // For example, for BUY order if mid_price > 1.12335 then trade is WIN.
    tick = { 1.12350, 1.12340, 0.1, 1695483030000, 1695483031000, 0 };
    optionx::mark_live_payload(tick.flags);
    tick.set_flag(optionx::MarketDataFlags::INITIALIZED);

    bus.notify_async(make_price_update_event("EURUSD", tick, 5));

    // Wait a short time for the callback to be invoked.
    int wait_iterations = 0;
    while (callback_step != 5 && wait_iterations < 30) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        wait_iterations++;
    }

    // Stop processing and join the thread
    processing = false;
    processor.join();

    EXPECT_EQ(callback_step, 5);

    LOGIT_WAIT();
}

TEST_F(TradeManagerTestFixture, InvalidTradeTest) {
    // Initialize the event bus.
    utils::EventBus bus;

    // Create shared account info.
    auto account_info = std::make_shared<AccountInfoData>();
    // Set dummy account info.
    account_info->user_id      = 12345;
    account_info->balance      = 1000.0;
    account_info->currency     = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect      = true;

    // Create the test TradeManager.
    TradeManagerTest trade_manager(bus, account_info);

    // Create the test mediator to handle events
    TradeTestMediator test_mediator(bus);

    // Atomic flag to control the processing loop
    std::atomic<bool> processing{true};

    // Start processing in a separate thread
    std::thread processor([&processing, &bus, &trade_manager, &test_mediator]() {
        while (processing) {
            trade_manager.process();
            test_mediator.process();
            bus.process();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Finalize all trades
        LOGIT_STREAM_TRACE() << "Finalizing all trades...\n";
        trade_manager.shutdown();
        test_mediator.shutdown();
    });

    std::atomic<int> callback_step{0};
    TradeErrorCode final_error = TradeErrorCode::SUCCESS;

    // Create a valid trade request.
    auto trade_request = std::make_unique<TradeRequest>();
    trade_request->symbol      = "";
    trade_request->amount      = 100.0;
    trade_request->option_type = OptionType::SPRINT;
    trade_request->order_type  = OrderType::BUY;
    trade_request->duration    = 10;
    trade_request->add_callback([&callback_step, &final_error](
            std::unique_ptr<TradeRequest> req,
            std::unique_ptr<TradeResult> res) {
        LOGIT_STREAM_TRACE() << "State: " << to_str(res->trade_state);
        switch (res->trade_state) {
        case TradeState::OPEN_ERROR:
            callback_step = 1;
            final_error = res->error_code;
            break;
        default:
            callback_step = 0;
            break;
        }
    });

    // Place the trade.
    bool placed = trade_manager.place_trade(std::move(trade_request));
    ASSERT_TRUE(placed);

    std::this_thread::sleep_for(std::chrono::milliseconds(5000));

    // Simulate a price update event to trigger closing.
    Tick tick;
    // Set tick price data so that mid_price > open_price.
    // For example, for BUY order if mid_price > 1.12335 then trade is WIN.
    tick = { 1.12350, 1.12340, 0.1, 1695483030000, 1695483031000, 0 };
    optionx::mark_live_payload(tick.flags);
    tick.set_flag(optionx::MarketDataFlags::INITIALIZED);

    bus.notify_async(make_price_update_event("EURUSD", tick, 5));

    // Wait a short time for the callback to be invoked.
    int wait_iterations = 0;
    while (callback_step != 1 && wait_iterations < 30) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        wait_iterations++;
    }

    // Stop processing and join the thread
    processing = false;
    processor.join();

    EXPECT_EQ(callback_step, 1);
    // Expect that an empty symbol leads to INVALID_SYMBOL error.
    EXPECT_EQ(final_error, TradeErrorCode::INVALID_SYMBOL);

    LOGIT_WAIT();
}

TEST_F(TradeManagerTestFixture, ShutdownTest) {
    // Initialize the event bus.
    utils::EventBus bus;

    // Create shared account info.
    auto account_info = std::make_shared<AccountInfoData>();
    // Set dummy account info.
    account_info->user_id      = 12345;
    account_info->balance      = 1000.0;
    account_info->currency     = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect      = true;

    // Create the test TradeManager.
    TradeManagerTest trade_manager(bus, account_info);

    // Create the test mediator to handle events
    TradeTestMediator test_mediator(bus);

    // Atomic flag to control the processing loop
    std::atomic<bool> processing{true};

    // Start processing in a separate thread
    std::thread processor([&processing, &bus, &trade_manager, &test_mediator]() {
        while (processing) {
            trade_manager.process();
            test_mediator.process();
            bus.process();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Finalize all trades
        LOGIT_STREAM_TRACE() << "Finalizing all trades...\n";
        trade_manager.shutdown();
        test_mediator.shutdown();
    });

    std::atomic<int> callback_step{0};
    TradeErrorCode final_error = TradeErrorCode::SUCCESS;

    // Create a valid trade request.
    auto trade_request = std::make_unique<TradeRequest>();
    trade_request->symbol      = "EURUSD";
    trade_request->amount      = 100.0;
    trade_request->option_type = OptionType::SPRINT;
    trade_request->order_type  = OrderType::BUY;
    trade_request->duration    = 10;
    trade_request->add_callback([&callback_step, &final_error](
            std::unique_ptr<TradeRequest> req,
            std::unique_ptr<TradeResult> res) {
        LOGIT_STREAM_TRACE() << "State: " << to_str(res->trade_state);
        switch (res->trade_state) {
        case TradeState::CHECK_ERROR:
            callback_step = 1;
            final_error = res->error_code;
            break;
        default:
            callback_step = 0;
            break;
        }
    });

    // Place the trade.
    bool placed = trade_manager.place_trade(std::move(trade_request));
    ASSERT_TRUE(placed);

    std::this_thread::sleep_for(std::chrono::milliseconds(5000));

    // Simulate a price update event to trigger closing.
    Tick tick;
    // Set tick price data so that mid_price > open_price.
    // For example, for BUY order if mid_price > 1.12335 then trade is WIN.
    tick = { 1.12350, 1.12340, 0.1, 1695483030000, 1695483031000, 0 };
    optionx::mark_live_payload(tick.flags);
    tick.set_flag(optionx::MarketDataFlags::INITIALIZED);

    bus.notify_async(make_price_update_event("EURUSD", tick, 5));

    // Wait a short time for the callback to be invoked.
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));

    // Stop processing and join the thread
    processing = false;
    processor.join();

    EXPECT_EQ(callback_step, 1);
    // Expect that an empty symbol leads to INVALID_SYMBOL error.
    EXPECT_EQ(final_error, TradeErrorCode::CLIENT_FORCED_CLOSE);

    LOGIT_WAIT();
}

TEST_F(TradeManagerTestFixture, TradeResultFactoriesCopyTradeId) {
    TradeRequest request;
    request.trade_id = 1234;

    auto unique_result = request.create_trade_result_unique();
    auto shared_result = request.create_trade_result_shared();

    ASSERT_NE(unique_result, nullptr);
    ASSERT_NE(shared_result, nullptr);
    EXPECT_EQ(unique_result->trade_id, request.trade_id);
    EXPECT_EQ(shared_result->trade_id, request.trade_id);
}

TEST_F(TradeManagerTestFixture, TradeTransactionEventPreservesRequestTradeId) {
    auto request = std::make_unique<TradeRequest>();
    request->trade_id = 555;

    events::TradeTransactionEvent event(request, PlatformType::SIMULATOR);

    ASSERT_NE(event.request, nullptr);
    ASSERT_NE(event.result, nullptr);
    EXPECT_EQ(event.request->trade_id, 555u);
    EXPECT_EQ(event.result->trade_id, 555u);
}

TEST_F(TradeManagerTestFixture, TradeIdProviderInitializesEmptyRequestId) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;

    TradeManagerTest trade_manager(bus, account_info);
    trade_manager.on_trade_id() = [] { return std::uint32_t{777}; };

    std::uint32_t callback_request_id = 0;
    std::uint32_t callback_result_id = 0;
    auto trade_request = std::make_unique<TradeRequest>();
    trade_request->symbol = "EURUSD";
    trade_request->amount = 100.0;
    trade_request->option_type = OptionType::SPRINT;
    trade_request->order_type = OrderType::BUY;
    trade_request->duration = 10;
    trade_request->add_callback([&](std::unique_ptr<TradeRequest> req, std::unique_ptr<TradeResult> res) {
        callback_request_id = req->trade_id;
        callback_result_id = res->trade_id;
    });

    ASSERT_TRUE(trade_manager.place_trade(std::move(trade_request)));
    trade_manager.shutdown();

    EXPECT_EQ(callback_request_id, 777u);
    EXPECT_EQ(callback_result_id, 777u);
}

TEST_F(TradeManagerTestFixture, TradeIdFallbackInitializesEmptyRequestId) {
    utils::EventBus bus;
    auto account_info = std::make_shared<AccountInfoData>();
    account_info->currency = CurrencyType::USD;
    account_info->account_type = AccountType::DEMO;
    account_info->connect = true;

    TradeManagerTest trade_manager(bus, account_info);

    std::uint32_t callback_request_id = 0;
    std::uint32_t callback_result_id = 0;
    auto trade_request = std::make_unique<TradeRequest>();
    trade_request->symbol = "EURUSD";
    trade_request->amount = 100.0;
    trade_request->option_type = OptionType::SPRINT;
    trade_request->order_type = OrderType::BUY;
    trade_request->duration = 10;
    trade_request->add_callback([&](std::unique_ptr<TradeRequest> req, std::unique_ptr<TradeResult> res) {
        callback_request_id = req->trade_id;
        callback_result_id = res->trade_id;
    });

    ASSERT_TRUE(trade_manager.place_trade(std::move(trade_request)));
    trade_manager.shutdown();

    EXPECT_GT(callback_request_id, 0u);
    EXPECT_EQ(callback_result_id, callback_request_id);
}

/// \brief Main entry point for GoogleTest.
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    LOGIT_ADD_CONSOLE_DEFAULT();
    LOGIT_ADD_FILE_LOGGER_DEFAULT();
    LOGIT_ADD_UNIQUE_FILE_LOGGER_DEFAULT_SINGLE_MODE();
    return RUN_ALL_TESTS();
}
