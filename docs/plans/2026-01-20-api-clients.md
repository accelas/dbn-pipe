# API Clients Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add MetadataClient and SymbologyClient for querying Databento's JSON APIs before downloading data.

**Architecture:** Reuse existing TLS/HTTP pipeline, add rapidjson SAX parser as terminal component. Schema-driven builders with optional fields enable best-effort results on connection errors. RetryPolicy enhanced with error classification.

**Tech Stack:** rapidjson (SAX parsing), existing TlsTransport/HttpClient, std::expected for results

---

## Task 1: Add rapidjson Dependency

**Files:**
- Modify: `MODULE.bazel`
- Create: `third_party/rapidjson/BUILD.rapidjson.bazel`
- Create: `third_party/rapidjson/rapidjson.bzl`

**Step 1: Create rapidjson module extension**

Create `third_party/rapidjson/rapidjson.bzl`:

```starlark
"""RapidJSON dependency."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def rapidjson_dependency():
    http_archive(
        name = "rapidjson",
        urls = ["https://github.com/Tencent/rapidjson/archive/refs/tags/v1.1.0.tar.gz"],
        strip_prefix = "rapidjson-1.1.0",
        sha256 = "bf7ced29704a1e696fbccf2a2b4ea068e7774fa37f6d7dd4039d0787f8bed98e",
        build_file = "//third_party/rapidjson:BUILD.rapidjson.bazel",
    )
```

**Step 2: Create rapidjson BUILD file**

Create `third_party/rapidjson/BUILD.rapidjson.bazel`:

```starlark
package(default_visibility = ["//visibility:public"])

cc_library(
    name = "rapidjson",
    hdrs = glob(["include/rapidjson/**/*.h"]),
    includes = ["include"],
)
```

**Step 3: Add to MODULE.bazel**

Add after existing extensions in `MODULE.bazel`:

```starlark
# RapidJSON
rapidjson_ext = use_extension("//third_party/rapidjson:rapidjson.bzl", "rapidjson_dependency")
use_repo(rapidjson_ext, "rapidjson")
```

**Step 4: Verify build**

Run: `bazel build @rapidjson//:rapidjson`
Expected: BUILD SUCCESS

**Step 5: Commit**

```bash
git add third_party/rapidjson/ MODULE.bazel
git commit -m "feat: add rapidjson dependency for JSON API parsing"
```

---

## Task 2: Extract URL Encoding Utilities

**Files:**
- Create: `src/api/url_encode.hpp`
- Modify: `src/historical_protocol.hpp`
- Create: `tests/url_encode_test.cpp`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

Create `tests/url_encode_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include <sstream>
#include "src/api/url_encode.hpp"

namespace dbn_pipe {
namespace {

TEST(UrlEncodeTest, AlphanumericPassthrough) {
    std::ostringstream out;
    UrlEncode(out, "abc123XYZ");
    EXPECT_EQ(out.str(), "abc123XYZ");
}

TEST(UrlEncodeTest, SpecialCharsEncoded) {
    std::ostringstream out;
    UrlEncode(out, "hello world");
    EXPECT_EQ(out.str(), "hello%20world");
}

TEST(UrlEncodeTest, SymbolsEncoded) {
    std::ostringstream out;
    UrlEncode(out, "SPY,QQQ");
    EXPECT_EQ(out.str(), "SPY%2CQQQ");
}

TEST(UrlEncodeTest, SafeCharsPassthrough) {
    std::ostringstream out;
    UrlEncode(out, "a-b_c.d~e");
    EXPECT_EQ(out.str(), "a-b_c.d~e");
}

TEST(Base64EncodeTest, BasicString) {
    std::ostringstream out;
    Base64Encode(out, "test");
    EXPECT_EQ(out.str(), "dGVzdA==");
}

TEST(Base64EncodeTest, ApiKeyFormat) {
    std::ostringstream out;
    Base64Encode(out, "db-abc123:");
    EXPECT_EQ(out.str(), "ZGItYWJjMTIzOg==");
}

}  // namespace
}  // namespace dbn_pipe
```

**Step 2: Add test to BUILD.bazel**

Add to `tests/BUILD.bazel`:

```starlark
cc_test(
    name = "url_encode_test",
    srcs = ["url_encode_test.cpp"],
    deps = [
        "//src/api:url_encode",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:url_encode_test`
Expected: FAIL (file not found)

**Step 4: Create src/api/BUILD.bazel**

Create `src/api/BUILD.bazel`:

```starlark
package(default_visibility = ["//visibility:public"])

cc_library(
    name = "url_encode",
    hdrs = ["url_encode.hpp"],
)
```

**Step 5: Create url_encode.hpp**

Create `src/api/url_encode.hpp`:

```cpp
// src/api/url_encode.hpp
#pragma once

#include <cctype>
#include <iomanip>
#include <ostream>
#include <string_view>

namespace dbn_pipe {

// URL encode a string, writing directly to output stream
inline void UrlEncode(std::ostream& out, std::string_view value) {
    auto flags = out.flags();
    out.fill('0');
    out << std::hex;

    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::setw(2) << std::uppercase
                << static_cast<int>(static_cast<unsigned char>(c));
        }
    }

    out.flags(flags);
}

// Base64 encode a string, writing directly to output stream
inline void Base64Encode(std::ostream& out, std::string_view input) {
    static const char* kBase64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t i = 0;
    while (i + 2 < input.size()) {
        uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) |
                          (static_cast<uint8_t>(input[i + 1]) << 8) |
                          static_cast<uint8_t>(input[i + 2]);
        out << kBase64Chars[(triple >> 18) & 0x3F];
        out << kBase64Chars[(triple >> 12) & 0x3F];
        out << kBase64Chars[(triple >> 6) & 0x3F];
        out << kBase64Chars[triple & 0x3F];
        i += 3;
    }

    if (i + 1 == input.size()) {
        uint32_t val = static_cast<uint8_t>(input[i]) << 16;
        out << kBase64Chars[(val >> 18) & 0x3F];
        out << kBase64Chars[(val >> 12) & 0x3F];
        out << '=';
        out << '=';
    } else if (i + 2 == input.size()) {
        uint32_t val = (static_cast<uint8_t>(input[i]) << 16) |
                       (static_cast<uint8_t>(input[i + 1]) << 8);
        out << kBase64Chars[(val >> 18) & 0x3F];
        out << kBase64Chars[(val >> 12) & 0x3F];
        out << kBase64Chars[(val >> 6) & 0x3F];
        out << '=';
    }
}

}  // namespace dbn_pipe
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:url_encode_test`
Expected: PASS

**Step 7: Update historical_protocol.hpp to use shared utilities**

Modify `src/historical_protocol.hpp`:
- Add `#include "api/url_encode.hpp"` after other includes
- Remove the inline `UrlEncode` and `Base64Encode` methods from `HistoricalProtocol` struct
- Update `SendRequest` to use `dbn_pipe::UrlEncode` and `dbn_pipe::Base64Encode`

**Step 8: Run existing tests**

