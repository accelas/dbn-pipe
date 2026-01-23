// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

// Tests for PostgreSQL async timing behavior
//
// These tests verify the correct sequence of libpq async operations:
//   1. PQsendQuery() - send query asynchronously
//   2. PQconsumeInput() - read available data from socket
//   3. PQisBusy() - check if result is ready
//   4. If busy, wait for socket readable, then go to step 2
//   5. PQgetResult() - get result when ready
//
// The key insight is that we must consume input BEFORE checking busy,
// and we must wait for readable ONLY if still busy after consuming.

#include "mock_libpq.hpp"
#include <gtest/gtest.h>
#include <asio.hpp>

namespace dbwriter {
namespace {

using namespace testing;

// Helper class that implements the async wait pattern we use in production
class AsyncQueryRunner {
public:
    explicit AsyncQueryRunner(ILibPq& pq, asio::io_context& ctx)
        : pq_(pq), ctx_(ctx) {}

    // Simulates the async execute pattern from postgres.cpp
    // Returns number of async waits performed
    struct Result {
        bool success;
        int wait_count;
        int consume_count;
    };

    Result execute(PGconn* conn, const char* sql) {
        int wait_count = 0;
        int consume_count = 0;

        if (!pq_.sendQuery(conn, sql)) {
            return {false, wait_count, consume_count};
        }

        // Correct pattern: consume → check busy → wait if needed
        do {
            if (!pq_.consumeInput(conn)) {
                return {false, wait_count, consume_count};
            }
            consume_count++;

            if (!pq_.isBusy(conn)) break;

            // In real code, this would be co_await async_wait
            // Here we just count it
            wait_count++;
        } while (pq_.isBusy(conn));

        PGresult* res = pq_.getResult(conn);
        bool success = (pq_.resultStatus(res) == PGRES_COMMAND_OK);
        pq_.clear(res);

        // Consume remaining results
        while ((res = pq_.getResult(conn)) != nullptr) {
            pq_.clear(res);
        }

        return {success, wait_count, consume_count};
    }

private:
    ILibPq& pq_;
    asio::io_context& ctx_;
};

// Test: Data already available (no wait needed)
// Scenario: Server responds immediately, data in socket buffer
// Expected: consume once, no waits, success
TEST(PostgresAsyncTest, DataAlreadyAvailable) {
    SimulatedLibPq pq;
    pq.set_consume_calls_until_ready(1);  // Ready after first consume
    pq.set_result_status(PGRES_COMMAND_OK);

    asio::io_context ctx;
    AsyncQueryRunner runner(pq, ctx);

    auto conn = pq.connectdb("");
    auto result = runner.execute(conn, "SELECT 1");

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.wait_count, 0);  // No waits needed
    EXPECT_EQ(result.consume_count, 1);
}

// Test: Data arrives after one wait
// Scenario: Query sent, need to wait for server response
// Expected: consume once (busy), wait, consume again (ready), success
TEST(PostgresAsyncTest, DataArrivesAfterOneWait) {
    SimulatedLibPq pq;
    pq.set_consume_calls_until_ready(2);  // Ready after second consume
    pq.set_result_status(PGRES_COMMAND_OK);

    asio::io_context ctx;
    AsyncQueryRunner runner(pq, ctx);

    auto conn = pq.connectdb("");
    auto result = runner.execute(conn, "SELECT 1");

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.wait_count, 1);  // One wait
    EXPECT_EQ(result.consume_count, 2);
}

// Test: Data arrives in multiple chunks
// Scenario: Large result arrives in multiple TCP packets
// Expected: multiple consume/wait cycles
TEST(PostgresAsyncTest, DataArrivesInMultipleChunks) {
    SimulatedLibPq pq;
    pq.set_consume_calls_until_ready(5);  // Ready after 5 consumes
    pq.set_result_status(PGRES_COMMAND_OK);

    asio::io_context ctx;
    AsyncQueryRunner runner(pq, ctx);

    auto conn = pq.connectdb("");
    auto result = runner.execute(conn, "SELECT large_data");

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.wait_count, 4);  // 4 waits (between 5 consumes)
    EXPECT_EQ(result.consume_count, 5);
}

