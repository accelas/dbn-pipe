// tests/retry_policy_test.cpp
#include <gtest/gtest.h>
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
