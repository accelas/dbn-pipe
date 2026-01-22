// tests/url_encode_test.cpp
#include <gtest/gtest.h>
#include <sstream>
#include "lib/stream/url_encode.hpp"

namespace dbn_pipe {
namespace {

TEST(UrlEncodeTest, AlphanumericPassthrough) {
    std::ostringstream out;
    UrlEncode(out, "abc123XYZ");
    EXPECT_EQ(out.str(), "abc123XYZ");
}

TEST(UrlEncodeTest, SpecialCharsEncoded) {
    std::ostringstream out;
    UrlEncode(out, "hello world");
    EXPECT_EQ(out.str(), "hello%20world");
}

TEST(UrlEncodeTest, SymbolsEncoded) {
    std::ostringstream out;
    UrlEncode(out, "SPY,QQQ");
    EXPECT_EQ(out.str(), "SPY%2CQQQ");
}

TEST(UrlEncodeTest, SafeCharsPassthrough) {
    std::ostringstream out;
    UrlEncode(out, "a-b_c.d~e");
    EXPECT_EQ(out.str(), "a-b_c.d~e");
}

TEST(UrlEncodeTest, EmptyString) {
    std::ostringstream out;
    UrlEncode(out, "");
    EXPECT_EQ(out.str(), "");
}

TEST(UrlEncodeTest, NonAsciiCharsEncoded) {
    std::ostringstream out;
    UrlEncode(out, "\xc3\xa9");  // UTF-8 for 'e-acute'
    EXPECT_EQ(out.str(), "%C3%A9");
}

TEST(Base64EncodeTest, BasicString) {
    std::ostringstream out;
    Base64Encode(out, "test");
    EXPECT_EQ(out.str(), "dGVzdA==");
}

TEST(Base64EncodeTest, ApiKeyFormat) {
    std::ostringstream out;
    Base64Encode(out, "db-abc123:");
    EXPECT_EQ(out.str(), "ZGItYWJjMTIzOg==");
}

TEST(Base64EncodeTest, EmptyString) {
    std::ostringstream out;
    Base64Encode(out, "");
    EXPECT_EQ(out.str(), "");
}

}  // namespace
}  // namespace dbn_pipe
