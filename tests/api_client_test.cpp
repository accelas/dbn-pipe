// SPDX-License-Identifier: MIT

// tests/api_client_test.cpp
#include <gtest/gtest.h>

#include <expected>
#include <optional>
#include <string>

#include "lib/stream/json_parser.hpp"
#include "src/api_client.hpp"

using namespace dbn_pipe;

// Simple builder that parses a single string value
struct StringBuilder {
    using Result = std::string;
    std::optional<std::string> value;

    void OnKey(std::string_view) {}
    void OnString(std::string_view v) { value = v; }
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
        if (!value) return std::unexpected("no string value");
        return *value;
    }
};

// Verify JsonBuilder concept is satisfied
static_assert(JsonBuilder<StringBuilder>);

TEST(ApiClientTest, CanCreate) {
    // This test just verifies the template compiles correctly
    // Actual network tests would require a mock server

    // We can't create without an event loop, but we can verify types
    using ClientType = ApiClient<StringBuilder>;
    using ResultType = typename ClientType::Result;

    static_assert(std::is_same_v<ResultType, std::string>);
}

TEST(ApiClientTest, ApiRequestBuildsCorrectly) {
    ApiRequest req{
        .method = "GET",
        .path = "/v1/test",
        .host = "api.example.com",
        .port = 443,
        .query_params = {{"key", "value"}},
        .form_params = {},
    };

    std::string http = req.BuildHttpRequest("api.example.com", "test_key");

    EXPECT_TRUE(http.find("GET /v1/test?key=value") != std::string::npos);
    EXPECT_TRUE(http.find("Host: api.example.com") != std::string::npos);
    EXPECT_TRUE(http.find("Authorization: Basic") != std::string::npos);
}

TEST(ApiClientTest, PostRequestWithFormParams) {
    ApiRequest req{
        .method = "POST",
        .path = "/v1/submit",
        .host = "api.example.com",
        .port = 443,
        .query_params = {},
        .form_params = {{"field1", "value1"}, {"field2", "value2"}},
    };

    std::string http = req.BuildHttpRequest("api.example.com", "");

    EXPECT_TRUE(http.find("POST /v1/submit") != std::string::npos);
    EXPECT_TRUE(http.find("Content-Type: application/x-www-form-urlencoded") != std::string::npos);
    EXPECT_TRUE(http.find("field1=value1") != std::string::npos);
    EXPECT_TRUE(http.find("field2=value2") != std::string::npos);
}
