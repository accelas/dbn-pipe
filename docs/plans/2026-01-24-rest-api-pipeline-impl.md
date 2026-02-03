# RestApiPipeline Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a coroutine-based REST API pipeline that composes with dbWriter's BatchWriter on the same `io_context`.

**Architecture:** RestApiPipeline wraps the existing callback-based HTTP stack (ApiProtocol) and exposes an `asio::awaitable<>` interface. Core library gets `PathTemplate()` for URL path parameter substitution.

**Tech Stack:** C++23, ASIO coroutines, existing dbn-pipe HTTP/JSON infrastructure, Bazel build

---

## Task 1: Add PathTemplate to HttpRequestBuilder

**Files:**
- Modify: `lib/stream/http_request_builder.hpp:46-50`
- Test: `tests/http_request_builder_test.cpp`

**Step 1: Write the failing test**

Add to `tests/http_request_builder_test.cpp`:

```cpp
TEST(HttpRequestBuilderTest, PathTemplateSubstitution) {
    std::string out;
    std::vector<std::pair<std::string, std::string>> params = {
        {"ticker", "AAPL"},
        {"start", "2024-01-01"},
        {"end", "2024-01-31"}
    };

    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .PathTemplate("/v2/aggs/ticker/{ticker}/range/1/day/{start}/{end}", params)
        .Host("api.polygon.io")
        .Finish();

    std::string expected =
        "GET /v2/aggs/ticker/AAPL/range/1/day/2024-01-01/2024-01-31 HTTP/1.1\r\n"
        "Host: api.polygon.io\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, PathTemplateNoParams) {
    std::string out;
    std::vector<std::pair<std::string, std::string>> params;

    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .PathTemplate("/v0/data", params)
        .Host("example.com")
        .Finish();

    std::string expected =
        "GET /v0/data HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, PathTemplateMissingParam) {
    std::string out;
    std::vector<std::pair<std::string, std::string>> params = {
        {"ticker", "AAPL"}
        // "date" is missing
    };

    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .PathTemplate("/v2/ticker/{ticker}/date/{date}", params)
        .Host("example.com")
        .Finish();

    // Missing param results in empty substitution
    std::string expected =
        "GET /v2/ticker/AAPL/date/ HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, PathTemplateWithQueryParams) {
    std::string out;
    std::vector<std::pair<std::string, std::string>> path_params = {
        {"symbol", "SPY"}
    };

    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .PathTemplate("/v1/quote/{symbol}", path_params)
        .QueryParam("apiKey", "test123")
        .Host("api.example.com")
        .Finish();

    std::string expected =
        "GET /v1/quote/SPY?apiKey=test123 HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:http_request_builder_test --test_filter='*PathTemplate*' --test_output=errors`
Expected: FAIL with compilation error "no member named 'PathTemplate'"

**Step 3: Write minimal implementation**

Add to `lib/stream/http_request_builder.hpp` after the `Path()` method (around line 50):

```cpp
    // Set request path with parameter substitution
    // Template syntax: {name} is replaced with corresponding value from params
    // Missing params result in empty substitution
    HttpRequestBuilder& PathTemplate(
        std::string_view path_template,
        std::span<const std::pair<std::string, std::string>> params
    ) {
        *out_++ = ' ';

        size_t pos = 0;
        while (pos < path_template.size()) {
            size_t brace = path_template.find('{', pos);
            if (brace == std::string_view::npos) {
                out_ = fmt::format_to(out_, "{}", path_template.substr(pos));
                break;
            }

            // Copy literal part before placeholder
            if (brace > pos) {
                out_ = fmt::format_to(out_, "{}", path_template.substr(pos, brace - pos));
            }

            // Find closing brace
            size_t end_brace = path_template.find('}', brace);
            if (end_brace == std::string_view::npos) {
                // Malformed template, copy rest as-is
                out_ = fmt::format_to(out_, "{}", path_template.substr(brace));
                break;
            }

            // Extract param name and substitute
            auto name = path_template.substr(brace + 1, end_brace - brace - 1);
            for (const auto& [key, value] : params) {
                if (key == name) {
                    out_ = fmt::format_to(out_, "{}", value);
                    break;
                }
            }

            pos = end_brace + 1;
        }

        return *this;
    }
```

