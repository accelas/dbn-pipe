// SPDX-License-Identifier: MIT

// tests/http_client_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/stream/http_client.hpp"
#include "dbn_pipe/stream/component.hpp"

using namespace dbn_pipe;

// Mock downstream that receives body data
struct MockHttpDownstream {
    std::vector<std::byte> body;
    Error last_error;
    bool done = false;
    bool error_called = false;

    void OnData(BufferChain& chain) {
        while (!chain.Empty()) {
            size_t chunk_size = chain.ContiguousSize();
            const std::byte* ptr = chain.DataAt(0);
            body.insert(body.end(), ptr, ptr + chunk_size);
            chain.Consume(chunk_size);
        }
    }
    void OnError(const Error& e) {
        last_error = e;
        error_called = true;
    }
    void OnDone() { done = true; }
};

// Verify MockHttpDownstream satisfies Downstream concept
static_assert(Downstream<MockHttpDownstream>);

// Helper to create BufferChain from string
BufferChain ToChain(const std::string& str) {
    BufferChain chain;
    auto seg = std::make_shared<Segment>();
    std::memcpy(seg->data.data(), str.data(), str.size());
    seg->size = str.size();
    chain.Append(std::move(seg));
    return chain;
}

TEST(HttpClientTest, FactoryCreatesInstance) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();

    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);
    ASSERT_NE(http, nullptr);
}

TEST(HttpClientTest, ParsesSuccessResponse) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_EQ(downstream->body.size(), 5);
    EXPECT_TRUE(downstream->done);
}

TEST(HttpClientTest, ParsesChunkedResponse) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    // Send response in chunks
    std::string part1 = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
    std::string part2 = "world";

    auto chain1 = ToChain(part1);
    http->OnData(chain1);

    EXPECT_EQ(downstream->body.size(), 5);
    EXPECT_FALSE(downstream->done);  // Not complete yet

    auto chain2 = ToChain(part2);
    http->OnData(chain2);

    EXPECT_EQ(downstream->body.size(), 10);
    EXPECT_TRUE(downstream->done);
}

TEST(HttpClientTest, HandlesHttpError400) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "Bad Request";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::HttpError);
    EXPECT_TRUE(downstream->last_error.message.find("400") != std::string::npos);
    EXPECT_TRUE(downstream->last_error.message.find("Bad Request") != std::string::npos);
    // Body should NOT be forwarded for error responses
    EXPECT_TRUE(downstream->body.empty());
}

TEST(HttpClientTest, HandlesHttpError500) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Length: 6\r\n"
        "\r\n"
        "failed";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::ServerError);  // 5xx -> ServerError
    EXPECT_TRUE(downstream->last_error.message.find("500") != std::string::npos);
}

TEST(HttpClientTest, HandlesRedirect301AsError) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: https://example.com/new\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    auto chain = ToChain(response);
    http->OnData(chain);

    // Redirects (status >= 300) are treated as errors per the design
    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::HttpError);
    EXPECT_TRUE(downstream->last_error.message.find("301") != std::string::npos);
}

TEST(HttpClientTest, SuspendAndResumeWork) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    // Initialize event loop thread ID for Suspend/Resume assertions
    loop.Poll(0);

    // Initially not suspended
    EXPECT_FALSE(http->IsSuspended());

    http->Suspend();
    EXPECT_TRUE(http->IsSuspended());

    http->Resume();
    EXPECT_FALSE(http->IsSuspended());
}

TEST(HttpClientTest, CloseCallsDoClose) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    http->Close();
    EXPECT_TRUE(http->IsClosed());
}

TEST(HttpClientTest, EmptyBodyResponse) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 204 No Content\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_TRUE(downstream->body.empty());
    EXPECT_TRUE(downstream->done);
}

TEST(HttpClientTest, StatusCodeAccessor) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    // Send just headers to check status code before message complete
    std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n";

    auto chain1 = ToChain(headers);
    http->OnData(chain1);

    // After headers, status code should be set (but may be reset on message complete)
    // We verify by sending the complete response and checking final state
    std::string body = "hello";
    auto chain2 = ToChain(body);
    http->OnData(chain2);

    // After message complete, state is reset for keep-alive
    EXPECT_EQ(http->StatusCode(), 0);
    EXPECT_FALSE(http->IsMessageComplete());  // Reset after completion
}

TEST(HttpClientTest, ImplementsUpstreamConcept) {
    // HttpClient must satisfy Upstream concept for pipeline integration
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    // Verify Upstream interface is available
    static_assert(Upstream<HttpClient<MockHttpDownstream>>);
    SUCCEED();
}

// Tests for HTTP status code to ErrorCode mapping

TEST(HttpClientTest, Maps401ToUnauthorized) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "Invalid key";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::Unauthorized);
}

TEST(HttpClientTest, Maps403ToUnauthorized) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Length: 9\r\n"
        "\r\n"
        "Forbidden";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::Unauthorized);
}

TEST(HttpClientTest, Maps404ToNotFound) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 9\r\n"
        "\r\n"
        "Not found";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::NotFound);
}

TEST(HttpClientTest, Maps422ToValidationError) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 422 Unprocessable Entity\r\n"
        "Content-Length: 14\r\n"
        "\r\n"
        "Invalid params";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::ValidationError);
}

TEST(HttpClientTest, Maps429ToRateLimited) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Content-Length: 12\r\n"
        "\r\n"
        "Rate limited";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::RateLimited);
}

TEST(HttpClientTest, ParsesRetryAfterHeader) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Retry-After: 30\r\n"
        "Content-Length: 12\r\n"
        "\r\n"
        "Rate limited";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::RateLimited);
    ASSERT_TRUE(downstream->last_error.retry_after.has_value());
    EXPECT_EQ(downstream->last_error.retry_after->count(), 30000);  // 30 seconds in ms
}

TEST(HttpClientTest, ParsesRetryAfterHeaderCaseInsensitive) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(loop, downstream);

    std::string response =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "retry-after: 60\r\n"
        "Content-Length: 12\r\n"
        "\r\n"
        "Rate limited";

    auto chain = ToChain(response);
    http->OnData(chain);

    EXPECT_TRUE(downstream->error_called);
    ASSERT_TRUE(downstream->last_error.retry_after.has_value());
    EXPECT_EQ(downstream->last_error.retry_after->count(), 60000);  // 60 seconds in ms
}
