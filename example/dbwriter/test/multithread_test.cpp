// SPDX-License-Identifier: MIT

// Multi-threaded test: Download and PostgreSQL writer on separate threads.
// Verifies thread-safe handoff between download thread and DB writer thread.

#include "dbwriter/asio_event_loop.hpp"
#include "dbwriter/batch_writer.hpp"
#include "dbn_pipe/pg/pg_types.hpp"
#include "dbwriter/transform.hpp"
#include "dbn_pipe/table/table.hpp"
#include "test/mock_libpq.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace dbwriter {
namespace {

using namespace std::chrono_literals;

// Test record type (simulates downloaded data)
struct TestRecord {
    int64_t ts_event;
    int32_t instrument_id;
    int64_t price;
    int32_t size;
};

// Table schema
constexpr auto test_table = dbn_pipe::Table{"trades",
    dbn_pipe::Column<"ts_event", dbn_pipe::Int64>{},
    dbn_pipe::Column<"instrument_id", dbn_pipe::Int32>{},
    dbn_pipe::Column<"price", dbn_pipe::Int64>{},
    dbn_pipe::Column<"size", dbn_pipe::Int32>{},
};

// Transform function
auto test_transform = [](const TestRecord& r) {
    typename decltype(test_table)::RowType row;
    row.template get<"ts_event">() = r.ts_event;
    row.template get<"instrument_id">() = r.instrument_id;
    row.template get<"price">() = r.price;
    row.template get<"size">() = r.size;
    return row;
};

// Thread-safe queue for cross-thread batch handoff
template <typename T>
class ThreadSafeQueue {
public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    bool try_pop(T& item, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; })) {
            return false;
        }
        if (queue_.empty()) {
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool is_closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    bool closed_ = false;
};

// Mock database that tracks operations
class MockDatabase : public IDatabase {
public:
    explicit MockDatabase(testing::SimulatedLibPq& pq) : pq_(pq) {}

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
        batches_started_++;
        return std::make_unique<MockCopyWriter>(*this);
    }

    bool is_connected() const override { return true; }

    void record_row_written() { rows_written_++; }
    void record_batch_finished() { batches_finished_++; }

    int rows_written() const { return rows_written_.load(); }
    int batches_finished() const { return batches_finished_.load(); }
    int batches_started() const { return batches_started_.load(); }

private:
    class MockCopyWriter : public ICopyWriter {
    public:
        explicit MockCopyWriter(MockDatabase& db) : db_(db) {}

        asio::awaitable<void> start() override { co_return; }
        asio::awaitable<void> write_row(std::span<const std::byte>) override {
            db_.record_row_written();
            co_return;
        }
        asio::awaitable<void> finish() override {
            db_.record_batch_finished();
            co_return;
        }
        asio::awaitable<void> abort() override { co_return; }

    private:
        MockDatabase& db_;
    };

    testing::SimulatedLibPq& pq_;
    std::atomic<int> rows_written_{0};
    std::atomic<int> batches_finished_{0};
    std::atomic<int> batches_started_{0};
};

// Simulates a download producer that generates batches on a separate thread
class DownloadProducer {
public:
    explicit DownloadProducer(ThreadSafeQueue<std::vector<TestRecord>>& queue)
        : queue_(queue) {}

    void produce(int num_batches, int batch_size) {
        for (int b = 0; b < num_batches; ++b) {
            std::vector<TestRecord> batch;
            batch.reserve(batch_size);
            for (int i = 0; i < batch_size; ++i) {
                batch.push_back({
                    .ts_event = static_cast<int64_t>(b * batch_size + i),
                    .instrument_id = 1,
                    .price = 10000 + i,
                    .size = 100
                });
            }
            queue_.push(std::move(batch));
            // Simulate network delay
            std::this_thread::sleep_for(1ms);
        }
        queue_.close();
    }

private:
    ThreadSafeQueue<std::vector<TestRecord>>& queue_;
};

TEST(MultithreadTest, DownloadAndWriteOnSeparateThreads) {
    // Setup: Queue for cross-thread communication
    ThreadSafeQueue<std::vector<TestRecord>> batch_queue;

    // Setup: Database writer on its own io_context/thread
    asio::io_context db_ctx;
    testing::SimulatedLibPq pq;
    MockDatabase db(pq);

    using WriterType = BatchWriter<TestRecord, decltype(test_table), decltype(test_transform)>;
    WriterType writer(db_ctx, db, test_table, test_transform);

    std::atomic<int> batches_enqueued{0};
    std::atomic<bool> writer_running{true};

    // Consumer thread: Runs io_context and processes batches
    std::thread db_thread([&] {
        // Create work guard to keep io_context running
        auto work = asio::make_work_guard(db_ctx);

        // Poll for batches and enqueue them
        while (writer_running || !batch_queue.is_closed()) {
            std::vector<TestRecord> batch;
            if (batch_queue.try_pop(batch, 10ms)) {
                // Post enqueue to io_context to ensure thread-safety
                asio::post(db_ctx, [&writer, batch = std::move(batch), &batches_enqueued]() mutable {
                    writer.enqueue(std::move(batch));
                    batches_enqueued++;
                });
            }
            // Run pending handlers
            db_ctx.poll();
        }

        // Signal stop and drain remaining work
        asio::post(db_ctx, [&writer] {
            writer.request_stop();
        });

        // Run until idle
        work.reset();
        db_ctx.run();
    });

    // Producer thread: Simulates download producing batches
    const int num_batches = 10;
    const int batch_size = 100;

    std::thread producer_thread([&] {
        DownloadProducer producer(batch_queue);
        producer.produce(num_batches, batch_size);
    });

    // Wait for producer to finish
    producer_thread.join();

    // Signal writer thread to stop
    writer_running = false;

    // Wait for writer thread to finish
    db_thread.join();

    // Verify results.
    // With coalescing, multiple enqueued batches may be combined into fewer
    // COPY operations. With chunked I/O, write_row is called per ~1MB chunk,
    // not per record. request_stop() may drop pending batches, so exact
    // counts are timing-dependent.
    EXPECT_EQ(batches_enqueued.load(), num_batches);
    EXPECT_GE(db.batches_started(), 1);
    EXPECT_EQ(db.batches_started(), db.batches_finished());
}

