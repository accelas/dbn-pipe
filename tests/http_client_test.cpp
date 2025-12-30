// tests/http_client_test.cpp
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "src/http_client.hpp"
#include "src/pipeline.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

// Mock downstream that receives body data
struct MockHttpDownstream {
    std::vector<std::byte> body;
    Error last_error;
    bool done = false;
    bool error_called = false;

    void Read(std::pmr::vector<std::byte> data) {
        body.insert(body.end(), data.begin(), data.end());
    }
    void OnError(const Error& e) {
        last_error = e;
        error_called = true;
    }
    void OnDone() { done = true; }
};

// Verify MockHttpDownstream satisfies Downstream concept
static_assert(Downstream<MockHttpDownstream>);

TEST(HttpClientTest, FactoryCreatesInstance) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();

    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);
    ASSERT_NE(http, nullptr);
}

TEST(HttpClientTest, ParsesSuccessResponse) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    std::pmr::vector<std::byte> data;
    for (char c : response) {
        data.push_back(static_cast<std::byte>(c));
    }

    http->Read(std::move(data));

    EXPECT_EQ(downstream->body.size(), 5);
    EXPECT_TRUE(downstream->done);
}

TEST(HttpClientTest, ParsesChunkedResponse) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    // Send response in chunks
    std::string part1 = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
    std::string part2 = "world";

    std::pmr::vector<std::byte> data1;
    for (char c : part1) {
        data1.push_back(static_cast<std::byte>(c));
    }
    http->Read(std::move(data1));

    EXPECT_EQ(downstream->body.size(), 5);
    EXPECT_FALSE(downstream->done);  // Not complete yet

    std::pmr::vector<std::byte> data2;
    for (char c : part2) {
        data2.push_back(static_cast<std::byte>(c));
    }
    http->Read(std::move(data2));

    EXPECT_EQ(downstream->body.size(), 10);
    EXPECT_TRUE(downstream->done);
}

TEST(HttpClientTest, HandlesHttpError400) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    std::string response =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "Bad Request";

    std::pmr::vector<std::byte> data;
    for (char c : response) {
        data.push_back(static_cast<std::byte>(c));
    }

    http->Read(std::move(data));

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::HttpError);
    EXPECT_TRUE(downstream->last_error.message.find("400") != std::string::npos);
    EXPECT_TRUE(downstream->last_error.message.find("Bad Request") != std::string::npos);
    // Body should NOT be forwarded for error responses
    EXPECT_TRUE(downstream->body.empty());
}

TEST(HttpClientTest, HandlesHttpError500) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    std::string response =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Length: 6\r\n"
        "\r\n"
        "failed";

    std::pmr::vector<std::byte> data;
    for (char c : response) {
        data.push_back(static_cast<std::byte>(c));
    }

    http->Read(std::move(data));

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::HttpError);
    EXPECT_TRUE(downstream->last_error.message.find("500") != std::string::npos);
}

TEST(HttpClientTest, HandlesRedirect301AsError) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    std::string response =
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: https://example.com/new\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    std::pmr::vector<std::byte> data;
    for (char c : response) {
        data.push_back(static_cast<std::byte>(c));
    }

    http->Read(std::move(data));

    // Redirects (status >= 300) are treated as errors per the design
    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::HttpError);
    EXPECT_TRUE(downstream->last_error.message.find("301") != std::string::npos);
}

TEST(HttpClientTest, SuspendAndResumeWork) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    // Initialize reactor thread ID for Suspend/Resume assertions
    reactor.Poll(0);

    // Initially not suspended
    EXPECT_FALSE(http->IsSuspended());

    http->Suspend();
    EXPECT_TRUE(http->IsSuspended());

    http->Resume();
    EXPECT_FALSE(http->IsSuspended());
}

TEST(HttpClientTest, CloseCallsDoClose) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    http->Close();
    EXPECT_TRUE(http->IsClosed());
}

TEST(HttpClientTest, EmptyBodyResponse) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    std::string response =
        "HTTP/1.1 204 No Content\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    std::pmr::vector<std::byte> data;
    for (char c : response) {
        data.push_back(static_cast<std::byte>(c));
    }

    http->Read(std::move(data));

    EXPECT_TRUE(downstream->body.empty());
    EXPECT_TRUE(downstream->done);
}

TEST(HttpClientTest, StatusCodeAccessor) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    // Send just headers to check status code before message complete
    std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n";

    std::pmr::vector<std::byte> data;
    for (char c : headers) {
        data.push_back(static_cast<std::byte>(c));
    }

    http->Read(std::move(data));

    // After headers, status code should be set (but may be reset on message complete)
    // We verify by sending the complete response and checking final state
    std::string body = "hello";
    std::pmr::vector<std::byte> bodyData;
    for (char c : body) {
        bodyData.push_back(static_cast<std::byte>(c));
    }
    http->Read(std::move(bodyData));

    // After message complete, state is reset for keep-alive
    EXPECT_EQ(http->StatusCode(), 0);
    EXPECT_FALSE(http->IsMessageComplete());  // Reset after completion
}

TEST(HttpClientTest, ImplementsUpstreamConcept) {
    // HttpClient must satisfy Upstream concept for pipeline integration
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    // Verify Upstream interface is available
    static_assert(Upstream<HttpClient<MockHttpDownstream>>);
    SUCCEED();
}
