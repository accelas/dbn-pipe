// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#include "dbwriter/batch_writer.hpp"
#include "dbwriter/pg_types.hpp"
#include "dbwriter/table.hpp"
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
constexpr auto test_table = Table{"test",
    Column<"id", int64_t, pg::BigInt>{},
    Column<"value", int32_t, pg::Integer>{},
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
    std::atomic<int>& rows_written;
    std::atomic<int>& batches_finished;

    TestCopyWriter(std::atomic<int>& rw, std::atomic<int>& bf)
        : rows_written(rw), batches_finished(bf) {}

    asio::awaitable<void> start() override { co_return; }
    asio::awaitable<void> write_row(std::span<const std::byte>) override {
        rows_written++;
        co_return;
    }
    asio::awaitable<void> finish() override {
        batches_finished++;
        co_return;
    }
    asio::awaitable<void> abort() override { co_return; }
};

// Simple mock database for testing
class TestDatabase : public IDatabase {
public:
    std::atomic<int> rows_written{0};
    std::atomic<int> batches_finished{0};

    asio::awaitable<QueryResult> query(std::string_view) override {
        co_return QueryResult{};
    }
    asio::awaitable<void> execute(std::string_view) override {
        co_return;
    }
    std::unique_ptr<ICopyWriter> begin_copy(
            std::string_view,
            std::span<const std::string_view>) override {
        return std::make_unique<TestCopyWriter>(rows_written, batches_finished);
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

    // Verify all batches were processed (not dropped)
    EXPECT_TRUE(drain_completed);
    EXPECT_EQ(db.batches_finished.load(), num_batches);
    EXPECT_EQ(db.rows_written.load(), num_batches * batch_size);
    EXPECT_TRUE(writer.is_idle());
    EXPECT_TRUE(writer.is_draining());
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
    EXPECT_EQ(db.batches_finished.load(), 1);
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

    // Less than 10 batches should be finished (some were dropped)
    EXPECT_LT(db.batches_finished.load(), 10);
}

}  // namespace
}  // namespace dbwriter
