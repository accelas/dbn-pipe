// SPDX-License-Identifier: MIT

// tests/url_encode_test.cpp
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include "lib/stream/url_encode.hpp"

namespace dbn_pipe {
namespace {

TEST(UrlEncodeTest, AlphanumericPassthrough) {
    std::string out;
    UrlEncode(std::back_inserter(out), "abc123XYZ");
    EXPECT_EQ(out, "abc123XYZ");
}

TEST(UrlEncodeTest, SpecialCharsEncoded) {
    std::string out;
    UrlEncode(std::back_inserter(out), "hello world");
    EXPECT_EQ(out, "hello%20world");
}

TEST(UrlEncodeTest, SymbolsEncoded) {
    std::string out;
    UrlEncode(std::back_inserter(out), "SPY,QQQ");
    EXPECT_EQ(out, "SPY%2CQQQ");
}

TEST(UrlEncodeTest, SafeCharsPassthrough) {
    std::string out;
    UrlEncode(std::back_inserter(out), "a-b_c.d~e");
    EXPECT_EQ(out, "a-b_c.d~e");
}

TEST(UrlEncodeTest, EmptyString) {
    std::string out;
    UrlEncode(std::back_inserter(out), "");
    EXPECT_EQ(out, "");
}

TEST(UrlEncodeTest, NonAsciiCharsEncoded) {
    std::string out;
    UrlEncode(std::back_inserter(out), "\xc3\xa9");  // UTF-8 for 'e-acute'
    EXPECT_EQ(out, "%C3%A9");
}

TEST(Base64EncodeTest, BasicString) {
    std::string out;
    Base64Encode(std::back_inserter(out), "test");
    EXPECT_EQ(out, "dGVzdA==");
}

TEST(Base64EncodeTest, ApiKeyFormat) {
    std::string out;
    Base64Encode(std::back_inserter(out), "db-abc123:");
    EXPECT_EQ(out, "ZGItYWJjMTIzOg==");
}

TEST(Base64EncodeTest, EmptyString) {
    std::string out;
    Base64Encode(std::back_inserter(out), "");
    EXPECT_EQ(out, "");
}

}  // namespace
}  // namespace dbn_pipe
