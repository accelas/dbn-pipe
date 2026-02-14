// SPDX-License-Identifier: MIT

#pragma once

#include "dbwriter/database.hpp"
#include "dbwriter/transform.hpp"
#include "dbn_pipe/pg/byte_buffer.hpp"
#include "dbn_pipe/pg/mapper.hpp"
#include <asio.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace dbwriter {

struct BackpressureConfig {
    // Number of pending batches that triggers backpressure (suspends upstream)
    size_t high_water_mark = 256;
    // Number of pending batches that releases backpressure (resumes upstream)
    size_t low_water_mark = 64;
};

enum class WriteError {
    ConnectionLost,
    CopyFailed,
    SerializationError,
    Timeout,
};

// Accumulated write statistics for monitoring
struct WriteStats {
    uint64_t copies_completed = 0;    // Number of COPY operations executed
    uint64_t batches_coalesced = 0;   // Number of enqueued batches consumed
    uint64_t rows_copied = 0;         // Total rows sent via COPY to staging
    uint64_t rows_inserted = 0;       // Rows actually inserted (after dedup)
    size_t batches_pending = 0;       // Batches waiting in queue
};

// Suspendable interface for backpressure
class ISuspendable {
public:
    virtual ~ISuspendable() = default;
    virtual void Suspend() = 0;
    virtual void Resume() = 0;
};

// IMPORTANT: BatchWriter lifecycle requirement
// The caller must ensure the coroutine completes before destroying BatchWriter.
// Recommended approach: co_await drain() before destruction.
// Alternative: Call request_stop() and run io_context until is_idle() returns true.
//
// Destroying while a coroutine is in flight results in undefined behavior.
//
// IMPORTANT: Thread safety
// BatchWriter is NOT thread-safe. All method calls (enqueue, request_stop, etc.)
// must be made from the io_context thread or externally synchronized.
// Calling from multiple threads without synchronization results in data races.
template <typename Record, typename Table, typename TransformT>
class BatchWriter {
public:
    using RowType = typename Table::RowType;
    using ErrorHandler = std::function<void(WriteError, std::string_view)>;

    BatchWriter(asio::io_context& ctx,
                IDatabase& db,
                const Table& table,
                TransformT transform,
                BackpressureConfig config = {})
        : ctx_(ctx)
        , db_(db)
        , table_(table)
        , transform_(std::move(transform))
        , mapper_(table)
        , config_(config) {}

    ~BatchWriter() {
        // Destructor is non-blocking. Caller must ensure coroutine completed.
        // Assert in debug builds to catch misuse.
        assert(!writing_ && "BatchWriter destroyed while coroutine in flight");
    }

    // Non-copyable, non-movable (coroutine captures this)
    BatchWriter(const BatchWriter&) = delete;
    BatchWriter& operator=(const BatchWriter&) = delete;
    BatchWriter(BatchWriter&&) = delete;
    BatchWriter& operator=(BatchWriter&&) = delete;

    void set_suspendable(ISuspendable* s) { suspendable_ = s; }
    void on_error(ErrorHandler handler) { error_handler_ = std::move(handler); }

    void enqueue(std::vector<Record> batch) {
        if (stopping_ || draining_) return;  // Don't accept new work after stop/drain
        pending_batches_.push_back(std::move(batch));
        check_backpressure();

        if (!writing_) {
            // Set writing_ BEFORE co_spawn to prevent race:
            // If request_stop() + destruction happens before coroutine starts,
            // is_idle() must return false to prevent use-after-free.
            writing_ = true;
            asio::co_spawn(ctx_, process_queue(), asio::detached);
        }
    }

    // Signal immediate stop - discards pending work, completes after current batch.
    // Non-blocking. Caller must run io_context until is_idle() returns true.
    // Use drain() instead if you need to process all pending batches.
    void request_stop() {
        stopping_ = true;
        pending_batches_.clear();  // Discard pending work
        // Resume suspended upstream so it's not stuck forever
        if (suspendable_ && suspended_) {
            suspendable_->Resume();
            suspended_ = false;
        }
    }

    // Returns true when stopped and no coroutine in flight
    bool is_idle() const { return !writing_; }
    bool is_writing() const { return writing_; }
    bool stop_requested() const { return stopping_; }
    bool is_draining() const { return draining_; }

    size_t pending_count() const { return pending_batches_.size(); }

    WriteStats stats() const {
        return {copies_completed_, batches_coalesced_, rows_copied_,
                rows_inserted_, pending_batches_.size()};
    }

    // Awaitable that completes when all pending work is processed.
    // MUST be awaited before destroying BatchWriter to prevent use-after-free.
    //
    // Unlike request_stop(), drain() does NOT discard pending batches.
    // All enqueued batches will be written before drain() completes.
    //
    // IMPORTANT: drain() is TERMINAL - once called, the writer permanently
    // stops accepting new work. Any enqueue() calls after drain() starts
    // will be silently ignored. If you need to flush without stopping,
    // wait for pending_count() == 0 and is_idle() instead.
    asio::awaitable<void> drain(std::chrono::seconds timeout = std::chrono::seconds{0}) {
        draining_ = true;  // Stop accepting new work, but keep processing
        // Resume suspended upstream so it's not stuck forever
        if (suspendable_ && suspended_) {
            suspendable_->Resume();
            suspended_ = false;
        }
        // Wait for process_queue to complete using timer cancellation.
        // process_queue cancels the timer when it finishes, waking us immediately.
        // The 1-second timeout is a fallback; normal wakeup is via cancel().
        auto start = std::chrono::steady_clock::now();
        while (writing_) {
            asio::steady_timer timer(ctx_, std::chrono::seconds(1));
            drain_timer_ = &timer;
            asio::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            drain_timer_ = nullptr;
            // ec == operation_aborted means cancelled by process_queue - loop will check writing_
            if (timeout.count() > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start);
                if (elapsed >= timeout) {
                    stopping_ = true;  // Signal process_queue to exit after current batch
                    break;
                }
            }
        }
    }