Also add `#include <span>` to the includes if not present.

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:http_request_builder_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add lib/stream/http_request_builder.hpp tests/http_request_builder_test.cpp
git commit -m "feat(http): add PathTemplate for URL path parameter substitution"
```

---

## Task 2: Add path_params to ApiRequest

**Files:**
- Modify: `src/api_protocol.hpp:28-67`
- Test: `tests/api_protocol_test.cpp` (new file)

**Step 1: Write the failing test**

Create `tests/api_protocol_test.cpp`:

```cpp
// SPDX-License-Identifier: MIT

// tests/api_protocol_test.cpp
#include <gtest/gtest.h>

#include "dbn_pipe/api_protocol.hpp"

namespace dbn_pipe {
namespace {

TEST(ApiRequestTest, BuildHttpRequestWithPathParams) {
    ApiRequest req{
        .method = "GET",
        .path = "/v2/aggs/ticker/{ticker}/range/1/day/{start}/{end}",
        .host = "api.polygon.io",
        .port = 443,
        .path_params = {{"ticker", "AAPL"}, {"start", "2024-01-01"}, {"end", "2024-01-31"}},
        .query_params = {{"apiKey", "test123"}},
    };

    std::string result = req.BuildHttpRequest("api.polygon.io", "my_api_key");

    EXPECT_TRUE(result.find("GET /v2/aggs/ticker/AAPL/range/1/day/2024-01-01/2024-01-31?") != std::string::npos);
    EXPECT_TRUE(result.find("apiKey=test123") != std::string::npos);
    EXPECT_TRUE(result.find("Host: api.polygon.io") != std::string::npos);
}

TEST(ApiRequestTest, BuildHttpRequestWithoutPathParams) {
    ApiRequest req{
        .method = "GET",
        .path = "/v0/metadata",
        .host = "hist.databento.com",
        .port = 443,
        .query_params = {{"dataset", "GLBX.MDP3"}},
    };

    std::string result = req.BuildHttpRequest("hist.databento.com", "my_api_key");

    EXPECT_TRUE(result.find("GET /v0/metadata?") != std::string::npos);
    EXPECT_TRUE(result.find("dataset=GLBX.MDP3") != std::string::npos);
}

TEST(ApiRequestTest, BuildHttpRequestEmptyPathParams) {
    ApiRequest req{
        .method = "GET",
        .path = "/v0/data",
        .host = "example.com",
        .port = 443,
        .path_params = {},
        .query_params = {},
    };

    std::string result = req.BuildHttpRequest("example.com", "key");

    EXPECT_TRUE(result.find("GET /v0/data HTTP/1.1") != std::string::npos);
}

}  // namespace
}  // namespace dbn_pipe
```

**Step 2: Add test to BUILD.bazel**

Add to `tests/BUILD.bazel`:

```python
cc_test(
    name = "api_protocol_test",
    srcs = ["api_protocol_test.cpp"],
    deps = [
        "//src:client",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:api_protocol_test --test_output=errors`
Expected: FAIL with compilation error about path_params not being a member

**Step 4: Write minimal implementation**

Modify `src/api_protocol.hpp`. Update the `ApiRequest` struct:

```cpp
struct ApiRequest {
    std::string method = "GET";
    std::string path;
    std::string host = "hist.databento.com";
    uint16_t port = 443;
    std::vector<std::pair<std::string, std::string>> path_params;    // NEW
    std::vector<std::pair<std::string, std::string>> query_params;
    std::vector<std::pair<std::string, std::string>> form_params;  // For POST

    // Build HTTP request string for this request
    // host_header: Host header value (e.g., "hist.databento.com")
    // api_key: API key for Basic auth (encoded as base64(api_key + ":"))
    std::string BuildHttpRequest(const std::string& host_header,
                                 const std::string& api_key) const {
        // Estimate: method + path + query params + headers typically 200-500 bytes
        std::string out;
        out.reserve(512);

        auto builder = HttpRequestBuilder(std::back_inserter(out))
            .Method(method);

        // Use PathTemplate if path_params provided, else plain Path
        if (path_params.empty()) {
            builder.Path(path);
        } else {
            builder.PathTemplate(path, path_params);
        }

        for (const auto& [key, value] : query_params) {
            builder.QueryParam(key, value);
        }

        builder
            .Host(host_header)
            .BasicAuth(api_key)
            .Header("Accept", "application/json")
            .Header("Connection", "close");

        if (method == "POST" && !form_params.empty()) {
            builder.FormBody(form_params);
        } else {
            builder.Finish();
        }

        return out;
    }
};
```

**Step 5: Run test to verify it passes**

Run: `bazel test //tests:api_protocol_test --test_output=errors`
Expected: PASS

**Step 6: Run all tests to verify no regressions**

Run: `bazel test //...`
Expected: All tests pass

**Step 7: Commit**

```bash
git add src/api_protocol.hpp tests/api_protocol_test.cpp tests/BUILD.bazel
git commit -m "feat(api): add path_params to ApiRequest for URL path substitution"
```

---

## Task 3: Create example/rest_api directory structure and BUILD.bazel

**Files:**
- Create: `example/rest_api/BUILD.bazel`
- Create: `example/rest_api/include/rest_api/.gitkeep` (placeholder)

**Step 1: Create directory structure**

```bash
mkdir -p example/rest_api/include/rest_api
mkdir -p example/rest_api/test
```

**Step 2: Create BUILD.bazel**

Create `example/rest_api/BUILD.bazel`:

```python
load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "rest_api",
    hdrs = glob(["include/rest_api/*.hpp"]),
    includes = ["include"],
    deps = [
        "//example/dbwriter",
        "//lib/stream:event_loop",
        "//lib/stream:http_client",
        "//lib/stream:json_parser",
        "//lib/stream:tcp_socket",
        "//lib/stream:tls_transport",
        "//src:client",
        "@asio",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "rest_api_pipeline_test",
    srcs = ["test/rest_api_pipeline_test.cpp"],
    deps = [
        ":rest_api",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Verify build file is valid**

Run: `bazel build //example/rest_api:rest_api 2>&1 | head -20`
Expected: Warning about no sources (expected at this stage), no syntax errors

**Step 4: Commit**

```bash
git add example/rest_api/BUILD.bazel
git commit -m "build: add example/rest_api directory structure"
```

---

## Task 4: Implement RestApiPipeline class

**Files:**
- Create: `example/rest_api/include/rest_api/rest_api_pipeline.hpp`
- Test: `example/rest_api/test/rest_api_pipeline_test.cpp`

**Step 1: Write the failing test**

Create `example/rest_api/test/rest_api_pipeline_test.cpp`:

```cpp
// SPDX-License-Identifier: MIT

// example/rest_api/test/rest_api_pipeline_test.cpp
#include <gtest/gtest.h>

#include <expected>
#include <string>

#include "rest_api/rest_api_pipeline.hpp"

namespace rest_api {
namespace {

// Simple test builder that extracts a "value" field from JSON
struct TestBuilder {
    using Result = std::string;
    std::string value;
    std::string current_key;

    void OnKey(std::string_view key) { current_key = key; }
    void OnString(std::string_view v) {
        if (current_key == "value") value = v;
    }
    void OnInt(int64_t) {}
    void OnUint(uint64_t) {}
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (value.empty()) return std::unexpected("missing value field");
        return value;
    }
};

TEST(RestApiPipelineTest, ConstructsWithHostAndPort) {
    asio::io_context ctx;
    RestApiPipeline<TestBuilder> pipeline(ctx, "api.example.com", 443);
    // Should compile and construct without error
    SUCCEED();
}

TEST(RestApiPipelineTest, ConstructsWithDefaultPort) {
    asio::io_context ctx;
    RestApiPipeline<TestBuilder> pipeline(ctx, "api.example.com");
    // Default port should be 443
    SUCCEED();
}

TEST(RestApiPipelineTest, SetApiKey) {
    asio::io_context ctx;
    RestApiPipeline<TestBuilder> pipeline(ctx, "api.example.com");
    pipeline.set_api_key("test_key");
    // Should compile and set key without error
    SUCCEED();
}

// Note: Full fetch() tests require network mocking or integration tests
// These basic tests verify the interface compiles correctly

}  // namespace
}  // namespace rest_api
```

**Step 2: Run test to verify it fails**

Run: `bazel test //example/rest_api:rest_api_pipeline_test --test_output=errors`
Expected: FAIL with "no such file" for rest_api_pipeline.hpp

**Step 3: Write minimal implementation**

Create `example/rest_api/include/rest_api/rest_api_pipeline.hpp`:

```cpp
// SPDX-License-Identifier: MIT

// example/rest_api/include/rest_api/rest_api_pipeline.hpp
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "dbwriter/asio_event_loop.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/json_parser.hpp"
#include "dbn_pipe/stream/pipeline.hpp"
#include "dbn_pipe/api_protocol.hpp"
#include "dbn_pipe/dns_resolver.hpp"

namespace rest_api {

// RestApiPipeline - Coroutine-based REST API fetcher
//
// Fetches JSON from REST APIs and returns parsed results as an awaitable.
// Designed to compose with dbwriter::BatchWriter on the same io_context.
//
// Usage:
//   RestApiPipeline<MyBuilder> api(ctx, "api.example.com");
//   api.set_api_key("my_key");
//   auto result = co_await api.fetch("/v1/data/{id}", {{"id", "123"}}, {{"format", "json"}});
//
// Template parameter Builder must satisfy the JsonBuilder concept.
template<dbn_pipe::JsonBuilder Builder>
class RestApiPipeline {
public:
    using Result = typename Builder::Result;
    using Params = std::vector<std::pair<std::string, std::string>>;

    RestApiPipeline(asio::io_context& ctx,
                    std::string host,
                    uint16_t port = 443)
        : ctx_(ctx)
        , loop_(ctx)
        , host_(std::move(host))
        , port_(port) {}

    void set_api_key(std::string key) { api_key_ = std::move(key); }

    // Fetch single URL, parse JSON, return result
    // path_template: path with {name} placeholders (e.g., "/v1/data/{id}")
    // path_params: substituted into path template (e.g., {{"id", "123"}})
    // query_params: appended as ?key=value&... (e.g., {{"format", "json"}})
    asio::awaitable<std::expected<Result, dbn_pipe::Error>> fetch(
        std::string path_template,
        Params path_params = {},
        Params query_params = {}
    ) {
        // Use a completion token to bridge callback to coroutine
        auto result = co_await asio::async_initiate<
            decltype(asio::use_awaitable),
            void(std::expected<Result, dbn_pipe::Error>)
        >(
            [this, path = std::move(path_template),
             pp = std::move(path_params),
             qp = std::move(query_params)]
            (auto handler) mutable {
                do_fetch(std::move(path), std::move(pp), std::move(qp),
                         std::move(handler));
            },
            asio::use_awaitable
        );

        co_return result;
    }

private:
    template<typename Handler>
    void do_fetch(std::string path, Params path_params,
                  Params query_params, Handler handler) {
        auto addr = dbn_pipe::ResolveHostname(host_, port_);
        if (!addr) {
            asio::post(ctx_, [h = std::move(handler)]() mutable {
                h(std::unexpected(dbn_pipe::Error{
                    dbn_pipe::ErrorCode::DnsResolutionFailed,
                    "Failed to resolve hostname"}));
            });
            return;
        }

        // Create builder (shared to outlive callback chain)
        auto builder = std::make_shared<Builder>();

        using Protocol = dbn_pipe::ApiProtocol<Builder>;
        using SinkType = typename Protocol::SinkType;
        using PipelineType = dbn_pipe::Pipeline<Protocol>;

        // Pipeline holder prevents destruction during callback
        auto pipeline_holder = std::make_shared<std::shared_ptr<PipelineType>>();

        // Handler wrapper to ensure single invocation
        auto handler_ptr = std::make_shared<Handler>(std::move(handler));
        auto called = std::make_shared<bool>(false);

        auto sink = std::make_shared<SinkType>(
            [this, handler_ptr, called, pipeline_holder, builder](auto result) mutable {
                if (*called) return;
                *called = true;
                // Move handler out and invoke
                auto h = std::move(*handler_ptr);
                // Prevent destruction on own call stack
                ctx_.post([pipeline_holder]() { pipeline_holder->reset(); });
                h(std::move(result));
            });

        auto chain = Protocol::BuildChain(loop_, *sink, api_key_);
        chain->SetBuilder(*builder);
        chain->SetHost(host_);

        dbn_pipe::ApiRequest req{
            .method = "GET",
            .path = std::move(path),
            .host = host_,
            .port = port_,
            .path_params = std::move(path_params),
            .query_params = std::move(query_params),
        };

        *pipeline_holder = std::make_shared<PipelineType>(
            typename PipelineType::PrivateTag{},
            loop_, chain, sink, req);

        std::weak_ptr<PipelineType> weak_pipeline = *pipeline_holder;
        chain->SetReadyCallback([weak_pipeline]() {
            if (auto p = weak_pipeline.lock()) {
                p->MarkReady();
                p->Start();
            }
        });

        (*pipeline_holder)->Connect(*addr);
    }

    asio::io_context& ctx_;
    dbwriter::AsioEventLoop loop_;
    std::string host_;
    uint16_t port_;
    std::string api_key_;
};

}  // namespace rest_api
```

**Step 4: Run test to verify it passes**

Run: `bazel test //example/rest_api:rest_api_pipeline_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add example/rest_api/include/rest_api/rest_api_pipeline.hpp example/rest_api/test/rest_api_pipeline_test.cpp
git commit -m "feat(rest_api): implement RestApiPipeline with awaitable fetch()"
```

---

## Task 5: Add integration test with mock server

**Files:**
- Modify: `example/rest_api/test/rest_api_pipeline_test.cpp`

**Step 1: Write the integration test**

Add to `example/rest_api/test/rest_api_pipeline_test.cpp`:

```cpp
// Integration test using localhost echo server pattern
// This tests the full fetch flow without external network

TEST(RestApiPipelineTest, FetchReturnsErrorForUnreachableHost) {
    asio::io_context ctx;
    RestApiPipeline<TestBuilder> pipeline(ctx, "localhost", 1);  // Port 1 should fail

    bool completed = false;
    std::expected<std::string, dbn_pipe::Error> result_holder;

    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        auto result = co_await pipeline.fetch("/test", {}, {});
        result_holder = std::move(result);
        completed = true;
    }, asio::detached);

    // Run until completion or timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!completed && std::chrono::steady_clock::now() < deadline) {
        ctx.poll();
    }

    ASSERT_TRUE(completed) << "Fetch did not complete within timeout";
    ASSERT_FALSE(result_holder.has_value()) << "Expected error for unreachable host";
}

TEST(RestApiPipelineTest, FetchReturnsErrorForInvalidHost) {
    asio::io_context ctx;
    RestApiPipeline<TestBuilder> pipeline(ctx, "this.host.does.not.exist.invalid");

    bool completed = false;
    std::expected<std::string, dbn_pipe::Error> result_holder;

    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        auto result = co_await pipeline.fetch("/test", {}, {});
        result_holder = std::move(result);
        completed = true;
    }, asio::detached);

    // Run until completion or timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!completed && std::chrono::steady_clock::now() < deadline) {
        ctx.poll();
    }

    ASSERT_TRUE(completed) << "Fetch did not complete within timeout";
    ASSERT_FALSE(result_holder.has_value()) << "Expected error for invalid host";
}
```

**Step 2: Run test to verify it passes**

Run: `bazel test //example/rest_api:rest_api_pipeline_test --test_output=errors`
Expected: PASS (errors returned for unreachable/invalid hosts)

**Step 3: Commit**

```bash
git add example/rest_api/test/rest_api_pipeline_test.cpp
git commit -m "test(rest_api): add integration tests for error handling"
```

---

## Task 6: Run all tests and verify no regressions

**Step 1: Run full test suite**

Run: `bazel test //...`
Expected: All tests pass

**Step 2: If any failures, debug and fix**

Check output for failures. Common issues:
- Missing includes
- Namespace conflicts
- Build dependency issues

**Step 3: Commit any fixes**

```bash
git add -A
git commit -m "fix: address test failures" # if needed
```

---

## Task 7: Final cleanup and documentation

**Files:**
- Create: `example/rest_api/README.md`

**Step 1: Create README**

Create `example/rest_api/README.md`:

```markdown
# RestApiPipeline

Coroutine-based REST API pipeline that composes with dbWriter's BatchWriter.

## Usage

```cpp
#include "rest_api/rest_api_pipeline.hpp"
#include "dbwriter/batch_writer.hpp"

// Define JSON builder for your API response
struct MyResponseBuilder {
    using Result = MyResponse;
    // SAX parser implementation...
};

asio::awaitable<void> fetch_data(asio::io_context& ctx, IDatabase& db) {
    rest_api::RestApiPipeline<MyResponseBuilder> api(ctx, "api.example.com");
    api.set_api_key("your_key");

    dbwriter::BatchWriter writer(ctx, db, my_table, MyTransform{});

    for (const auto& id : ids) {
        auto result = co_await api.fetch(
            "/v1/data/{id}",
            {{"id", id}},
            {{"format", "json"}});

        if (result) {
            writer.enqueue(result->records);
        }

        // Rate limiting
        asio::steady_timer timer(ctx, std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }

    co_await writer.drain();
}
```

## API

### Constructor

```cpp
RestApiPipeline(asio::io_context& ctx, std::string host, uint16_t port = 443);
```

### Methods

- `set_api_key(std::string key)` - Set API key for Basic auth
- `fetch(path_template, path_params, query_params)` - Returns `asio::awaitable<std::expected<Result, Error>>`

### Path Parameters

Use `{name}` syntax in path_template:

```cpp
api.fetch("/v2/ticker/{symbol}/range/{start}/{end}",
          {{"symbol", "AAPL"}, {"start", "2024-01-01"}, {"end", "2024-01-31"}},
          {{"apiKey", key}});
```
```

**Step 2: Commit**

```bash
git add example/rest_api/README.md
git commit -m "docs(rest_api): add README with usage examples"
```

---

## Summary

| Task | Description | Files |
|------|-------------|-------|
| 1 | Add PathTemplate to HttpRequestBuilder | lib/stream/http_request_builder.hpp, tests/ |
| 2 | Add path_params to ApiRequest | src/api_protocol.hpp, tests/ |
| 3 | Create example/rest_api structure | BUILD.bazel |
| 4 | Implement RestApiPipeline | include/rest_api/rest_api_pipeline.hpp |
| 5 | Add integration tests | test/rest_api_pipeline_test.cpp |
| 6 | Verify no regressions | - |
| 7 | Documentation | README.md |
