// SPDX-License-Identifier: MIT

// example/rest_api/test/rest_api_pipeline_test.cpp
#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
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

// Integration test using localhost echo server pattern
// This tests the full fetch flow without external network

TEST(RestApiPipelineTest, FetchReturnsErrorForUnreachableHost) {
    // Ignore SIGPIPE since socket operations can generate it
    signal(SIGPIPE, SIG_IGN);

    asio::io_context ctx;
    RestApiPipeline<TestBuilder> pipeline(ctx, "localhost", 1);  // Port 1 should fail

    bool completed = false;
    std::expected<std::string, dbn_pipe::Error> result_holder;

    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        auto result = co_await pipeline.fetch("/test", {}, {});
        result_holder = std::move(result);
        completed = true;
        // Stop the context once we have a result
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    }, [&](std::exception_ptr) { ctx.stop(); });

    // Add a timeout guard to prevent infinite hangs
    asio::steady_timer timeout(ctx, std::chrono::seconds(10));
    timeout.async_wait([&](auto) { ctx.stop(); });

    ctx.run();

    ASSERT_TRUE(completed) << "Fetch did not complete within timeout";
    ASSERT_FALSE(result_holder.has_value()) << "Expected error for unreachable host";
}

TEST(RestApiPipelineTest, FetchReturnsErrorForInvalidHost) {
    // Ignore SIGPIPE since socket operations can generate it
    signal(SIGPIPE, SIG_IGN);

    asio::io_context ctx;
    RestApiPipeline<TestBuilder> pipeline(ctx, "this.host.does.not.exist.invalid");

    bool completed = false;
    std::expected<std::string, dbn_pipe::Error> result_holder;

    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        auto result = co_await pipeline.fetch("/test", {}, {});
        result_holder = std::move(result);
        completed = true;
        // Stop the context once we have a result
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    }, [&](std::exception_ptr) { ctx.stop(); });

    // Add a timeout guard to prevent infinite hangs
    asio::steady_timer timeout(ctx, std::chrono::seconds(10));
    timeout.async_wait([&](auto) { ctx.stop(); });

    ctx.run();

    ASSERT_TRUE(completed) << "Fetch did not complete within timeout";
    ASSERT_FALSE(result_holder.has_value()) << "Expected error for invalid host";
}

}  // namespace
}  // namespace rest_api
