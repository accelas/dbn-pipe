// SPDX-License-Identifier: MIT

#include "dbwriter/batch_writer.hpp"
#include "dbwriter/pg_types.hpp"
#include "dbn_pipe/table/table.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>

namespace dbwriter {
namespace {

using namespace std::chrono_literals;

// Test record type
struct TestRecord {
    int64_t id;
    int32_t value;
};

// Test table schema
constexpr auto test_table = dbn_pipe::Table{"test",
    dbn_pipe::Column<"id", dbn_pipe::Int64>{},
    dbn_pipe::Column<"value", dbn_pipe::Int32>{},
};

// Transform function
auto test_transform = [](const TestRecord& r) {
    typename decltype(test_table)::RowType row;
    row.template get<"id">() = r.id;
    row.template get<"value">() = r.value;
    return row;
};

// Simple mock copy writer for testing
class TestCopyWriter : public ICopyWriter {
public:
    std::atomic<int>& write_row_calls;
    std::atomic<int>& copies_finished;

    TestCopyWriter(std::atomic<int>& wr, std::atomic<int>& cf)
        : write_row_calls(wr), copies_finished(cf) {}

    asio::awaitable<void> start() override { co_return; }
    asio::awaitable<void> write_row(std::span<const std::byte>) override {
        write_row_calls++;
        co_return;
    }
    asio::awaitable<void> finish() override {
        copies_finished++;
        co_return;
    }
    asio::awaitable<void> abort() override { co_return; }
};

// Simple mock database for testing
class TestDatabase : public IDatabase {
public:
    std::atomic<int> write_row_calls{0};
    std::atomic<int> copies_finished{0};
    std::atomic<int> execute_calls{0};
    uint64_t execute_count_result = 0;  // Set to control INSERT ... ON CONFLICT return

    asio::awaitable<QueryResult> query(std::string_view) override {
        co_return QueryResult{};
    }
    asio::awaitable<void> execute(std::string_view) override {
        execute_calls++;
        co_return;
    }
    asio::awaitable<uint64_t> execute_count(std::string_view) override {
        co_return execute_count_result;
    }
    std::unique_ptr<ICopyWriter> begin_copy(
            std::string_view,
            std::span<const std::string_view>) override {
        return std::make_unique<TestCopyWriter>(write_row_calls, copies_finished);
    }
    bool is_connected() const override { return true; }
};

// Slow mock copy writer that delays each finish() call
class SlowCopyWriter : public ICopyWriter {
public:
    asio::io_context& ctx;
    std::atomic<int>& write_row_calls;
    std::atomic<int>& copies_finished;
    std::chrono::milliseconds delay;

    SlowCopyWriter(asio::io_context& c, std::atomic<int>& wr, std::atomic<int>& cf,
                   std::chrono::milliseconds d)
        : ctx(c), write_row_calls(wr), copies_finished(cf), delay(d) {}

    asio::awaitable<void> start() override { co_return; }
    asio::awaitable<void> write_row(std::span<const std::byte>) override {
        write_row_calls++;
        co_return;
    }
    asio::awaitable<void> finish() override {
        asio::steady_timer timer(ctx, delay);
        co_await timer.async_wait(asio::use_awaitable);
        copies_finished++;
    }
    asio::awaitable<void> abort() override { co_return; }
};

// Slow mock database for timeout testing
class SlowTestDatabase : public IDatabase {
public:
    asio::io_context& ctx;
    std::atomic<int> write_row_calls{0};
    std::atomic<int> copies_finished{0};
    std::chrono::milliseconds delay;

    SlowTestDatabase(asio::io_context& c, std::chrono::milliseconds d)
        : ctx(c), delay(d) {}

    asio::awaitable<QueryResult> query(std::string_view) override {
        co_return QueryResult{};
    }
    asio::awaitable<void> execute(std::string_view) override {
        co_return;
    }
    asio::awaitable<uint64_t> execute_count(std::string_view) override {
        co_return 0;
    }
    std::unique_ptr<ICopyWriter> begin_copy(
            std::string_view,
            std::span<const std::string_view>) override {
        return std::make_unique<SlowCopyWriter>(ctx, write_row_calls, copies_finished, delay);
    }
    bool is_connected() const override { return true; }
};

TEST(BackpressureConfigTest, Defaults) {
    BackpressureConfig config;

    EXPECT_EQ(config.high_water_mark, 256);
    EXPECT_EQ(config.low_water_mark, 64);
}

TEST(BatchWriterTest, DrainProcessesAllBatches) {
    asio::io_context ctx;
    TestDatabase db;

    using WriterType = BatchWriter<TestRecord, decltype(test_table), decltype(test_transform)>;
    WriterType writer(ctx, db, test_table, test_transform);

    const int num_batches = 5;
    const int batch_size = 10;

    // Enqueue multiple batches
    for (int b = 0; b < num_batches; ++b) {
        std::vector<TestRecord> batch;
        for (int i = 0; i < batch_size; ++i) {
            batch.push_back({.id = b * batch_size + i, .value = i});
        }
        writer.enqueue(std::move(batch));
    }

    EXPECT_EQ(writer.pending_count(), num_batches);
    EXPECT_TRUE(writer.is_writing());

    // Run drain() in a coroutine
    bool drain_completed = false;
    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        co_await writer.drain();
        drain_completed = true;
    }, asio::detached);