Run: `bazel test //tests:historical_protocol_test`
Expected: PASS

**Step 9: Commit**

```bash
git add src/api/ tests/url_encode_test.cpp tests/BUILD.bazel src/historical_protocol.hpp
git commit -m "refactor: extract URL encoding utilities to src/api/url_encode.hpp"
```

---

## Task 3: Add API Error Codes

**Files:**
- Modify: `lib/stream/error.hpp`
- Create: `tests/api_error_test.cpp`

**Step 1: Write the failing test**

Create `tests/api_error_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include "lib/stream/error.hpp"

namespace dbn_pipe {
namespace {

TEST(ApiErrorTest, ErrorCodesExist) {
    // Verify new error codes compile
    Error unauthorized{ErrorCode::Unauthorized, "Invalid API key"};
    Error rate_limited{ErrorCode::RateLimited, "Too many requests"};
    Error not_found{ErrorCode::NotFound, "Resource not found"};
    Error validation{ErrorCode::ValidationError, "Invalid parameters"};
    Error server{ErrorCode::ServerError, "Internal server error"};

    EXPECT_EQ(unauthorized.code, ErrorCode::Unauthorized);
    EXPECT_EQ(rate_limited.code, ErrorCode::RateLimited);
    EXPECT_EQ(not_found.code, ErrorCode::NotFound);
    EXPECT_EQ(validation.code, ErrorCode::ValidationError);
    EXPECT_EQ(server.code, ErrorCode::ServerError);
}

TEST(ApiErrorTest, RetryAfterField) {
    Error e{ErrorCode::RateLimited, "Too many requests"};
    e.retry_after = std::chrono::milliseconds{5000};

    EXPECT_TRUE(e.retry_after.has_value());
    EXPECT_EQ(e.retry_after->count(), 5000);
}

}  // namespace
}  // namespace dbn_pipe
```

**Step 2: Add test to BUILD.bazel**

Add to `tests/BUILD.bazel`:

```starlark
cc_test(
    name = "api_error_test",
    srcs = ["api_error_test.cpp"],
    deps = [
        "//lib/stream:error",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:api_error_test`
Expected: FAIL (error codes not defined)

**Step 4: Add API error codes to error.hpp**

Modify `lib/stream/error.hpp`, add to `ErrorCode` enum:

```cpp
    // API errors
    Unauthorized,       // HTTP 401/403
    RateLimited,        // HTTP 429
    NotFound,           // HTTP 404
    ValidationError,    // HTTP 422
    ServerError,        // HTTP 5xx
```

Add `retry_after` field to `Error` struct:

```cpp
struct Error {
    ErrorCode code;
    std::string message;
    int os_error = 0;
    std::optional<std::chrono::milliseconds> retry_after;  // For rate limiting
};
```

**Step 5: Run test to verify it passes**

Run: `bazel test //tests:api_error_test`
Expected: PASS

**Step 6: Run all tests**

Run: `bazel test //tests:all`
Expected: All tests pass

**Step 7: Commit**

```bash
git add lib/stream/error.hpp tests/api_error_test.cpp tests/BUILD.bazel
git commit -m "feat: add API error codes and retry_after field"
```

---

## Task 4: Enhance RetryPolicy with Error Classification

**Files:**
- Modify: `src/retry_policy.hpp`
- Modify: `tests/retry_policy_test.cpp`

**Step 1: Write the failing test**

Add to `tests/retry_policy_test.cpp`:

```cpp
#include "lib/stream/error.hpp"

TEST(RetryPolicyTest, ShouldNotRetryUnauthorized) {
    RetryPolicy policy;
    Error e{ErrorCode::Unauthorized, "Invalid API key"};
    EXPECT_FALSE(policy.ShouldRetry(e));
}

TEST(RetryPolicyTest, ShouldNotRetryValidationError) {
    RetryPolicy policy;
    Error e{ErrorCode::ValidationError, "Invalid parameters"};
    EXPECT_FALSE(policy.ShouldRetry(e));
}

TEST(RetryPolicyTest, ShouldNotRetryNotFound) {
    RetryPolicy policy;
    Error e{ErrorCode::NotFound, "Resource not found"};
    EXPECT_FALSE(policy.ShouldRetry(e));
}

TEST(RetryPolicyTest, ShouldRetryServerError) {
    RetryPolicy policy;
    Error e{ErrorCode::ServerError, "Internal server error"};
    EXPECT_TRUE(policy.ShouldRetry(e));
}

TEST(RetryPolicyTest, ShouldRetryConnectionFailed) {
    RetryPolicy policy;
    Error e{ErrorCode::ConnectionFailed, "Connection reset"};
    EXPECT_TRUE(policy.ShouldRetry(e));
}

TEST(RetryPolicyTest, ShouldRetryRateLimited) {
    RetryPolicy policy;
    Error e{ErrorCode::RateLimited, "Too many requests"};
    EXPECT_TRUE(policy.ShouldRetry(e));
}

TEST(RetryPolicyTest, RespectsMaxRetries) {
    RetryConfig config{.max_retries = 2};
    RetryPolicy policy(config);
    Error e{ErrorCode::ServerError, "Internal server error"};

    EXPECT_TRUE(policy.ShouldRetry(e));
    policy.RecordAttempt();
    EXPECT_TRUE(policy.ShouldRetry(e));
    policy.RecordAttempt();
    EXPECT_FALSE(policy.ShouldRetry(e));  // Max reached
}

TEST(RetryPolicyTest, GetNextDelayRespectsRetryAfter) {
    RetryPolicy policy;
    Error e{ErrorCode::RateLimited, "Too many requests"};
    e.retry_after = std::chrono::milliseconds{5000};

    auto delay = policy.GetNextDelay(e);
    EXPECT_EQ(delay.count(), 5000);
}

TEST(RetryPolicyTest, GetNextDelayUsesBackoffWithoutRetryAfter) {
    RetryPolicy policy;
    Error e{ErrorCode::ServerError, "Internal server error"};

    auto delay = policy.GetNextDelay(e);
    EXPECT_GE(delay.count(), 900);   // initial_delay * (1 - jitter)
    EXPECT_LE(delay.count(), 1100);  // initial_delay * (1 + jitter)
}
```

**Step 2: Update retry_policy_test.cpp deps in BUILD.bazel**

Modify `tests/BUILD.bazel`, update retry_policy_test deps:

```starlark
cc_test(
    name = "retry_policy_test",
    srcs = ["retry_policy_test.cpp"],
    deps = [
        "//src:retry_policy",
        "//lib/stream:error",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:retry_policy_test`
Expected: FAIL (ShouldRetry(Error) not defined)

**Step 4: Implement error-aware RetryPolicy**

Modify `src/retry_policy.hpp`:

