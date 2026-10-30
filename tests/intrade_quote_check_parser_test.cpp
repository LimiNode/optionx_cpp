#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <server_http.hpp>
#include <optionx_cpp/platforms.hpp>

using namespace optionx;
using namespace optionx::platforms::intrade_bar;

namespace {

using QuoteCheckHttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

class BoundQuoteCheckHttpServer : public QuoteCheckHttpServer {
public:
    unsigned short bound_port() const noexcept {
        return m_bound_port.load();
    }

protected:
    void after_bind() override {
        m_bound_port.store(acceptor->local_endpoint().port());
    }

private:
    std::atomic<unsigned short> m_bound_port{0};
};

class QuoteCheckServerFixture {
public:
    ~QuoteCheckServerFixture() {
        stop();
    }

    bool start() {
        server.config.address = "127.0.0.1";
        server.config.port = 0;
        server.resource["^/health$"]["GET"] = [](
                std::shared_ptr<QuoteCheckHttpServer::Response> response,
                std::shared_ptr<QuoteCheckHttpServer::Request>) {
            response->write(SimpleWeb::StatusCode::success_ok, "ok");
        };
        server.resource["^/quotes$"]["GET"] = [this](
                std::shared_ptr<QuoteCheckHttpServer::Response> response,
                std::shared_ptr<QuoteCheckHttpServer::Request> request) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                request_queries.push_back(request->query_string);
            }
            const auto cookie = request->header.find("Cookie");
            if (cookie != request->header.end() &&
                    cookie->second.find("PHPSESSID=fixture") != std::string::npos) {
                cookie_seen.store(true);
            }
            std::optional<std::string> forced_response;
            std::optional<std::string> sequence_response;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                forced_response = m_forced_response;
                if (!m_response_sequence.empty() &&
                        m_response_sequence_index < m_response_sequence.size()) {
                    sequence_response = m_response_sequence[m_response_sequence_index++];
                }
            }
            if (forced_response) {
                response->write(SimpleWeb::StatusCode::success_ok, *forced_response);
                return;
            }
            if (sequence_response) {
                response->write(SimpleWeb::StatusCode::success_ok, *sequence_response);
                return;
            }

            if (request->query_string.find("date=2026-10-28") != std::string::npos) {
                response->write(
                    SimpleWeb::StatusCode::success_ok,
                    R"HTML(<td class="partner_stat_table_left">23:59:59</td><td class="">1.19990</td><td class="">1.20000</td><td class="">1.20010</td>)HTML");
                return;
            }
            response->write(
                SimpleWeb::StatusCode::success_ok,
                R"HTML(<td class="partner_stat_table_left">00:00:00</td><td class="">1.20000</td><td class="">1.20010</td><td class="">1.20020</td>)HTML");
        };

        thread = std::thread([this]() {
            server.start();
        });
        return wait_until_ready();
    }

    void stop() {
        server.stop();
        if (thread.joinable()) thread.join();
    }

    std::string host() const {
        return "http://127.0.0.1:" + std::to_string(server.bound_port());
    }

    std::vector<std::string> queries() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return request_queries;
    }

    void set_response(std::string response) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_forced_response = std::move(response);
        m_response_sequence.clear();
        m_response_sequence_index = 0;
    }

    void set_response_sequence(std::vector<std::string> responses) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_forced_response.reset();
        m_response_sequence = std::move(responses);
        m_response_sequence_index = 0;
    }

    std::atomic<bool> cookie_seen{false};

