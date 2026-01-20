// tests/api_error_test.cpp
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

TEST(ApiErrorTest, RetryAfterDefaultsToEmpty) {
    Error e{ErrorCode::RateLimited, "Too many requests"};
    EXPECT_FALSE(e.retry_after.has_value());
}

TEST(ApiErrorTest, ApiErrorCategories) {
    EXPECT_EQ(error_category(ErrorCode::Unauthorized), "api");
    EXPECT_EQ(error_category(ErrorCode::RateLimited), "api");
    EXPECT_EQ(error_category(ErrorCode::NotFound), "api");
    EXPECT_EQ(error_category(ErrorCode::ValidationError), "api");
    EXPECT_EQ(error_category(ErrorCode::ServerError), "api");
}

}  // namespace
}  // namespace dbn_pipe