```cpp
// src/retry_policy.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>

#include "lib/stream/error.hpp"

namespace dbn_pipe {

struct RetryConfig {
    uint32_t max_retries = 5;
    std::chrono::milliseconds initial_delay{1000};
    std::chrono::milliseconds max_delay{60000};
    double backoff_multiplier = 2.0;
    double jitter_factor = 0.1;  // +/- 10%
};

class RetryPolicy {
public:
    explicit RetryPolicy(RetryConfig config = {})
        : config_(config), attempts_(0) {}

    // Legacy: check if more retries available (for backward compat)
    bool ShouldRetry() const {
        return attempts_ < config_.max_retries;
    }

    // Error-aware: classify error and check retry budget
    bool ShouldRetry(const Error& e) const {
        if (!IsRetryable(e.code)) {
            return false;
        }
        return attempts_ < config_.max_retries;
    }

    void RecordAttempt() {
        ++attempts_;
    }

    void Reset() {
        attempts_ = 0;
    }

    // Legacy: calculate delay with optional retry_after
    std::chrono::milliseconds GetNextDelay(
        std::optional<std::chrono::seconds> retry_after = std::nullopt) const {
        if (retry_after.has_value()) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(*retry_after);
        }
        return CalculateBackoff();
    }

    // Error-aware: use retry_after from error if present
    std::chrono::milliseconds GetNextDelay(const Error& e) const {
        if (e.retry_after.has_value()) {
            return *e.retry_after;
        }
        return CalculateBackoff();
    }

    uint32_t Attempts() const { return attempts_; }

private:
    static bool IsRetryable(ErrorCode code) {
        switch (code) {
            // Retryable errors
            case ErrorCode::ConnectionFailed:
            case ErrorCode::ServerError:
            case ErrorCode::TlsHandshakeFailed:
            case ErrorCode::RateLimited:
                return true;

            // Non-retryable errors
            case ErrorCode::Unauthorized:
            case ErrorCode::NotFound:
            case ErrorCode::ValidationError:
            case ErrorCode::ParseError:
            default:
                return false;
        }
    }

    std::chrono::milliseconds CalculateBackoff() const {
        double delay_ms = static_cast<double>(config_.initial_delay.count());
        for (uint32_t i = 0; i < attempts_; ++i) {
            delay_ms *= config_.backoff_multiplier;
        }
        delay_ms = std::min(delay_ms, static_cast<double>(config_.max_delay.count()));

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(
            1.0 - config_.jitter_factor,
            1.0 + config_.jitter_factor);
        delay_ms *= dis(gen);

        return std::chrono::milliseconds(static_cast<int64_t>(delay_ms));
    }

    RetryConfig config_;
    uint32_t attempts_;
};

}  // namespace dbn_pipe
```

**Step 5: Update src/BUILD.bazel for retry_policy deps**

Modify `src/BUILD.bazel`, update retry_policy:

```starlark
cc_library(
    name = "retry_policy",
    hdrs = ["retry_policy.hpp"],
    deps = ["//lib/stream:error"],
)
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:retry_policy_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/retry_policy.hpp src/BUILD.bazel tests/retry_policy_test.cpp tests/BUILD.bazel
git commit -m "feat: add error classification to RetryPolicy"
```

---

## Task 5: Create JsonParser Component

**Files:**
- Create: `src/api/json_parser.hpp`
- Create: `tests/json_parser_test.cpp`
- Modify: `src/api/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

Create `tests/json_parser_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include <expected>
#include <optional>
#include <string>

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/event_loop.hpp"
#include "src/api/json_parser.hpp"

namespace dbn_pipe {
namespace {

// Simple builder for testing - parses {"value": 123}
struct IntBuilder {
    using Result = int64_t;
    std::optional<int64_t> value;
    std::string current_key;

    void OnKey(std::string_view key) { current_key = key; }
    void OnInt(int64_t v) { if (current_key == "value") value = v; }
    void OnUint(uint64_t v) { if (current_key == "value") value = static_cast<int64_t>(v); }
    void OnString(std::string_view) {}
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (!value) return std::unexpected("missing 'value'");
        return *value;
    }
};

class JsonParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create mock event loop for testing
    }

    void FeedJson(JsonParser<IntBuilder>& parser, const std::string& json) {
        BufferChain chain;
        auto seg = std::make_shared<Segment>();
        std::memcpy(seg->data.data(), json.data(), json.size());
        seg->size = json.size();
        chain.Append(std::move(seg));
        parser.OnData(chain);
    }
};

TEST_F(JsonParserTest, ParsesSimpleObject) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder,
        [&](auto r) { result = std::move(r); called = true; });

    FeedJson(*parser, R"({"value": 42})");
    parser->OnDone();

    ASSERT_TRUE(called);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST_F(JsonParserTest, HandlesMissingField) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder,
        [&](auto r) { result = std::move(r); called = true; });

    FeedJson(*parser, R"({"other": 123})");
    parser->OnDone();

    ASSERT_TRUE(called);
    ASSERT_FALSE(result.has_value());
}

TEST_F(JsonParserTest, BestEffortOnError) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder,
        [&](auto r) { result = std::move(r); called = true; });

    // Feed complete data, then simulate connection error
    FeedJson(*parser, R"({"value": 99})");
    parser->OnError(Error{ErrorCode::ConnectionFailed, "Connection reset"});

    ASSERT_TRUE(called);
    // Should succeed because we have all required data
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 99);
}

TEST_F(JsonParserTest, ErrorWhenIncomplete) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder,
        [&](auto r) { result = std::move(r); called = true; });

    // Feed incomplete data, then error
    FeedJson(*parser, R"({"other":)");
    parser->OnError(Error{ErrorCode::ConnectionFailed, "Connection reset"});

    ASSERT_TRUE(called);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::ConnectionFailed);
}