private:
    bool wait_until_ready() const {
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (server.bound_port() != 0) {
                try {
                    kurlyk::HttpClient client(host());
                    client.set_timeout(1);
                    client.set_connect_timeout(1);
                    auto future = client.get("/health", {}, {});
                    if (future.wait_for(std::chrono::seconds(1)) ==
                            std::future_status::ready) {
                        const auto response = future.get();
                        if (response && response->status_code == 200) return true;
                    }
                } catch (...) {
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    BoundQuoteCheckHttpServer server;
    std::thread thread;
    mutable std::mutex m_mutex;
    std::vector<std::string> request_queries;
    std::optional<std::string> m_forced_response;
    std::vector<std::string> m_response_sequence;
    std::size_t m_response_sequence_index = 0;
};

class QuoteCheckTestPlatform final : public optionx::platforms::BaseTradingPlatform {
public:
    QuoteCheckTestPlatform()
        : optionx::platforms::BaseTradingPlatform(
            std::make_shared<AccountInfoData>()) {}

    optionx::PlatformType platform_type() const override {
        return optionx::PlatformType::INTRADE_BAR;
    }
};

BrokerQuoteCheckRequest make_request(
        std::uint64_t from_time_ms,
        std::uint64_t to_time_ms,
        std::int64_t broker_offset_sec = 3 * 60 * 60) {
    BrokerQuoteCheckRequest request;
    request.symbol = "EUR/USD";
    request.from_time_ms = from_time_ms;
    request.to_time_ms = to_time_ms;
    request.broker_utc_offset_sec = broker_offset_sec;
    return request;
}

TEST(IntradeQuoteCheckParser, UsesSelectedQuoteAndBuildsDenseSecondGrid) {
    const auto base_utc = time_shield::to_timestamp(2026, 10, 28, 9, 0, 1);
    const auto request = make_request(
        time_shield::sec_to_ms(base_utc),
        time_shield::sec_to_ms(base_utc + 2));
    const std::string html = R"HTML(
        <tr><td class="partner_stat_table_left">12:00:02, 28 Oct 2026</td>
            <td class="">ignored-1</td><td class="">1.23452</td></tr>
        <tr><td class="partner_stat_table_left">12:00:01, 28 Oct 2026</td>
            <td class="">ignored-2</td><td class="">1.23450</td></tr>
        <tr><td class="partner_stat_table_left">12:00:03, 28 Oct 2026</td>
            <td class="">ignored-3</td><td class="">1.23455</td></tr>
    )HTML";

    const auto result = detail::parse_quote_check_html(html, request);

    EXPECT_EQ(result.symbol, "EURUSD");
    EXPECT_EQ(result.source, INTRADE_QUOTE_CHECK_SOURCE);
    ASSERT_EQ(result.observations.size(), 3U);
    EXPECT_DOUBLE_EQ(result.observations[0].price, 1.23450);
    EXPECT_DOUBLE_EQ(result.observations[1].price, 1.23452);
    EXPECT_DOUBLE_EQ(result.observations[2].price, 1.23455);
    EXPECT_TRUE(result.range_complete);
    EXPECT_EQ(result.malformed_rows, 0U);
}

TEST(IntradeQuoteCheckParser, ParsesCurrentTimeOnlyQuoteRows) {
    const auto base_utc = time_shield::to_timestamp(2026, 10, 28, 9, 0, 1);
    const auto request = make_request(
        time_shield::sec_to_ms(base_utc),
        time_shield::sec_to_ms(base_utc + 2));
    const std::string html = R"HTML(
        <tr><td class="partner_stat_table_left">12:00:01</td>
            <td class="">1.23440</td><td class="">1.23450</td><td class="">1.23460</td></tr>
        <tr><td class="partner_stat_table_left">12:00:02</td>
            <td class="">1.23450</td><td class="">1.23455</td><td class="">1.23460</td></tr>
        <tr><td class="partner_stat_table_left">12:00:03</td>
            <td class="">1.23460</td><td class="">1.23465</td><td class="">1.23470</td></tr>
    )HTML";

    const auto result = detail::parse_quote_check_html(html, request);

    ASSERT_EQ(result.observations.size(), 3U);
    EXPECT_DOUBLE_EQ(result.observations[0].price, 1.23450);
    EXPECT_DOUBLE_EQ(result.observations[1].price, 1.23455);
    EXPECT_DOUBLE_EQ(result.observations[2].price, 1.23465);
    EXPECT_TRUE(result.range_complete);
    EXPECT_EQ(result.malformed_rows, 0U);
}