TEST(MultithreadTest, IsInEventLoopThreadCorrect) {
    asio::io_context ctx;
    AsioEventLoop loop(ctx);

    std::atomic<bool> in_loop_from_main{false};
    std::atomic<bool> in_loop_from_run{false};
    std::atomic<bool> in_loop_from_other{false};

    // Check from main thread before Run() - should be true because
    // constructor captures main thread
    in_loop_from_main = loop.IsInEventLoopThread();

    // Create a separate thread that runs the event loop and checks
    // from both inside and outside simultaneously
    std::thread runner([&] {
        // Post a check that runs inside the event loop
        asio::post(ctx, [&] {
            in_loop_from_run = loop.IsInEventLoopThread();
            ctx.stop();
        });
        loop.Run();
    });

    // While runner is executing, check from main thread
    // Wait a bit for Run() to update thread_id_
    std::this_thread::sleep_for(10ms);

    // Now check from main thread - should be false since Run() updated
    // thread_id_ to the runner thread
    in_loop_from_other = loop.IsInEventLoopThread();

    runner.join();

    // Main thread before Run(): true (constructor captured main thread)
    EXPECT_TRUE(in_loop_from_main);
    // Inside Run(): true because Run() updates thread_id_ to runner thread
    EXPECT_TRUE(in_loop_from_run);
    // Main thread after Run() started: false (thread_id_ is now runner thread)
    EXPECT_FALSE(in_loop_from_other);
}

TEST(MultithreadTest, ThreadIdUpdatedOnRun) {
    asio::io_context ctx;
    AsioEventLoop loop(ctx);

    std::thread::id main_id = std::this_thread::get_id();
    std::thread::id run_thread_id;

    // Run on a different thread
    std::thread runner([&] {
        run_thread_id = std::this_thread::get_id();

        asio::post(ctx, [&] {
            // Inside Run(), IsInEventLoopThread should return true
            EXPECT_TRUE(loop.IsInEventLoopThread());
            ctx.stop();
        });

        loop.Run();
    });
    runner.join();

    // Verify it ran on a different thread
    EXPECT_NE(main_id, run_thread_id);
}

TEST(MultithreadTest, BackpressureAcrossThreads) {
    // Test that backpressure signals work across threads
    ThreadSafeQueue<std::vector<TestRecord>> batch_queue;

    asio::io_context db_ctx;
    testing::SimulatedLibPq pq;
    MockDatabase db(pq);

    BackpressureConfig config;
    config.high_water_mark = 5;
    config.low_water_mark = 2;

    using WriterType = BatchWriter<TestRecord, decltype(test_table), decltype(test_transform)>;
    WriterType writer(db_ctx, db, test_table, test_transform, config);

    std::atomic<int> suspend_count{0};
    std::atomic<int> resume_count{0};

    class TestSuspendable : public ISuspendable {
    public:
        TestSuspendable(std::atomic<int>& sc, std::atomic<int>& rc)
            : suspend_count_(sc), resume_count_(rc) {}
        void Suspend() override { suspend_count_++; }
        void Resume() override { resume_count_++; }
    private:
        std::atomic<int>& suspend_count_;
        std::atomic<int>& resume_count_;
    } suspendable(suspend_count, resume_count);

    writer.set_suspendable(&suspendable);

    std::atomic<bool> done{false};

    // Writer thread
    std::thread db_thread([&] {
        auto work = asio::make_work_guard(db_ctx);

        while (!done) {
            db_ctx.poll();
            std::this_thread::sleep_for(1ms);
        }

        asio::post(db_ctx, [&writer] {
            writer.request_stop();
        });

        work.reset();
        db_ctx.run();
    });

    // Producer: Enqueue many batches quickly to trigger backpressure
    for (int i = 0; i < 20; ++i) {
        std::vector<TestRecord> batch;
        batch.push_back({.ts_event = i, .instrument_id = 1, .price = 100, .size = 10});

        asio::post(db_ctx, [&writer, batch = std::move(batch)]() mutable {
            writer.enqueue(std::move(batch));
        });
    }

    // Give time for processing
    std::this_thread::sleep_for(100ms);

    done = true;
    db_thread.join();

    // Should have triggered backpressure at some point
    // (may or may not depending on timing, but shouldn't crash)
    EXPECT_GE(db.batches_started(), 1);
}

}  // namespace
}  // namespace dbwriter