TEST_F(JsonParserTest, ParsesChunkedInput) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder,
        [&](auto r) { result = std::move(r); called = true; });

    // Feed JSON in chunks
    FeedJson(*parser, R"({"val)");
    FeedJson(*parser, R"(ue": 1)");
    FeedJson(*parser, R"(23})");
    parser->OnDone();

    ASSERT_TRUE(called);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 123);
}

}  // namespace
}  // namespace dbn_pipe
```

**Step 2: Add test to BUILD.bazel**

Add to `tests/BUILD.bazel`:

```starlark
cc_test(
    name = "json_parser_test",
    srcs = ["json_parser_test.cpp"],
    deps = [
        "//src/api:json_parser",
        "//lib/stream:buffer_chain",
        "//lib/stream:error",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:json_parser_test`
Expected: FAIL (file not found)

**Step 4: Add json_parser to src/api/BUILD.bazel**

Add to `src/api/BUILD.bazel`:

```starlark
cc_library(
    name = "json_parser",
    hdrs = ["json_parser.hpp"],
    deps = [
        "//lib/stream:buffer_chain",
        "//lib/stream:error",
        "@rapidjson",
    ],
)
```

**Step 5: Create json_parser.hpp**

Create `src/api/json_parser.hpp`:

```cpp
// src/api/json_parser.hpp
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>

#include <rapidjson/reader.h>

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/error.hpp"

namespace dbn_pipe {

// JsonParser - SAX-style JSON parser that feeds events to a Builder.
//
// Builder must provide:
//   - using Result = <result type>
//   - void OnKey(std::string_view)
//   - void OnString(std::string_view)
//   - void OnInt(int64_t)
//   - void OnUint(uint64_t)
//   - void OnDouble(double)
//   - void OnBool(bool)
//   - void OnNull()
//   - void OnStartObject()
//   - void OnEndObject()
//   - void OnStartArray()
//   - void OnEndArray()
//   - std::expected<Result, std::string> Build()
//
template <typename Builder>
class JsonParser : public std::enable_shared_from_this<JsonParser<Builder>> {
public:
    using Result = typename Builder::Result;
    using Callback = std::function<void(std::expected<Result, Error>)>;

    static std::shared_ptr<JsonParser> Create(Builder& builder, Callback on_complete) {
        return std::shared_ptr<JsonParser>(new JsonParser(builder, std::move(on_complete)));
    }

    // Downstream interface - receives HTTP body
    void OnData(BufferChain& data) {
        if (complete_) return;

        // Append to buffer for incremental parsing
        while (!data.Empty()) {
            size_t chunk_size = data.ContiguousSize();
            const char* ptr = reinterpret_cast<const char*>(data.DataAt(0));
            buffer_.append(ptr, chunk_size);
            data.Consume(chunk_size);
        }

        // Try to parse what we have (rapidjson handles incomplete JSON)
        ParseBuffer();
    }

    void OnError(const Error& e) {
        if (complete_) return;

        // Try to build with what we have (best effort)
        auto result = builder_.Build();
        if (result) {
            Complete(std::move(*result));
        } else {
            Complete(std::unexpected(e));
        }
    }

    void OnDone() {
        if (complete_) return;

        // Final parse attempt
        ParseBuffer();

        // Build result
        auto result = builder_.Build();
        if (result) {
            Complete(std::move(*result));
        } else {
            Complete(std::unexpected(Error{
                ErrorCode::ParseError,
                "JSON parse incomplete: " + result.error()
            }));
        }
    }

private:
    JsonParser(Builder& builder, Callback on_complete)
        : builder_(builder), on_complete_(std::move(on_complete)) {}

    void ParseBuffer() {
        if (buffer_.empty()) return;

        SaxHandler handler{builder_};
        rapidjson::Reader reader;
        rapidjson::InsituStringStream ss(buffer_.data());

        // Parse with kParseStopWhenDoneFlag to handle trailing data
        reader.Parse<rapidjson::kParseStopWhenDoneFlag>(ss, handler);

        if (reader.HasParseError()) {
            auto err = reader.GetParseErrorCode();
            // Ignore incomplete errors - we may get more data
            if (err != rapidjson::kParseErrorDocumentEmpty &&
                err != rapidjson::kParseErrorValueInvalid) {
                // Real parse error
                Complete(std::unexpected(Error{
                    ErrorCode::ParseError,
                    "JSON parse error at offset " + std::to_string(reader.GetErrorOffset())
                }));
            }
        }
    }

    void Complete(std::expected<Result, Error> result) {
        if (complete_) return;
        complete_ = true;
        if (on_complete_) {
            on_complete_(std::move(result));
        }
    }

    // rapidjson SAX handler
    struct SaxHandler : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>, SaxHandler> {
        Builder& builder;

        explicit SaxHandler(Builder& b) : builder(b) {}

        bool Key(const char* str, rapidjson::SizeType len, bool) {
            builder.OnKey(std::string_view(str, len));
            return true;
        }

        bool String(const char* str, rapidjson::SizeType len, bool) {
            builder.OnString(std::string_view(str, len));
            return true;
        }

        bool Int(int i) {
            builder.OnInt(static_cast<int64_t>(i));
            return true;
        }

        bool Uint(unsigned u) {
            builder.OnUint(static_cast<uint64_t>(u));
            return true;
        }

        bool Int64(int64_t i) {
            builder.OnInt(i);
            return true;
        }

        bool Uint64(uint64_t u) {
            builder.OnUint(u);
            return true;
        }

        bool Double(double d) {
            builder.OnDouble(d);
            return true;
        }

        bool Bool(bool b) {
            builder.OnBool(b);
            return true;
        }

        bool Null() {
            builder.OnNull();
            return true;
        }

        bool StartObject() {
            builder.OnStartObject();
            return true;
        }

        bool EndObject(rapidjson::SizeType) {
            builder.OnEndObject();
            return true;
        }

        bool StartArray() {
            builder.OnStartArray();
            return true;
        }

        bool EndArray(rapidjson::SizeType) {
            builder.OnEndArray();
            return true;
        }
    };

    Builder& builder_;
    Callback on_complete_;
    std::string buffer_;
    bool complete_ = false;
};

}  // namespace dbn_pipe
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:json_parser_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/api/json_parser.hpp src/api/BUILD.bazel tests/json_parser_test.cpp tests/BUILD.bazel
git commit -m "feat: add JsonParser SAX component for API responses"
```

---

## Task 6: Create ApiPipeline

**Files:**
- Create: `src/api/api_pipeline.hpp`
- Create: `tests/api_pipeline_test.cpp`
- Modify: `src/api/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

Create `tests/api_pipeline_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include <expected>
#include <optional>
#include <string>

#include "src/api/api_pipeline.hpp"

namespace dbn_pipe {
namespace {

// Builder for {"count": N}
struct CountBuilder {
    using Result = uint64_t;
    std::optional<uint64_t> count;
    std::string current_key;

    void OnKey(std::string_view key) { current_key = key; }
    void OnInt(int64_t v) { if (current_key == "count") count = static_cast<uint64_t>(v); }
    void OnUint(uint64_t v) { if (current_key == "count") count = v; }
    void OnString(std::string_view) {}
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (!count) return std::unexpected("missing 'count'");
        return *count;
    }
};

TEST(ApiPipelineTest, BuildsGetRequest) {
    ApiRequest req{
        .method = "GET",
        .path = "/v0/metadata.get_dataset_range",
        .query_params = {{"dataset", "GLBX.MDP3"}},
    };

    std::string http = req.BuildHttpRequest("hist.databento.com", "db-test123");

    EXPECT_NE(http.find("GET /v0/metadata.get_dataset_range?dataset=GLBX.MDP3"), std::string::npos);
    EXPECT_NE(http.find("Host: hist.databento.com"), std::string::npos);
    EXPECT_NE(http.find("Authorization: Basic"), std::string::npos);
}

TEST(ApiPipelineTest, BuildsPostRequest) {
    ApiRequest req{
        .method = "POST",
        .path = "/v0/metadata.get_record_count",
        .form_params = {
            {"dataset", "GLBX.MDP3"},
            {"symbols", "ESM4"},
            {"schema", "trades"},
        },
    };

    std::string http = req.BuildHttpRequest("hist.databento.com", "db-test123");

    EXPECT_NE(http.find("POST /v0/metadata.get_record_count"), std::string::npos);
    EXPECT_NE(http.find("Content-Type: application/x-www-form-urlencoded"), std::string::npos);
    EXPECT_NE(http.find("dataset=GLBX.MDP3"), std::string::npos);
}

TEST(ApiPipelineTest, UrlEncodesParams) {
    ApiRequest req{
        .method = "GET",
        .path = "/test",
        .query_params = {{"symbols", "SPY,QQQ"}},
    };

    std::string http = req.BuildHttpRequest("example.com", "key");

    EXPECT_NE(http.find("symbols=SPY%2CQQQ"), std::string::npos);
}

}  // namespace
}  // namespace dbn_pipe
```

**Step 2: Add test to BUILD.bazel**

Add to `tests/BUILD.bazel`:

```starlark
cc_test(
    name = "api_pipeline_test",
    srcs = ["api_pipeline_test.cpp"],
    deps = [
        "//src/api:api_pipeline",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:api_pipeline_test`
Expected: FAIL (file not found)

**Step 4: Add api_pipeline to BUILD.bazel**

Add to `src/api/BUILD.bazel`:

```starlark
cc_library(
    name = "api_pipeline",
    hdrs = ["api_pipeline.hpp"],
    deps = [
        ":json_parser",
        ":url_encode",
        "//lib/stream:buffer_chain",
        "//lib/stream:tcp_socket",
        "//src:http_client",
        "//src:tls_transport",
    ],
)
```

**Step 5: Create api_pipeline.hpp**

Create `src/api/api_pipeline.hpp`:

```cpp
// src/api/api_pipeline.hpp
#pragma once

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "json_parser.hpp"
#include "url_encode.hpp"
#include "lib/stream/buffer_chain.hpp"

namespace dbn_pipe {

// API request configuration
struct ApiRequest {
    std::string method = "GET";
    std::string path;
    std::vector<std::pair<std::string, std::string>> query_params;
    std::vector<std::pair<std::string, std::string>> form_params;  // For POST

    // Build complete HTTP request
    std::string BuildHttpRequest(const std::string& host, const std::string& api_key) const {
        std::ostringstream out;

        // Request line
        out << method << " " << path;

        // Query string
        if (!query_params.empty()) {
            out << "?";
            bool first = true;
            for (const auto& [key, value] : query_params) {
                if (!first) out << "&";
                first = false;
                UrlEncode(out, key);
                out << "=";
                UrlEncode(out, value);
            }
        }
        out << " HTTP/1.1\r\n";

        // Headers
        out << "Host: " << host << "\r\n";
        out << "Authorization: Basic ";
        Base64Encode(out, api_key + ":");
        out << "\r\n";
        out << "Accept: application/json\r\n";

        // Body for POST
        std::string body;
        if (method == "POST" && !form_params.empty()) {
            std::ostringstream body_stream;
            bool first = true;
            for (const auto& [key, value] : form_params) {
                if (!first) body_stream << "&";
                first = false;
                UrlEncode(body_stream, key);
                body_stream << "=";
                UrlEncode(body_stream, value);
            }
            body = body_stream.str();

            out << "Content-Type: application/x-www-form-urlencoded\r\n";
            out << "Content-Length: " << body.size() << "\r\n";
        }

        out << "Connection: close\r\n";
        out << "\r\n";

        if (!body.empty()) {
            out << body;
        }

        return out.str();
    }
};

// ApiPipeline orchestrates TLS -> HTTP -> JSON parsing
// Type-erased base for runtime polymorphism
class ApiPipelineBase {
public:
    virtual ~ApiPipelineBase() = default;
    virtual void Connect(const sockaddr_storage& addr) = 0;
    virtual void Close() = 0;
    virtual void SetReadyCallback(std::function<void()> cb) = 0;
    virtual void SendRequest(const std::string& http_request) = 0;
};

// Concrete pipeline for a specific builder type
// Chain: TcpSocket -> TlsTransport -> HttpClient -> JsonParser<Builder>
template <typename Builder>
class ApiPipeline : public ApiPipelineBase {
public:
    using Result = typename Builder::Result;
    using Callback = std::function<void(std::expected<Result, Error>)>;

    ApiPipeline(IEventLoop& loop, Builder& builder, Callback callback)
        : loop_(loop)
        , json_parser_(JsonParser<Builder>::Create(builder, std::move(callback)))
        , http_(HttpClient<JsonParser<Builder>>::Create(loop, json_parser_))
        , tls_(TlsTransport<HttpClient<JsonParser<Builder>>>::Create(loop, http_))
        , socket_(TcpSocket<TlsTransport<HttpClient<JsonParser<Builder>>>>::Create(loop, tls_))
    {
        http_->SetUpstream(tls_.get());

        socket_->OnConnect([this]() {
            tls_->StartHandshake();
        });

        tls_->SetHandshakeCompleteCallback([this]() {
            if (ready_cb_) ready_cb_();
        });
    }

    void SetHostname(const std::string& hostname) {
        tls_->SetHostname(hostname);
    }

    void Connect(const sockaddr_storage& addr) override {
        socket_->Connect(addr);
    }

    void Close() override {
        socket_->Close();
    }

    void SetReadyCallback(std::function<void()> cb) override {
        ready_cb_ = std::move(cb);
    }

    void SendRequest(const std::string& http_request) override {
        BufferChain chain;
        auto seg = std::make_shared<Segment>();
        std::memcpy(seg->data.data(), http_request.data(), http_request.size());
        seg->size = http_request.size();
        chain.Append(std::move(seg));
        tls_->Write(std::move(chain));
    }

private:
    IEventLoop& loop_;
    std::shared_ptr<JsonParser<Builder>> json_parser_;
    std::shared_ptr<HttpClient<JsonParser<Builder>>> http_;
    std::shared_ptr<TlsTransport<HttpClient<JsonParser<Builder>>>> tls_;
    std::shared_ptr<TcpSocket<TlsTransport<HttpClient<JsonParser<Builder>>>>> socket_;
    std::function<void()> ready_cb_;
};

}  // namespace dbn_pipe
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:api_pipeline_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/api/api_pipeline.hpp src/api/BUILD.bazel tests/api_pipeline_test.cpp tests/BUILD.bazel
git commit -m "feat: add ApiPipeline for JSON API requests"
```

---

## Task 7: Create MetadataClient

**Files:**
- Create: `src/api/metadata_client.hpp`
- Create: `tests/metadata_client_test.cpp`
- Modify: `src/api/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

Create `tests/metadata_client_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include "src/api/metadata_client.hpp"

namespace dbn_pipe {
namespace {

TEST(RecordCountBuilderTest, ParsesInteger) {
    RecordCountBuilder builder;
    builder.OnUint(12345);

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 12345);
}

TEST(RecordCountBuilderTest, FailsWhenEmpty) {
    RecordCountBuilder builder;
    auto result = builder.Build();
    ASSERT_FALSE(result.has_value());
}

TEST(BillableSizeBuilderTest, ParsesInteger) {
    BillableSizeBuilder builder;
    builder.OnUint(1048576);

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1048576);
}

TEST(CostBuilderTest, ParsesDouble) {
    CostBuilder builder;
    builder.OnDouble(0.50);

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 0.50);
}

TEST(DatasetRangeBuilderTest, ParsesStartEnd) {
    DatasetRangeBuilder builder;

    builder.OnKey("start");
    builder.OnString("2023-01-01");
    builder.OnKey("end");
    builder.OnString("2026-01-20");

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->start, "2023-01-01");
    EXPECT_EQ(result->end, "2026-01-20");
}