    // Run io_context until drain completes
    ctx.run();

    // Verify all records were processed (not dropped).
    // With coalescing, all 5 batches become 1 COPY operation.
    EXPECT_TRUE(drain_completed);
    EXPECT_GE(db.copies_finished.load(), 1);
    EXPECT_TRUE(writer.is_idle());
    EXPECT_TRUE(writer.is_draining());

    // Stats should reflect coalescing
    auto s = writer.stats();
    EXPECT_EQ(s.batches_coalesced, num_batches);
    EXPECT_EQ(s.rows_copied, num_batches * batch_size);
}

TEST(BatchWriterTest, DrainRejectsNewWork) {
    asio::io_context ctx;
    TestDatabase db;

    using WriterType = BatchWriter<TestRecord, decltype(test_table), decltype(test_transform)>;
    WriterType writer(ctx, db, test_table, test_transform);

    // Enqueue one batch
    writer.enqueue({{.id = 1, .value = 1}});

    // Start draining
    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        co_await writer.drain();
    }, asio::detached);

    // Run until drain flag is set (more deterministic than single poll)
    while (!writer.is_draining()) {
        ctx.poll();
    }

    // Try to enqueue after drain started - should be rejected
    writer.enqueue({{.id = 2, .value = 2}});  // This should be ignored

    // Complete the drain
    ctx.run();

    // Verify only the first batch was processed
    EXPECT_EQ(db.copies_finished.load(), 1);
    EXPECT_TRUE(writer.is_draining());
}

TEST(BatchWriterTest, RequestStopDropsPendingWork) {
    asio::io_context ctx;
    TestDatabase db;

    using WriterType = BatchWriter<TestRecord, decltype(test_table), decltype(test_transform)>;
    WriterType writer(ctx, db, test_table, test_transform);

    // Enqueue multiple batches
    for (int i = 0; i < 10; ++i) {
        writer.enqueue({{.id = i, .value = i}});
    }

    EXPECT_EQ(writer.pending_count(), 10);

    // Request stop - should clear pending
    writer.request_stop();

    EXPECT_EQ(writer.pending_count(), 0);
    EXPECT_TRUE(writer.stop_requested());

    // Run to completion
    ctx.run();

    // With coalescing + stop, at most 1 COPY fires (or 0 if stopped fast enough)
    EXPECT_LE(db.copies_finished.load(), 1);
}

TEST(BatchWriterTest, DrainTimeoutStopsEarly) {
    asio::io_context ctx;
    SlowTestDatabase db(ctx, 1500ms);  // 1500ms per COPY (exceeds 1s drain timeout)

    using WriterType = BatchWriter<TestRecord, decltype(test_table), decltype(test_transform)>;
    WriterType writer(ctx, db, test_table, test_transform);

    // Enqueue 50 batches (50 * 200ms = 10s total without timeout)
    for (int i = 0; i < 50; ++i) {
        writer.enqueue({{.id = i, .value = i}});
    }

    // Drain with 1-second timeout — should stop well before all 50 batches
    bool drain_completed = false;
    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        co_await writer.drain(std::chrono::seconds{1});
        drain_completed = true;
    }, asio::detached);

    ctx.run();

    EXPECT_TRUE(drain_completed);
    EXPECT_LT(db.copies_finished.load(), 50);  // Timed out, not all processed
    EXPECT_TRUE(writer.stop_requested());       // Timeout triggers stopping_
}

TEST(BatchWriterTest, DrainZeroTimeoutProcessesAll) {
    // Default timeout=0 means no timeout — should process everything
    asio::io_context ctx;
    TestDatabase db;

    using WriterType = BatchWriter<TestRecord, decltype(test_table), decltype(test_transform)>;
    WriterType writer(ctx, db, test_table, test_transform);

    for (int i = 0; i < 5; ++i) {
        writer.enqueue({{.id = i, .value = i}});
    }

    bool drain_completed = false;
    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        co_await writer.drain(std::chrono::seconds{0});
        drain_completed = true;
    }, asio::detached);

    ctx.run();

    EXPECT_TRUE(drain_completed);
    EXPECT_GE(db.copies_finished.load(), 1);  // All records processed via coalesced copies
    EXPECT_FALSE(writer.stop_requested());
}

}  // namespace
}  // namespace dbwriter