private:
    void safe_error(WriteError code, std::string_view msg) {
        if (!error_handler_) return;
        try { error_handler_(code, msg); } catch (...) {}
    }

    asio::awaitable<void> process_queue() {
        // writing_ is already set by enqueue() before co_spawn()
        // Use scope guard to ensure writing_ = false on all exit paths
        // Also cancels drain timer to wake up drain() immediately
        struct WritingGuard {
            bool& flag;
            asio::steady_timer*& timer;
            ~WritingGuard() {
                flag = false;
                if (timer) {
                    try { timer->cancel(); } catch (...) {}
                }
            }
        } guard{writing_, drain_timer_};

        while (!pending_batches_.empty() && !stopping_) {
            // Coalesce: drain all available batches into one COPY operation.
            // While the previous COPY ran (~50ms), new batches pile up naturally.
            std::vector<std::vector<Record>> batches;
            while (!pending_batches_.empty()) {
                batches.push_back(std::move(pending_batches_.front()));
                pending_batches_.pop_front();
            }

            try {
                co_await write_batch(batches);
            } catch (const std::exception& e) {
                safe_error(WriteError::CopyFailed, e.what());
            } catch (...) {
                safe_error(WriteError::CopyFailed, "unknown error");
            }

            check_resume();
        }
        // writing_ = false handled by guard destructor
    }

    asio::awaitable<void> write_batch(std::vector<std::vector<Record>>& batches) {
        auto columns = table_.column_names();
        std::vector<std::string_view> col_views(columns.begin(), columns.end());

        // Stage via temp table for deduplication:
        // COPY → temp table (no constraints, never fails on duplicates)
        // INSERT INTO target SELECT ... ON CONFLICT DO NOTHING
        std::string target{table_.name()};
        std::string staging = "_staging_" + target;
        co_await db_.execute("DROP TABLE IF EXISTS " + staging);
        co_await db_.execute(
            "CREATE TEMP TABLE " + staging +
            " (LIKE " + target + ")");

        auto writer = db_.begin_copy(staging, col_views);

        std::exception_ptr ex;
        uint64_t batch_rows = 0;

        try {
            co_await writer->start();

            // Encode rows into buffer, flushing in chunks to cap memory.
            // Each flush is one PQputCopyData call instead of one per row,
            // eliminating ~2N coroutine frames per N rows.
            dbn_pipe::pg::ByteBuffer buf;
            constexpr size_t kFlushThreshold = 1024 * 1024;  // 1 MB

            for (auto& batch : batches) {
                for (const auto& record : batch) {
                    auto row = transform_(record);
                    mapper_.encode_row(row, buf);
                    ++batch_rows;

                    if (buf.size() >= kFlushThreshold) {
                        co_await writer->write_row(buf.view());
                        buf.clear();
                    }
                }
            }

            // Flush remaining data
            if (buf.size() > 0) {
                co_await writer->write_row(buf.view());
            }

            co_await writer->finish();
        } catch (...) {
            // Cannot co_await in catch block, so save exception and abort outside
            ex = std::current_exception();
        }

        if (ex) {
            // Abort COPY session to leave connection in usable state
            try { co_await writer->abort(); } catch (...) {}
            // Clean up staging table before rethrowing
            try { co_await db_.execute("DROP TABLE IF EXISTS " + staging); } catch (...) {}
            std::rethrow_exception(ex);
        }

        // Move from staging to target, skip duplicates
        uint64_t inserted = co_await db_.execute_count(
            "INSERT INTO " + target +
            " SELECT * FROM " + staging +
            " ON CONFLICT DO NOTHING");

        co_await db_.execute("DROP TABLE " + staging);

        // Update stats
        copies_completed_++;
        batches_coalesced_ += batches.size();
        rows_copied_ += batch_rows;
        rows_inserted_ += inserted;
    }

    void check_backpressure() {
        if (suspendable_ && pending_batches_.size() > config_.high_water_mark) {
            suspendable_->Suspend();
            suspended_ = true;
        }
    }

    void check_resume() {
        if (suspendable_ && suspended_ &&
            pending_batches_.size() < config_.low_water_mark) {
            suspendable_->Resume();
            suspended_ = false;
        }
    }

    asio::io_context& ctx_;
    IDatabase& db_;
    const Table& table_;
    TransformT transform_;
    dbn_pipe::pg::Mapper<Table> mapper_;
    BackpressureConfig config_;

    std::deque<std::vector<Record>> pending_batches_;
    bool writing_ = false;
    bool suspended_ = false;
    bool draining_ = false;
    std::atomic<bool> stopping_{false};
    ISuspendable* suspendable_ = nullptr;
    ErrorHandler error_handler_;
    asio::steady_timer* drain_timer_ = nullptr;  // For signaling drain() completion

    // Stats
    uint64_t copies_completed_ = 0;
    uint64_t batches_coalesced_ = 0;
    uint64_t rows_copied_ = 0;
    uint64_t rows_inserted_ = 0;
};

}  // namespace dbwriter