TEST(DatasetRangeBuilderTest, FailsWhenMissingStart) {
    DatasetRangeBuilder builder;
    builder.OnKey("end");
    builder.OnString("2026-01-20");

    auto result = builder.Build();
    ASSERT_FALSE(result.has_value());
}

TEST(DatasetRangeBuilderTest, PartialSucceedsWithBothFields) {
    DatasetRangeBuilder builder;
    builder.OnKey("start");
    builder.OnString("2023-01-01");
    builder.OnKey("end");
    builder.OnString("2026-01-20");
    // Extra field that arrives before connection drops
    builder.OnKey("extra");

    // Build should still succeed with required fields
    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
}

}  // namespace
}  // namespace dbn_pipe
```

**Step 2: Add test to BUILD.bazel**

Add to `tests/BUILD.bazel`:

```starlark
cc_test(
    name = "metadata_client_test",
    srcs = ["metadata_client_test.cpp"],
    deps = [
        "//src/api:metadata_client",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:metadata_client_test`
Expected: FAIL (file not found)

**Step 4: Add metadata_client to BUILD.bazel**

Add to `src/api/BUILD.bazel`:

```starlark
cc_library(
    name = "metadata_client",
    hdrs = ["metadata_client.hpp"],
    deps = [
        ":api_pipeline",
        "//src:dns_resolver",
        "//src:retry_policy",
    ],
)
```

**Step 5: Create metadata_client.hpp**

Create `src/api/metadata_client.hpp`:

```cpp
// src/api/metadata_client.hpp
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "api_pipeline.hpp"
#include "src/dns_resolver.hpp"
#include "src/retry_policy.hpp"

namespace dbn_pipe {

// Result types
struct DatasetRange {
    std::string start;
    std::string end;
};

// Builders for each response type

struct RecordCountBuilder {
    using Result = uint64_t;
    std::optional<uint64_t> value;

    void OnKey(std::string_view) {}
    void OnString(std::string_view) {}
    void OnInt(int64_t v) { value = static_cast<uint64_t>(v); }
    void OnUint(uint64_t v) { value = v; }
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (!value) return std::unexpected("missing record count value");
        return *value;
    }
};

struct BillableSizeBuilder {
    using Result = uint64_t;
    std::optional<uint64_t> value;

    void OnKey(std::string_view) {}
    void OnString(std::string_view) {}
    void OnInt(int64_t v) { value = static_cast<uint64_t>(v); }
    void OnUint(uint64_t v) { value = v; }
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (!value) return std::unexpected("missing billable size value");
        return *value;
    }
};

struct CostBuilder {
    using Result = double;
    std::optional<double> value;

    void OnKey(std::string_view) {}
    void OnString(std::string_view) {}
    void OnInt(int64_t v) { value = static_cast<double>(v); }
    void OnUint(uint64_t v) { value = static_cast<double>(v); }
    void OnDouble(double v) { value = v; }
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (!value) return std::unexpected("missing cost value");
        return *value;
    }
};

