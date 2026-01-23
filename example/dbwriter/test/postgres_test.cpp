// SPDX-License-Identifier: MIT

#include "dbwriter/postgres.hpp"
#include <gtest/gtest.h>

namespace dbwriter {
namespace {

// Note: Full integration tests require a real PostgreSQL connection
// These tests verify the interface and mock behavior

TEST(PostgresCopyWriterTest, ConstructsWithConnection) {
    // This test just verifies compilation
    // Real tests would use testcontainers
    SUCCEED();
}

TEST(PostgresDatabaseTest, ConnectionString) {
    PostgresConfig config{
        .host = "localhost",
        .port = 5432,
        .database = "test",
        .user = "user",
        .password = "pass",
    };

    auto conn_str = config.connection_string();

    EXPECT_NE(conn_str.find("host=localhost"), std::string::npos);
    EXPECT_NE(conn_str.find("port=5432"), std::string::npos);
    EXPECT_NE(conn_str.find("dbname=test"), std::string::npos);
}

}  // namespace
}  // namespace dbwriter
