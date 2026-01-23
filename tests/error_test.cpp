// SPDX-License-Identifier: MIT

// tests/error_test.cpp
#include <cerrno>

#include <gtest/gtest.h>

#include "lib/stream/error.hpp"

using namespace dbn_pipe;

TEST(ErrorTest, Construction) {
    Error err{ErrorCode::ConnectionFailed, "connection refused"};
    EXPECT_EQ(err.code, ErrorCode::ConnectionFailed);
    EXPECT_EQ(err.message, "connection refused");
    EXPECT_EQ(err.os_errno, 0);
}

TEST(ErrorTest, WithErrno) {
    Error err{ErrorCode::ConnectionFailed, "connection refused", ECONNREFUSED};
    EXPECT_EQ(err.code, ErrorCode::ConnectionFailed);
    EXPECT_EQ(err.os_errno, ECONNREFUSED);
}

TEST(ErrorTest, CategoryString) {
    // Connection category
    EXPECT_EQ(error_category(ErrorCode::ConnectionFailed), "connection");
    EXPECT_EQ(error_category(ErrorCode::ConnectionClosed), "connection");
    EXPECT_EQ(error_category(ErrorCode::DnsResolutionFailed), "connection");

    // Auth category
    EXPECT_EQ(error_category(ErrorCode::AuthFailed), "auth");
    EXPECT_EQ(error_category(ErrorCode::InvalidApiKey), "auth");

    // Protocol category
    EXPECT_EQ(error_category(ErrorCode::InvalidGreeting), "protocol");
    EXPECT_EQ(error_category(ErrorCode::InvalidChallenge), "protocol");
    EXPECT_EQ(error_category(ErrorCode::ParseError), "protocol");

    // Subscription category
    EXPECT_EQ(error_category(ErrorCode::InvalidDataset), "subscription");
    EXPECT_EQ(error_category(ErrorCode::InvalidSymbol), "subscription");
    EXPECT_EQ(error_category(ErrorCode::InvalidSchema), "subscription");

    // TLS category
    EXPECT_EQ(error_category(ErrorCode::TlsHandshakeFailed), "tls");
    EXPECT_EQ(error_category(ErrorCode::CertificateError), "tls");

    // HTTP category
    EXPECT_EQ(error_category(ErrorCode::HttpError), "http");

    // Decompression category
    EXPECT_EQ(error_category(ErrorCode::DecompressionError), "decompression");
}