struct DatasetRangeBuilder {
    using Result = DatasetRange;
    std::optional<std::string> start;
    std::optional<std::string> end;
    std::string current_key;

    void OnKey(std::string_view key) { current_key = key; }
    void OnString(std::string_view v) {
        if (current_key == "start") start = std::string(v);
        else if (current_key == "end") end = std::string(v);
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
        if (!start) return std::unexpected("missing 'start' field");
        if (!end) return std::unexpected("missing 'end' field");
        return DatasetRange{std::move(*start), std::move(*end)};
    }
};

// MetadataClient - high-level API for metadata endpoints
class MetadataClient {
public:
    MetadataClient(IEventLoop& loop, std::string api_key)
        : loop_(loop)
        , api_key_(std::move(api_key))
        , resolver_(loop) {}

    void GetRecordCount(
        const std::string& dataset,
        const std::string& symbols,
        const std::string& schema,
        const std::string& start,
        const std::string& end,
        const std::string& stype_in,
        std::function<void(std::expected<uint64_t, Error>)> callback)
    {
        ApiRequest req{
            .method = "POST",
            .path = "/v0/metadata.get_record_count",
            .form_params = {
                {"dataset", dataset},
                {"symbols", symbols},
                {"schema", schema},
                {"start", start},
                {"end", end},
                {"stype_in", stype_in},
            },
        };
        CallApi<RecordCountBuilder>(req, std::move(callback));
    }

    void GetBillableSize(
        const std::string& dataset,
        const std::string& symbols,
        const std::string& schema,
        const std::string& start,
        const std::string& end,
        const std::string& stype_in,
        std::function<void(std::expected<uint64_t, Error>)> callback)
    {
        ApiRequest req{
            .method = "POST",
            .path = "/v0/metadata.get_billable_size",
            .form_params = {
                {"dataset", dataset},
                {"symbols", symbols},
                {"schema", schema},
                {"start", start},
                {"end", end},
                {"stype_in", stype_in},
            },
        };
        CallApi<BillableSizeBuilder>(req, std::move(callback));
    }

    void GetCost(
        const std::string& dataset,
        const std::string& symbols,
        const std::string& schema,
        const std::string& start,
        const std::string& end,
        const std::string& stype_in,
        std::function<void(std::expected<double, Error>)> callback)
    {
        ApiRequest req{
            .method = "POST",
            .path = "/v0/metadata.get_cost",
            .form_params = {
                {"dataset", dataset},
                {"symbols", symbols},
                {"schema", schema},
                {"start", start},
                {"end", end},
                {"stype_in", stype_in},
            },
        };
        CallApi<CostBuilder>(req, std::move(callback));
    }

    void GetDatasetRange(
        const std::string& dataset,
        std::function<void(std::expected<DatasetRange, Error>)> callback)
    {
        ApiRequest req{
            .method = "GET",
            .path = "/v0/metadata.get_dataset_range",
            .query_params = {{"dataset", dataset}},
        };
        CallApi<DatasetRangeBuilder>(req, std::move(callback));
    }

private:
    static constexpr const char* kHostname = "hist.databento.com";
    static constexpr uint16_t kPort = 443;

    template <typename Builder>
    void CallApi(const ApiRequest& req,
                 std::function<void(std::expected<typename Builder::Result, Error>)> callback)
    {
        auto builder = std::make_shared<Builder>();
        auto pipeline = std::make_shared<ApiPipeline<Builder>>(
            loop_, *builder,
            [callback, builder](auto result) {
                callback(std::move(result));
            });

        pipeline->SetHostname(kHostname);

        std::string http_request = req.BuildHttpRequest(kHostname, api_key_);

        pipeline->SetReadyCallback([pipeline, http_request]() {
            pipeline->SendRequest(http_request);
        });

        resolver_.Resolve(kHostname, kPort,
            [pipeline](std::expected<sockaddr_storage, Error> addr) {
                if (addr) {
                    pipeline->Connect(*addr);
                }
            });
    }