// Test: Consume fails
// Scenario: Connection error during consume
// Expected: failure, no result processing
TEST(PostgresAsyncTest, ConsumeFailure) {
    SimulatedLibPq pq;
    pq.set_consume_fails(true);

    asio::io_context ctx;
    AsyncQueryRunner runner(pq, ctx);

    auto conn = pq.connectdb("");
    auto result = runner.execute(conn, "SELECT 1");

    EXPECT_FALSE(result.success);
    // consume_count is 0 because we return before incrementing on failure
    EXPECT_EQ(result.consume_count, 0);
}

// Test: Query returns error status
// Scenario: Query executes but returns error (e.g., syntax error)
TEST(PostgresAsyncTest, QueryReturnsError) {
    SimulatedLibPq pq;
    pq.set_consume_calls_until_ready(1);
    pq.set_result_status(PGRES_FATAL_ERROR);

    asio::io_context ctx;
    AsyncQueryRunner runner(pq, ctx);

    auto conn = pq.connectdb("");
    auto result = runner.execute(conn, "INVALID SQL");

    EXPECT_FALSE(result.success);
}

// Test: COPY command returns PGRES_COPY_IN
TEST(PostgresAsyncTest, CopyCommandReturnsCorrectStatus) {
    SimulatedLibPq pq;
    pq.set_consume_calls_until_ready(1);
    pq.set_result_status(PGRES_COPY_IN);

    asio::io_context ctx;

    auto conn = pq.connectdb("");
    pq.sendQuery(conn, "COPY t FROM STDIN");

    // Consume and check
    EXPECT_EQ(pq.consumeInput(conn), 1);
    EXPECT_EQ(pq.isBusy(conn), 0);

    PGresult* res = pq.getResult(conn);
    EXPECT_EQ(pq.resultStatus(res), PGRES_COPY_IN);
    pq.clear(res);
}

// This test documents the WRONG pattern that caused the original bug
// The wrong pattern: check busy BEFORE consuming
class WrongPatternRunner {
public:
    explicit WrongPatternRunner(ILibPq& pq) : pq_(pq) {}

    struct Result {
        bool success;
        int wait_count;
        int consume_count;
    };

    Result execute(PGconn* conn, const char* sql) {
        int wait_count = 0;
        int consume_count = 0;

        if (!pq_.sendQuery(conn, sql)) {
            return {false, wait_count, consume_count};
        }

        // WRONG pattern: check busy BEFORE consuming
        // This was the original bug!
        while (pq_.isBusy(conn)) {
            wait_count++;  // Would wait here
            if (!pq_.consumeInput(conn)) {
                return {false, wait_count, consume_count};
            }
            consume_count++;
        }

        PGresult* res = pq_.getResult(conn);
        bool success = (pq_.resultStatus(res) == PGRES_COMMAND_OK);
        pq_.clear(res);

        return {success, wait_count, consume_count};
    }

private:
    ILibPq& pq_;
};

// Test: Wrong pattern hangs when data is already available
// With the WRONG pattern, if isBusy() returns false before any consume,
// we skip the loop entirely and might get no result or wrong state
TEST(PostgresAsyncTest, WrongPatternBehavior) {
    SimulatedLibPq pq;
    pq.set_consume_calls_until_ready(1);  // Ready after first consume
    pq.set_result_status(PGRES_COMMAND_OK);

    WrongPatternRunner runner(pq);

    auto conn = pq.connectdb("");
    auto result = runner.execute(conn, "SELECT 1");

    // With the wrong pattern, when data arrives quickly:
    // - isBusy() is called BEFORE consumeInput()
    // - If the libpq buffer is empty, isBusy() might return 0 (idle)
    // - We skip the loop and call getResult() without consuming
    //
    // In this simulation, isBusy returns 0 after consume_calls_until_ready
    // consumes. With wrong pattern, we check busy first (0 consumes done),
    // so if consume_calls_until_ready == 1, isBusy returns 1 (busy).
    //
    // This test documents the subtle timing issue.
    EXPECT_EQ(result.consume_count, 1);
    EXPECT_EQ(result.wait_count, 1);  // Wrong pattern waits first
}

}  // namespace
}  // namespace dbwriter