TEST(IntradeQuoteCheckParser, MissingSecondFailsClosed) {
    const auto base_utc = time_shield::to_timestamp(2026, 10, 28, 9, 0, 1);
    const auto request = make_request(
        time_shield::sec_to_ms(base_utc),
        time_shield::sec_to_ms(base_utc + 2));
    const std::string html = R"HTML(
        <tr><td class="partner_stat_table_left">12:00:01, 28 Oct 2026</td>
            <td class="">ignored</td><td class="">1.23450</td></tr>
        <tr><td class="partner_stat_table_left">12:00:03, 28 Oct 2026</td>
            <td class="">ignored</td><td class="">1.23455</td></tr>
    )HTML";

    const auto result = detail::parse_quote_check_html(html, request);

    ASSERT_EQ(result.observations.size(), 2U);
    EXPECT_FALSE(result.range_complete);
}

TEST(IntradeQuoteCheckParser, FiltersPartialSecondBoundariesWithoutRoundingOutput) {
    const auto base_utc = time_shield::to_timestamp(2026, 10, 28, 9, 0, 0);
    const auto request = make_request(
        time_shield::sec_to_ms(base_utc) + 250,
        time_shield::sec_to_ms(base_utc + 2) + 700);
    const std::string html = R"HTML(
        <tr><td class="partner_stat_table_left">12:00:00, 28 Oct 2026</td>
            <td class="">ignored</td><td class="">1.23449</td></tr>
        <tr><td class="partner_stat_table_left">12:00:01, 28 Oct 2026</td>
            <td class="">ignored</td><td class="">1.23450</td></tr>
        <tr><td class="partner_stat_table_left">12:00:02, 28 Oct 2026</td>
            <td class="">ignored</td><td class="">1.23451</td></tr>
        <tr><td class="partner_stat_table_left">12:00:03, 28 Oct 2026</td>
            <td class="">ignored</td><td class="">1.23452</td></tr>
    )HTML";

    const auto result = detail::parse_quote_check_html(html, request);

    ASSERT_EQ(result.observations.size(), 2U);
    EXPECT_EQ(result.observations[0].time_ms, time_shield::sec_to_ms(base_utc + 1));
    EXPECT_EQ(result.observations[1].time_ms, time_shield::sec_to_ms(base_utc + 2));
    EXPECT_TRUE(result.range_complete);
}

TEST(IntradeQuoteCheckParser, EmptySubsecondGridIsComplete) {
    const auto base_ms = time_shield::sec_to_ms(
        time_shield::to_timestamp(2026, 10, 28, 9, 0, 0));
    const auto result = detail::parse_quote_check_html(
        {},
        make_request(base_ms + 250, base_ms + 700));

    EXPECT_TRUE(result.observations.empty());
    EXPECT_TRUE(result.range_complete);
}

TEST(IntradeQuoteCheckParser, MalformedRowDoesNotBecomeAnObservation) {
    const auto timestamp = time_shield::to_timestamp(2026, 10, 28, 9, 0, 1);
    const auto request = make_request(
        time_shield::sec_to_ms(timestamp),
        time_shield::sec_to_ms(timestamp));
    const std::string html = R"HTML(
        <tr><td class="partner_stat_table_left">not-a-date</td>
            <td class="">ignored</td><td class="">not-a-price</td></tr>
    )HTML";

    const auto result = detail::parse_quote_check_html(html, request);

    EXPECT_TRUE(result.observations.empty());
    EXPECT_FALSE(result.range_complete);
    EXPECT_GT(result.malformed_rows, 0U);
}

TEST(IntradeQuoteCheckParser, SupportsARequestCrossingLocalMidnight) {
    const auto first = time_shield::to_timestamp(2026, 10, 28, 20, 59, 59);
    const auto second = first + 1;
    const auto request = make_request(
        time_shield::sec_to_ms(first),
        time_shield::sec_to_ms(second));
    const std::string html = R"HTML(
        <tr><td class="partner_stat_table_left">23:59:59, 28 Oct 2026</td>
            <td class="">ignored</td><td class="">1.20000</td></tr>
        <tr><td class="partner_stat_table_left">00:00:00, 29 Oct 2026</td>
            <td class="">ignored</td><td class="">1.20010</td></tr>
    )HTML";

    const auto result = detail::parse_quote_check_html(html, request);

    ASSERT_EQ(result.observations.size(), 2U);
    EXPECT_TRUE(result.range_complete);
    EXPECT_EQ(result.observations.front().time_ms, time_shield::sec_to_ms(first));
    EXPECT_EQ(result.observations.back().time_ms, time_shield::sec_to_ms(second));
}