    IEventLoop& loop_;
    std::string api_key_;
    DnsResolver resolver_;
};

}  // namespace dbn_pipe
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:metadata_client_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/api/metadata_client.hpp src/api/BUILD.bazel tests/metadata_client_test.cpp tests/BUILD.bazel
git commit -m "feat: add MetadataClient for record count, billable size, cost, dataset range"
```

---

## Task 8: Create SymbologyClient

**Files:**
- Create: `src/api/symbology_client.hpp`
- Create: `tests/symbology_client_test.cpp`
- Modify: `src/api/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

Create `tests/symbology_client_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include "src/api/symbology_client.hpp"

namespace dbn_pipe {
namespace {

TEST(SymbologyBuilderTest, ParsesSimpleMapping) {
    SymbologyBuilder builder;

    // {"result": {"SPY": [{"d0": "2025-01-01", "d1": "2025-12-31", "s": "15144"}]}}
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnKey("SPY");
    builder.OnStartArray();
    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("s");
    builder.OnString("15144");
    builder.OnEndObject();
    builder.OnEndArray();
    builder.OnEndObject();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->result.size(), 1);
    ASSERT_EQ(result->result.count("SPY"), 1);
    ASSERT_EQ(result->result.at("SPY").size(), 1);
    EXPECT_EQ(result->result.at("SPY")[0].start_date, "2025-01-01");
    EXPECT_EQ(result->result.at("SPY")[0].end_date, "2025-12-31");
    EXPECT_EQ(result->result.at("SPY")[0].symbol, "15144");
}

TEST(SymbologyBuilderTest, ParsesMultipleMappings) {
    SymbologyBuilder builder;

    // {"result": {"SPY": [{"d0": "2025-01-01", "d1": "2025-06-30", "s": "15144"},
    //                     {"d0": "2025-07-01", "d1": "2025-12-31", "s": "15145"}]}}
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnKey("SPY");
    builder.OnStartArray();

    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnKey("d1");
    builder.OnString("2025-06-30");
    builder.OnKey("s");
    builder.OnString("15144");
    builder.OnEndObject();

    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2025-07-01");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("s");
    builder.OnString("15145");
    builder.OnEndObject();

    builder.OnEndArray();
    builder.OnEndObject();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->result.at("SPY").size(), 2);
}

TEST(SymbologyBuilderTest, ParsesNotFound) {
    SymbologyBuilder builder;

    // {"result": {}, "not_found": ["INVALID"]}
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnEndObject();
    builder.OnKey("not_found");
    builder.OnStartArray();
    builder.OnString("INVALID");
    builder.OnEndArray();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->result.empty());
    ASSERT_EQ(result->not_found.size(), 1);
    EXPECT_EQ(result->not_found[0], "INVALID");
}

TEST(SymbologyBuilderTest, ParsesPartial) {
    SymbologyBuilder builder;

    // {"result": {}, "partial": ["AMBIGUOUS"]}
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnEndObject();
    builder.OnKey("partial");
    builder.OnStartArray();
    builder.OnString("AMBIGUOUS");
    builder.OnEndArray();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->partial.size(), 1);
    EXPECT_EQ(result->partial[0], "AMBIGUOUS");
}

}  // namespace
}  // namespace dbn_pipe
```

**Step 2: Add test to BUILD.bazel**

Add to `tests/BUILD.bazel`:

```starlark
cc_test(
    name = "symbology_client_test",
    srcs = ["symbology_client_test.cpp"],
    deps = [
        "//src/api:symbology_client",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:symbology_client_test`
Expected: FAIL (file not found)

**Step 4: Add symbology_client to BUILD.bazel**

Add to `src/api/BUILD.bazel`:

```starlark
cc_library(
    name = "symbology_client",
    hdrs = ["symbology_client.hpp"],
    deps = [
        ":api_pipeline",
        "//src:dns_resolver",
        "//src:stype",
    ],
)
```

**Step 5: Create symbology_client.hpp**

Create `src/api/symbology_client.hpp`:

```cpp
// src/api/symbology_client.hpp
#pragma once

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api_pipeline.hpp"
#include "src/dns_resolver.hpp"
#include "src/stype.hpp"

namespace dbn_pipe {

// Symbol mapping interval
struct SymbolInterval {
    std::string start_date;  // d0
    std::string end_date;    // d1
    std::string symbol;      // s (the resolved symbol/instrument_id)
};

// Symbology resolution response
struct SymbologyResponse {
    std::map<std::string, std::vector<SymbolInterval>> result;
    std::vector<std::string> partial;
    std::vector<std::string> not_found;
};

// Builder for symbology response (nested JSON structure)
struct SymbologyBuilder {
    using Result = SymbologyResponse;

    // State machine for nested parsing
    enum class State {
        Root,
        InResult,
        InSymbolArray,
        InInterval,
        InPartialArray,
        InNotFoundArray,
    };

    State state = State::Root;
    std::string current_key;
    std::string current_symbol;
    SymbolInterval current_interval;
    SymbologyResponse response;

    void OnKey(std::string_view key) {
        current_key = key;
    }

    void OnString(std::string_view v) {
        switch (state) {
            case State::InInterval:
                if (current_key == "d0") current_interval.start_date = v;
                else if (current_key == "d1") current_interval.end_date = v;
                else if (current_key == "s") current_interval.symbol = v;
                break;
            case State::InPartialArray:
                response.partial.emplace_back(v);
                break;
            case State::InNotFoundArray:
                response.not_found.emplace_back(v);
                break;
            default:
                break;
        }
    }

    void OnInt(int64_t) {}
    void OnUint(uint64_t) {}
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}

    void OnStartObject() {
        switch (state) {
            case State::Root:
                break;
            case State::InResult:
                // Starting a new symbol's mapping array - current_key is the symbol
                current_symbol = current_key;
                break;
            case State::InSymbolArray:
                state = State::InInterval;
                current_interval = {};
                break;
            default:
                break;
        }
    }

    void OnEndObject() {
        switch (state) {
            case State::InInterval:
                response.result[current_symbol].push_back(std::move(current_interval));
                state = State::InSymbolArray;
                break;
            case State::InResult:
                state = State::Root;
                break;
            default:
                break;
        }
    }

    void OnStartArray() {
        if (state == State::Root) {
            if (current_key == "partial") {
                state = State::InPartialArray;
            } else if (current_key == "not_found") {
                state = State::InNotFoundArray;
            }
        } else if (state == State::InResult) {
            state = State::InSymbolArray;
            current_symbol = current_key;
        }
    }

    void OnEndArray() {
        switch (state) {
            case State::InSymbolArray:
                state = State::InResult;
                break;
            case State::InPartialArray:
            case State::InNotFoundArray:
                state = State::Root;
                break;
            default:
                break;
        }
    }

    std::expected<Result, std::string> Build() {
        // Symbology response is always valid (empty result is ok)
        return response;
    }

    // Entry point to result object
    void EnterResult() {
        if (current_key == "result") {
            state = State::InResult;
        }
    }
};

// Override OnStartObject to detect "result" entry
// Note: The SAX handler calls OnKey before OnStartObject,
// so we check current_key in OnStartObject

// SymbologyClient - high-level API for symbology resolution
class SymbologyClient {
public:
    SymbologyClient(IEventLoop& loop, std::string api_key)
        : loop_(loop)
        , api_key_(std::move(api_key))
        , resolver_(loop) {}

    void Resolve(
        const std::string& dataset,
        const std::vector<std::string>& symbols,
        SType stype_in,
        SType stype_out,
        const std::string& start_date,
        const std::string& end_date,
        std::function<void(std::expected<SymbologyResponse, Error>)> callback)
    {
        // Join symbols with comma
        std::string symbols_str;
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (i > 0) symbols_str += ",";
            symbols_str += symbols[i];
        }

        ApiRequest req{
            .method = "POST",
            .path = "/v0/symbology.resolve",
            .form_params = {
                {"dataset", dataset},
                {"symbols", symbols_str},
                {"stype_in", STypeToString(stype_in)},
                {"stype_out", STypeToString(stype_out)},
                {"start_date", start_date},
                {"end_date", end_date},
            },
        };

        auto builder = std::make_shared<SymbologyBuilder>();
        auto pipeline = std::make_shared<ApiPipeline<SymbologyBuilder>>(
            loop_, *builder,
            [callback, builder](auto result) {
                callback(std::move(result));
            });

        pipeline->SetHostname(kHostname);

        std::string http_request = req.BuildHttpRequest(kHostname, api_key_);

        pipeline->SetReadyCallback([pipeline, http_request]() {
            pipeline->SendRequest(http_request);
        });

        resolver_.Resolve(kHostname, kPort,
            [pipeline](std::expected<sockaddr_storage, Error> addr) {
                if (addr) {
                    pipeline->Connect(*addr);
                }
            });
    }

private:
    static constexpr const char* kHostname = "hist.databento.com";
    static constexpr uint16_t kPort = 443;

    IEventLoop& loop_;
    std::string api_key_;
    DnsResolver resolver_;
};

}  // namespace dbn_pipe
```

