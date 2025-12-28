// tests/cram_auth_test.cpp
#include <gtest/gtest.h>

#include "src/cram_auth.hpp"

using namespace databento_async;

TEST(CramAuthTest, ParseGreeting) {
    std::string greeting = "lsg-test|20231015\n";
    auto result = CramAuth::ParseGreeting(greeting);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->session_id, "lsg-test");
    EXPECT_EQ(result->version, "20231015");
}

TEST(CramAuthTest, ParseGreetingWithoutNewline) {
    std::string greeting = "session123|v2";
    auto result = CramAuth::ParseGreeting(greeting);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->session_id, "session123");
    EXPECT_EQ(result->version, "v2");
}

TEST(CramAuthTest, ParseGreetingMissingPipe) {
    std::string greeting = "lsg-test-20231015\n";
    auto result = CramAuth::ParseGreeting(greeting);

    ASSERT_FALSE(result.has_value());
}

TEST(CramAuthTest, ParseChallenge) {
    std::string challenge = "cram=abcdef123456\n";
    auto result = CramAuth::ParseChallenge(challenge);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "abcdef123456");
}

TEST(CramAuthTest, ParseChallengeWithoutNewline) {
    std::string challenge = "cram=xyz789";
    auto result = CramAuth::ParseChallenge(challenge);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "xyz789");
}

TEST(CramAuthTest, ParseChallengeMissingPrefix) {
    std::string challenge = "challenge=abcdef123456\n";
    auto result = CramAuth::ParseChallenge(challenge);

    ASSERT_FALSE(result.has_value());
}

TEST(CramAuthTest, ComputeResponse) {
    // Known test vector: echo -n "test_challenge|test_api_key" | sha256sum
    // = b769a86a50ffcaf90cfabf1189838ff2378f12e909075298cdb75ec91943b23f
    std::string challenge = "test_challenge";
    std::string api_key = "test_api_key";

    std::string response = CramAuth::ComputeResponse(challenge, api_key);

    EXPECT_EQ(response, "b769a86a50ffcaf90cfabf1189838ff2378f12e909075298cdb75ec91943b23f");
}

TEST(CramAuthTest, ComputeResponseDeterministic) {
    // Same inputs should produce same output
    std::string challenge = "reproducible_challenge";
    std::string api_key = "reproducible_key";

    std::string response1 = CramAuth::ComputeResponse(challenge, api_key);
    std::string response2 = CramAuth::ComputeResponse(challenge, api_key);

    EXPECT_EQ(response1, response2);
}

TEST(CramAuthTest, ComputeResponseDifferentInputs) {
    // Different inputs should produce different outputs
    std::string response1 = CramAuth::ComputeResponse("challenge1", "key1");
    std::string response2 = CramAuth::ComputeResponse("challenge2", "key2");

    EXPECT_NE(response1, response2);
}

TEST(CramAuthTest, FormatAuthMessage) {
    std::string msg = CramAuth::FormatAuthMessage(
        "db-abc123",
        "CRAM-SHA256",
        "deadbeef01234567"
    );

    EXPECT_EQ(msg, "auth=db-abc123|CRAM-SHA256|deadbeef01234567\n");
}

TEST(CramAuthTest, FormatAuthMessageEmptyValues) {
    std::string msg = CramAuth::FormatAuthMessage("", "", "");

    EXPECT_EQ(msg, "auth=||\n");
}
