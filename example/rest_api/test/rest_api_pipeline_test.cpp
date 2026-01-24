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