**Step 6: Fix SymbologyBuilder OnStartObject to handle "result" entry**

The builder needs to detect when entering the "result" object. Update `OnStartObject`:

```cpp
void OnStartObject() {
    switch (state) {
        case State::Root:
            if (current_key == "result") {
                state = State::InResult;
            }
            break;
        case State::InResult:
            // This is a symbol's array wrapper - ignore
            break;
        case State::InSymbolArray:
            state = State::InInterval;
            current_interval = {};
            break;
        default:
            break;
    }
}
```

**Step 7: Run test to verify it passes**

Run: `bazel test //tests:symbology_client_test`
Expected: PASS

**Step 8: Commit**

```bash
git add src/api/symbology_client.hpp src/api/BUILD.bazel tests/symbology_client_test.cpp tests/BUILD.bazel
git commit -m "feat: add SymbologyClient for bulk symbol resolution"
```

---

## Task 9: Run All Tests and Verify Build

**Step 1: Run all unit tests**

Run: `bazel test //tests:all`
Expected: All tests pass

**Step 2: Build everything**

Run: `bazel build //...`
Expected: BUILD SUCCESS

**Step 3: Commit any fixes**

If any fixes needed, commit them.

---

## Task 10: Update Documentation

**Files:**
- Modify: `docs/api-guide.md`

**Step 1: Add API client documentation**

Add to `docs/api-guide.md`:

```markdown
## Metadata API Client

Query data availability and cost before downloading:

```cpp
#include "src/api/metadata_client.hpp"

// Create client
MetadataClient client(loop, "db-your-api-key");

// Get record count
client.GetRecordCount(
    "GLBX.MDP3",      // dataset
    "ESM4",           // symbols
    "trades",         // schema
    "2025-01-01",     // start
    "2025-01-02",     // end
    "raw_symbol",     // stype_in
    [](auto result) {
        if (result) {
            std::cout << "Record count: " << *result << "\n";
        }
    });

// Get cost estimate
client.GetCost(
    "GLBX.MDP3", "ESM4", "trades",
    "2025-01-01", "2025-01-02", "raw_symbol",
    [](auto result) {
        if (result) {
            std::cout << "Cost: $" << *result << "\n";
        }
    });

// Get dataset date range
client.GetDatasetRange("GLBX.MDP3", [](auto result) {
    if (result) {
        std::cout << "Available: " << result->start
                  << " to " << result->end << "\n";
    }
});
```

## Symbology API Client

Resolve symbols to instrument IDs with date ranges:

```cpp
#include "src/api/symbology_client.hpp"

SymbologyClient client(loop, "db-your-api-key");

client.Resolve(
    "GLBX.MDP3",                          // dataset
    {"ESM4", "ESU4"},                      // symbols
    SType::RawSymbol,                      // stype_in
    SType::InstrumentId,                   // stype_out
    "2025-01-01",                          // start_date
    "2025-12-31",                          // end_date
    [](auto result) {
        if (result) {
            for (const auto& [symbol, mappings] : result->result) {
                for (const auto& m : mappings) {
                    std::cout << symbol << " -> " << m.symbol
                              << " (" << m.start_date << " to "
                              << m.end_date << ")\n";
                }
            }
        }
    });
```

## Error Handling with RetryPolicy

```cpp
#include "src/retry_policy.hpp"

RetryPolicy retry;

void OnApiResult(std::expected<uint64_t, Error> result) {
    if (result) {
        // Success
        std::cout << "Count: " << *result << "\n";
    } else if (retry.ShouldRetry(result.error())) {
        // Retryable error
        retry.RecordAttempt();
        auto delay = retry.GetNextDelay(result.error());
        ScheduleRetry(delay);
    } else {
        // Non-retryable error (auth, validation, etc.)
        std::cerr << "Error: " << result.error().message << "\n";
    }
}
```
```

**Step 2: Commit documentation**

```bash
git add docs/api-guide.md
git commit -m "docs: add MetadataClient and SymbologyClient examples"
```

---

## Summary

| Task | Component | Est. Lines |
|------|-----------|------------|
| 1 | rapidjson dependency | ~30 |
| 2 | URL encoding utilities | ~80 |
| 3 | API error codes | ~20 |
| 4 | RetryPolicy error classification | ~50 |
| 5 | JsonParser SAX component | ~200 |
| 6 | ApiPipeline | ~150 |
| 7 | MetadataClient | ~200 |
| 8 | SymbologyClient | ~200 |
| 9 | Integration testing | - |
| 10 | Documentation | ~100 |

**Total:** ~1030 lines of new code
