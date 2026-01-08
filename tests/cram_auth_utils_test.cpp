// tests/cram_auth_utils_test.cpp
#include <gtest/gtest.h>

#include "src/cram_auth.hpp"

using namespace dbn_pipe;

TEST(CramAuthUtilsTest, ParseGreeting) {
    std::string greeting = "lsg-test|20231015\n";
    auto result = CramAuthUtils::ParseGreeting(greeting);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->session_id, "lsg-test");
    EXPECT_EQ(result->version, "20231015");
}

TEST(CramAuthUtilsTest, ParseGreetingWithoutNewline) {
    std::string greeting = "session123|v2";
    auto result = CramAuthUtils::ParseGreeting(greeting);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->session_id, "session123");
    EXPECT_EQ(result->version, "v2");
}

TEST(CramAuthUtilsTest, ParseGreetingMissingPipe) {
    // New format: greetings without pipe are accepted (version only)
    std::string greeting = "lsg-test-20231015\n";
    auto result = CramAuthUtils::ParseGreeting(greeting);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->session_id.empty());  // No session_id in new format
    EXPECT_EQ(result->version, "lsg-test-20231015");  // Whole line stored as version
}

TEST(CramAuthUtilsTest, ParseGreetingNewFormat) {
    // New LSG format: lsg_version=X.Y.Z (build)
    std::string greeting = "lsg_version=0.7.2 (5)\n";
    auto result = CramAuthUtils::ParseGreeting(greeting);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->session_id.empty());  // Session ID comes from auth response
    EXPECT_EQ(result->version, "0.7.2");  // Parenthetical suffix stripped
}

TEST(CramAuthUtilsTest, ParseChallenge) {
    std::string challenge = "cram=abcdef123456\n";
    auto result = CramAuthUtils::ParseChallenge(challenge);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "abcdef123456");
}

TEST(CramAuthUtilsTest, ParseChallengeWithoutNewline) {
    std::string challenge = "cram=xyz789";
    auto result = CramAuthUtils::ParseChallenge(challenge);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "xyz789");
}

TEST(CramAuthUtilsTest, ParseChallengeMissingPrefix) {
    std::string challenge = "challenge=abcdef123456\n";
    auto result = CramAuthUtils::ParseChallenge(challenge);

    ASSERT_FALSE(result.has_value());
}

TEST(CramAuthUtilsTest, ComputeResponse) {
    // Known test vector: echo -n "test_challenge|test_api_key" | sha256sum
    // = b769a86a50ffcaf90cfabf1189838ff2378f12e909075298cdb75ec91943b23f
    // Plus bucket_id suffix: last 5 chars of api_key = "i_key"
    std::string challenge = "test_challenge";
    std::string api_key = "test_api_key";

    std::string response = CramAuthUtils::ComputeResponse(challenge, api_key);

    EXPECT_EQ(response, "b769a86a50ffcaf90cfabf1189838ff2378f12e909075298cdb75ec91943b23f-i_key");
}

TEST(CramAuthUtilsTest, ComputeResponseDeterministic) {
    // Same inputs should produce same output
    std::string challenge = "reproducible_challenge";
    std::string api_key = "reproducible_key";

    std::string response1 = CramAuthUtils::ComputeResponse(challenge, api_key);
    std::string response2 = CramAuthUtils::ComputeResponse(challenge, api_key);

    EXPECT_EQ(response1, response2);
}

TEST(CramAuthUtilsTest, ComputeResponseDifferentInputs) {
    // Different inputs should produce different outputs
    std::string response1 = CramAuthUtils::ComputeResponse("challenge1", "key1");
    std::string response2 = CramAuthUtils::ComputeResponse("challenge2", "key2");

    EXPECT_NE(response1, response2);
}
