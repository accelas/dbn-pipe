// SPDX-License-Identifier: MIT

// tests/retry_policy_test.cpp
#include <gtest/gtest.h>
#include "lib/stream/error.hpp"
#include "src/retry_policy.hpp"

using namespace dbn_pipe;

TEST(RetryPolicyTest, ShouldRetryInitiallyTrue) {
    RetryConfig config{.max_retries = 3};
    RetryPolicy policy(config);
    EXPECT_TRUE(policy.ShouldRetry());
}

TEST(RetryPolicyTest, ShouldRetryFalseAfterMaxAttempts) {
    RetryConfig config{.max_retries = 2};
    RetryPolicy policy(config);

    policy.RecordAttempt();
    EXPECT_TRUE(policy.ShouldRetry());

    policy.RecordAttempt();
    EXPECT_FALSE(policy.ShouldRetry());
}

TEST(RetryPolicyTest, ResetClearsAttempts) {
    RetryConfig config{.max_retries = 2};
    RetryPolicy policy(config);

    policy.RecordAttempt();
    policy.RecordAttempt();
    EXPECT_FALSE(policy.ShouldRetry());

    policy.Reset();
    EXPECT_TRUE(policy.ShouldRetry());
}

TEST(RetryPolicyTest, GetNextDelayRespectsRetryAfterHeader) {
    RetryConfig config{.initial_delay = std::chrono::milliseconds(1000)};
    RetryPolicy policy(config);

    auto delay = policy.GetNextDelay(std::chrono::seconds(30));
    EXPECT_EQ(delay.count(), 30000);  // 30 seconds in ms
}

TEST(RetryPolicyTest, GetNextDelayUsesExponentialBackoff) {
    RetryConfig config{
        .initial_delay = std::chrono::milliseconds(100),
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.0  // No jitter for predictable test
    };
    RetryPolicy policy(config);

    // First attempt: 100ms
    auto delay0 = policy.GetNextDelay();
    EXPECT_EQ(delay0.count(), 100);

    policy.RecordAttempt();

    // Second attempt: 200ms
    auto delay1 = policy.GetNextDelay();
    EXPECT_EQ(delay1.count(), 200);

    policy.RecordAttempt();

    // Third attempt: 400ms
    auto delay2 = policy.GetNextDelay();
    EXPECT_EQ(delay2.count(), 400);
}

TEST(RetryPolicyTest, GetNextDelayCapsAtMaxDelay) {
    RetryConfig config{
        .initial_delay = std::chrono::milliseconds(1000),
        .max_delay = std::chrono::milliseconds(5000),
        .backoff_multiplier = 10.0,
        .jitter_factor = 0.0
    };
    RetryPolicy policy(config);

    policy.RecordAttempt();  // Would be 10000ms without cap

    auto delay = policy.GetNextDelay();
    EXPECT_EQ(delay.count(), 5000);  // Capped at max
}

// Error-aware RetryPolicy tests

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

TEST(RetryPolicyTest, ShouldRetryTlsHandshakeFailed) {
    RetryPolicy policy;
    Error e{ErrorCode::TlsHandshakeFailed, "TLS error"};
    EXPECT_TRUE(policy.ShouldRetry(e));
}

TEST(RetryPolicyTest, ShouldNotRetryParseError) {
    RetryPolicy policy;
    Error e{ErrorCode::ParseError, "Invalid response"};
    EXPECT_FALSE(policy.ShouldRetry(e));
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
