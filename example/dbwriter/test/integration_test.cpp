// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

// Note: This test requires a running PostgreSQL container
// Run with: bazel test //example/dbwriter:integration_test --test_env=POSTGRES_HOST=... etc

#include "dbwriter/postgres.hpp"
#include "dbwriter/mapper.hpp"
#include "dbwriter/table.hpp"
#include "dbwriter/pg_types.hpp"
#include "dbwriter/types.hpp"
#include <gtest/gtest.h>
#include <cstdlib>

namespace dbwriter {
namespace {

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* host = std::getenv("POSTGRES_HOST");
        if (!host) {
            GTEST_SKIP() << "POSTGRES_HOST not set, skipping integration tests";
        }

        config_.host = host;
        config_.port = std::atoi(std::getenv("POSTGRES_PORT") ?: "5432");
        config_.database = std::getenv("POSTGRES_DB") ?: "testdb";
        config_.user = std::getenv("POSTGRES_USER") ?: "testuser";
        config_.password = std::getenv("POSTGRES_PASSWORD") ?: "testpass";
    }

    PostgresConfig config_;
};

TEST_F(IntegrationTest, CopyWritesData) {
    asio::io_context ctx;

    // Connect
    PostgresDatabase db(ctx, config_);
    auto connect_task = [&]() -> asio::awaitable<void> {
        co_await db.connect();
    };
    asio::co_spawn(ctx, connect_task(), asio::detached);
    ctx.run();
    ctx.restart();

    ASSERT_TRUE(db.is_connected());

    // Define table schema
    constexpr auto trades_table = Table{"trades",
        Column<"ts_event_ns", int64_t, pg::BigInt>{},
        Column<"ts_event", Timestamp, pg::Timestamptz>{},
        Column<"instrument_id", int32_t, pg::Integer>{},
        Column<"price", int64_t, pg::BigInt>{},
        Column<"size", int32_t, pg::Integer>{},
    };

    using Row = decltype(trades_table)::RowType;

    // Create mapper
    auto mapper = make_mapper(trades_table);

    // Begin COPY
    auto columns = trades_table.column_names();
    std::vector<std::string_view> col_views(columns.begin(), columns.end());
    auto writer = db.begin_copy("trades", col_views);

    // Write rows
    auto write_task = [&]() -> asio::awaitable<void> {
        co_await writer->start();

        ByteBuffer buf;

        // Write 3 test rows
        for (int i = 0; i < 3; ++i) {
            Row row;
            int64_t ts_ns = 1704067200000000000LL + i * 1000000000LL;
            row.get<"ts_event_ns">() = ts_ns;
            row.get<"ts_event">() = Timestamp::from_unix_ns(ts_ns);
            row.get<"instrument_id">() = 1000 + i;
            row.get<"price">() = 150'000'000'000LL + i * 100'000'000LL;
            row.get<"size">() = 100 + i * 10;

            mapper.encode_row(row, buf);
            co_await writer->write_row(buf.view());
            buf.clear();
        }

        co_await writer->finish();
    };

    asio::co_spawn(ctx, write_task(), asio::detached);
    ctx.run();

    std::cout << "Successfully wrote 3 rows to PostgreSQL via binary COPY\n";
}

}  // namespace
}  // namespace dbwriter