TEST(IntradeQuoteCheckParser, InvalidRequestReturnsNoData) {
    const auto result = detail::parse_quote_check_html(
        "<td class=\"partner_stat_table_left\">12:00:00, 28 Oct 2026</td>",
        BrokerQuoteCheckRequest{});

    EXPECT_TRUE(result.observations.empty());
    EXPECT_FALSE(result.range_complete);
}

TEST(IntradeQuoteCheckParser, RequestManagerSplitsLocalMidnightAndMergesRows) {
    QuoteCheckServerFixture server;
    ASSERT_TRUE(server.start());

    QuoteCheckTestPlatform platform;
    HttpClientComponent client(platform);
    RequestManager request_manager(platform, client);
    client.get_http_client().set_host(server.host());
    client.get_http_client().set_origin(server.host());
    client.get_http_client().set_referer(server.host() + "/");
    request_manager.set_auth_credentials("fixture-user", "fixture-hash", "PHPSESSID=fixture");

    const auto first = time_shield::to_timestamp(2026, 10, 28, 20, 59, 59);
    const auto second = first + 1;
    const auto request = make_request(
        time_shield::sec_to_ms(first),
        time_shield::sec_to_ms(second));
    std::atomic<bool> completed{false};
    QuoteCheckApiResult result;

    ASSERT_TRUE(request_manager.request_quote_check_result(
        request,
        [&completed, &result](QuoteCheckApiResult value) {
            result = std::move(value);
            completed.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        client.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.load());
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.value.observations.size(), 2U);
    EXPECT_TRUE(result.value.range_complete);
    EXPECT_TRUE(server.cookie_seen.load());
    const auto queries = server.queries();
    ASSERT_EQ(queries.size(), 2U);
    EXPECT_NE(queries[0].find("symbol=EUR%2FUSD"), std::string::npos);
    EXPECT_NE(queries[0].find("date=2026-10-28"), std::string::npos);
    EXPECT_NE(queries[0].find("from=23%3A59%3A59"), std::string::npos);
    EXPECT_NE(queries[0].find("to=23%3A59%3A59"), std::string::npos);
    EXPECT_NE(queries[1].find("date=2026-10-29"), std::string::npos);
    EXPECT_NE(queries[1].find("from=00%3A00%3A00"), std::string::npos);
    EXPECT_NE(queries[1].find("to=00%3A00%3A00"), std::string::npos);
    platform.shutdown();
}

TEST(IntradeQuoteCheckParser, RequestManagerSplitsLongRangeIntoThirtyMinuteQueries) {
    QuoteCheckServerFixture server;
    ASSERT_TRUE(server.start());

    QuoteCheckTestPlatform platform;
    HttpClientComponent client(platform);
    RequestManager request_manager(platform, client);
    client.get_http_client().set_host(server.host());
    client.get_http_client().set_origin(server.host());
    client.get_http_client().set_referer(server.host() + "/");
    request_manager.set_auth_credentials("fixture-user", "fixture-hash", "PHPSESSID=fixture");

    const auto first = time_shield::to_timestamp(2026, 10, 28, 9, 0, 0);
    const auto request = make_request(
        time_shield::sec_to_ms(first),
        time_shield::sec_to_ms(first + 60 * time_shield::SEC_PER_MIN));
    std::atomic<bool> completed{false};
    QuoteCheckApiResult result;

    ASSERT_TRUE(request_manager.request_quote_check_result(
        request,
        [&completed, &result](QuoteCheckApiResult value) {
            result = std::move(value);
            completed.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        client.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.load());
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.value.range_complete);

    const auto queries = server.queries();
    ASSERT_EQ(queries.size(), 2U);
    EXPECT_NE(queries[0].find("from=12%3A00%3A00"), std::string::npos);
    EXPECT_NE(queries[0].find("to=12%3A30%3A00"), std::string::npos);
    EXPECT_NE(queries[1].find("from=12%3A30%3A01"), std::string::npos);
    EXPECT_NE(queries[1].find("to=13%3A00%3A00"), std::string::npos);
    platform.shutdown();
}

TEST(IntradeQuoteCheckParser, KeepsExactlyThirtyMinutesInOneBrokerQuery) {
    QuoteCheckServerFixture server;
    ASSERT_TRUE(server.start());

    QuoteCheckTestPlatform platform;
    HttpClientComponent client(platform);
    RequestManager request_manager(platform, client);
    client.get_http_client().set_host(server.host());
    client.get_http_client().set_origin(server.host());
    client.get_http_client().set_referer(server.host() + "/");
    request_manager.set_auth_credentials("fixture-user", "fixture-hash", "PHPSESSID=fixture");

    const auto first = time_shield::to_timestamp(2026, 10, 28, 9, 0, 0);
    std::atomic<bool> completed{false};
    QuoteCheckApiResult result;
    ASSERT_TRUE(request_manager.request_quote_check_result(
        make_request(
            time_shield::sec_to_ms(first),
            time_shield::sec_to_ms(first + MAX_INTRADE_QUOTE_CHECK_INTERVAL_SEC)),
        [&completed, &result](QuoteCheckApiResult value) {
            result = std::move(value);
            completed.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        client.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.load());
    ASSERT_TRUE(result.success);
    const auto queries = server.queries();
    ASSERT_EQ(queries.size(), 1U);
    EXPECT_NE(queries.front().find("from=12%3A00%3A00"), std::string::npos);
    EXPECT_NE(queries.front().find("to=12%3A30%3A00"), std::string::npos);
    platform.shutdown();
}

TEST(IntradeQuoteCheckParser, SplitsThirtyMinutesPlusOneSecondWithoutOverlap) {
    QuoteCheckServerFixture server;
    ASSERT_TRUE(server.start());

    QuoteCheckTestPlatform platform;
    HttpClientComponent client(platform);
    RequestManager request_manager(platform, client);
    client.get_http_client().set_host(server.host());
    client.get_http_client().set_origin(server.host());
    client.get_http_client().set_referer(server.host() + "/");
    request_manager.set_auth_credentials("fixture-user", "fixture-hash", "PHPSESSID=fixture");

    const auto first = time_shield::to_timestamp(2026, 10, 28, 9, 0, 0);
    std::atomic<bool> completed{false};
    QuoteCheckApiResult result;
    ASSERT_TRUE(request_manager.request_quote_check_result(
        make_request(
            time_shield::sec_to_ms(first),
            time_shield::sec_to_ms(first + MAX_INTRADE_QUOTE_CHECK_INTERVAL_SEC + 1)),
        [&completed, &result](QuoteCheckApiResult value) {
            result = std::move(value);
            completed.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        client.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.load());
    ASSERT_TRUE(result.success);
    const auto queries = server.queries();
    ASSERT_EQ(queries.size(), 2U);
    EXPECT_NE(queries[0].find("from=12%3A00%3A00"), std::string::npos);
    EXPECT_NE(queries[0].find("to=12%3A30%3A00"), std::string::npos);
    EXPECT_NE(queries[1].find("from=12%3A30%3A01"), std::string::npos);
    EXPECT_NE(queries[1].find("to=12%3A30%3A01"), std::string::npos);
    platform.shutdown();
}

TEST(IntradeQuoteCheckParser, FailsWhenSecondSegmentReturnsUnrecognizedHtml) {
    QuoteCheckServerFixture server;
    ASSERT_TRUE(server.start());
    server.set_response_sequence({
        R"HTML(<td class="partner_stat_table_left">12:00:00</td><td class="">1.19990</td><td class="">1.20000</td><td class="">1.20010</td>)HTML",
        "<html><body>Temporarily unavailable</body></html>"});

    QuoteCheckTestPlatform platform;
    HttpClientComponent client(platform);
    RequestManager request_manager(platform, client);
    client.get_http_client().set_host(server.host());
    request_manager.set_auth_credentials("fixture-user", "fixture-hash", "PHPSESSID=fixture");

    const auto first = time_shield::to_timestamp(2026, 10, 28, 9, 0, 0);
    std::atomic<bool> completed{false};
    QuoteCheckApiResult result;
    ASSERT_TRUE(request_manager.request_quote_check_result(
        make_request(
            time_shield::sec_to_ms(first),
            time_shield::sec_to_ms(first + 60 * time_shield::SEC_PER_MIN)),
        [&completed, &result](QuoteCheckApiResult value) {
            result = std::move(value);
            completed.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        client.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.load());
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("format was not recognized"), std::string::npos);
    EXPECT_EQ(server.queries().size(), 2U);
    platform.shutdown();
}

TEST(IntradeQuoteCheckParser, PlatformReturnsBrokerHistoryThroughUnifiedApi) {
    QuoteCheckServerFixture server;
    ASSERT_TRUE(server.start());

    optionx::platforms::IntradeBarPlatform platform;
    auto auth = std::make_unique<AuthData>();
    auth->set_email_password("fixture@example.test", "fixture-password");
    auth->host = server.host();
    auth->auto_find_domain = false;
    ASSERT_TRUE(platform.configure_auth(std::move(auth)));
    platform.event_bus().drain();
    platform.run(false);

    const auto time_sec = time_shield::to_timestamp(2026, 10, 28, 20, 59, 59);
    const TickHistoryRequest request(
        "EUR/USD",
        time_shield::sec_to_ms(time_sec),
        time_shield::sec_to_ms(time_sec),
        TickHistorySource::BROKER_HISTORICAL);
    TickHistoryResult result;
    std::atomic<bool> completed{false};
    ASSERT_TRUE(platform.fetch_tick_history(
        request,
        [&result, &completed](TickHistoryResult update) {
            result = std::move(update);
            completed.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        platform.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.load());
    ASSERT_TRUE(result.success) << result.error_desc;
    EXPECT_TRUE(result.range_complete);
    EXPECT_EQ(result.sequence.symbol, "EURUSD");
    EXPECT_EQ(result.sequence.provider, "INTRADE_BAR");
    EXPECT_EQ(result.sequence.source, "BROKER_HISTORICAL");
    EXPECT_EQ(result.sequence.price_digits, 5U);
    EXPECT_EQ(result.sequence.volume_digits, 0U);
    ASSERT_EQ(result.sequence.ticks.size(), 1U);
    EXPECT_DOUBLE_EQ(result.sequence.ticks.front().reference, 1.20000);
    EXPECT_DOUBLE_EQ(result.sequence.ticks.front().ask, 0.0);
    EXPECT_DOUBLE_EQ(result.sequence.ticks.front().bid, 0.0);
    EXPECT_DOUBLE_EQ(result.sequence.ticks.front().last, 0.0);
    platform.shutdown();
}

TEST(IntradeQuoteCheckParser, RequestManagerRejectsHttp200LoginPage) {
    QuoteCheckServerFixture server;
    ASSERT_TRUE(server.start());
    server.set_response(
        "<title>Digital options, without the circus.</title>"
        "<script>window.__NUXT__={}</script>");

    QuoteCheckTestPlatform platform;
    HttpClientComponent client(platform);
    RequestManager request_manager(platform, client);
    client.get_http_client().set_host(server.host());
    request_manager.set_auth_credentials("fixture-user", "fixture-hash", "PHPSESSID=fixture");

    const auto time_sec = time_shield::to_timestamp(2026, 10, 28, 9, 0, 0);
    QuoteCheckApiResult result;
    std::atomic<bool> completed{false};
    ASSERT_TRUE(request_manager.request_quote_check_result(
        make_request(time_shield::sec_to_ms(time_sec), time_shield::sec_to_ms(time_sec)),
        [&result, &completed](QuoteCheckApiResult update) {
            result = std::move(update);
            completed.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        client.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.load());
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("authentication expired"), std::string::npos);
    platform.shutdown();
}

TEST(IntradeQuoteCheckParser, RequestManagerRejectsUnrecognizedHttp200Html) {
    QuoteCheckServerFixture server;
    ASSERT_TRUE(server.start());
    server.set_response("<html><body>Temporarily unavailable</body></html>");

    QuoteCheckTestPlatform platform;
    HttpClientComponent client(platform);
    RequestManager request_manager(platform, client);
    client.get_http_client().set_host(server.host());
    request_manager.set_auth_credentials("fixture-user", "fixture-hash", "PHPSESSID=fixture");

    const auto time_sec = time_shield::to_timestamp(2026, 10, 28, 9, 0, 0);
    QuoteCheckApiResult result;
    std::atomic<bool> completed{false};
    ASSERT_TRUE(request_manager.request_quote_check_result(
        make_request(time_shield::sec_to_ms(time_sec), time_shield::sec_to_ms(time_sec)),
        [&result, &completed](QuoteCheckApiResult update) {
            result = std::move(update);
            completed.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        client.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.load());
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("format was not recognized"), std::string::npos);
    platform.shutdown();
}

TEST(IntradeQuoteCheckParser, RequestManagerRejectsFuturePeriodWithBrokerReason) {
    QuoteCheckServerFixture server;
    ASSERT_TRUE(server.start());
    server.set_response(
        "<form><select name=\"symbol\"></select><input name=\"date\">"
        "<input name=\"from\"><input name=\"to\"></form>"
        "<p>This period has not ended yet. Choose a time in the past.</p>");

    QuoteCheckTestPlatform platform;
    HttpClientComponent client(platform);
    RequestManager request_manager(platform, client);
    client.get_http_client().set_host(server.host());
    request_manager.set_auth_credentials("fixture-user", "fixture-hash", "PHPSESSID=fixture");

    const auto time_sec = time_shield::to_timestamp(2026, 10, 28, 9, 0, 0);
    QuoteCheckApiResult result;
    std::atomic<bool> completed{false};
    ASSERT_TRUE(request_manager.request_quote_check_result(
        make_request(time_shield::sec_to_ms(time_sec), time_shield::sec_to_ms(time_sec)),
        [&result, &completed](QuoteCheckApiResult update) {
            result = std::move(update);
            completed.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        client.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.load());
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("period has not ended"), std::string::npos);
    platform.shutdown();
}

TEST(IntradeQuoteCheckParser, RequestManagerAcceptsEmptyHttp200AsIncompleteHistory) {
    QuoteCheckServerFixture server;
    ASSERT_TRUE(server.start());
    server.set_response({});

    QuoteCheckTestPlatform platform;
    HttpClientComponent client(platform);
    RequestManager request_manager(platform, client);
    client.get_http_client().set_host(server.host());
    request_manager.set_auth_credentials("fixture-user", "fixture-hash", "PHPSESSID=fixture");

    const auto time_sec = time_shield::to_timestamp(2026, 10, 28, 9, 0, 0);
    QuoteCheckApiResult result;
    std::atomic<bool> completed{false};
    ASSERT_TRUE(request_manager.request_quote_check_result(
        make_request(time_shield::sec_to_ms(time_sec), time_shield::sec_to_ms(time_sec)),
        [&result, &completed](QuoteCheckApiResult update) {
            result = std::move(update);
            completed.store(true);
        }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
        client.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.load());
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.value.observations.empty());
    EXPECT_FALSE(result.value.range_complete);
    platform.shutdown();
}

TEST(IntradeQuoteCheckParser, RequestManagerRejectsMoreThanTwoBrokerDates) {
    QuoteCheckTestPlatform platform;
    HttpClientComponent client(platform);
    RequestManager request_manager(platform, client);
    const auto first = time_shield::to_timestamp(2026, 10, 28, 9, 0, 0);
    QuoteCheckApiResult result;
    int callback_count = 0;

    EXPECT_FALSE(request_manager.request_quote_check_result(
        make_request(
            time_shield::sec_to_ms(first),
            time_shield::sec_to_ms(first + 2 * time_shield::SEC_PER_DAY)),
        [&result, &callback_count](QuoteCheckApiResult update) {
            result = std::move(update);
            ++callback_count;
        }));

    EXPECT_EQ(callback_count, 1);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("two-day limit"), std::string::npos);
    platform.shutdown();
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
