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
