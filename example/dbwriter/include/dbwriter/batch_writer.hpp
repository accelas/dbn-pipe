// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "dbwriter/database.hpp"
#include "dbwriter/mapper.hpp"
#include "dbwriter/transform.hpp"
#include "dbwriter/types.hpp"
#include <asio.hpp>
#include <atomic>
#include <deque>
#include <functional>
#include <span>
#include <vector>

namespace dbwriter {

struct BackpressureConfig {
    size_t max_pending_bytes = 1ULL * 1024 * 1024 * 1024;  // 1GB
    size_t high_water_mark = 256;
    size_t low_water_mark = 64;
};

enum class WriteError {
    ConnectionLost,
    CopyFailed,
    SerializationError,
    Timeout,
};

// Suspendable interface for backpressure
class ISuspendable {
public:
    virtual ~ISuspendable() = default;
    virtual void Suspend() = 0;
    virtual void Resume() = 0;
};

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
        // Signal stop and wait for coroutine to complete
        stop();
    }

    // Non-copyable, non-movable (coroutine captures this)
    BatchWriter(const BatchWriter&) = delete;
    BatchWriter& operator=(const BatchWriter&) = delete;
    BatchWriter(BatchWriter&&) = delete;
    BatchWriter& operator=(BatchWriter&&) = delete;

    void set_suspendable(ISuspendable* s) { suspendable_ = s; }
    void on_error(ErrorHandler handler) { error_handler_ = std::move(handler); }

    void enqueue(std::vector<Record> batch) {
        if (stopping_) return;  // Don't accept new work after stop
        pending_batches_.push_back(std::move(batch));
        check_backpressure();

        if (!writing_) {
            asio::co_spawn(ctx_, process_queue(), asio::detached);
        }
    }

    // Signal stop and block until coroutine completes
    void stop() {
        stopping_ = true;
        // Spin until coroutine completes (writing_ goes false)
        // This is safe because we're on the same thread as io_context
        while (writing_) {
            ctx_.poll_one();
        }
    }

    bool is_stopped() const { return stopping_ && !writing_; }

    size_t pending_count() const { return pending_batches_.size(); }

private:
    asio::awaitable<void> process_queue() {
        writing_ = true;

        while (!pending_batches_.empty() && !stopping_) {
            auto batch = std::move(pending_batches_.front());
            pending_batches_.pop_front();

            try {
                co_await write_batch(batch);
            } catch (const std::exception& e) {
                if (error_handler_) {
                    error_handler_(WriteError::CopyFailed, e.what());
                }
            }

            check_resume();
        }

        writing_ = false;
    }

    asio::awaitable<void> write_batch(const std::vector<Record>& batch) {
        auto columns = table_.column_names();
        std::vector<std::string_view> col_views(columns.begin(), columns.end());

        auto writer = db_.begin_copy(table_.name(), col_views);

        co_await writer->start();

        ByteBuffer buf;
        for (const auto& record : batch) {
            auto row = transform_(record);
            mapper_.encode_row(row, buf);
            co_await writer->write_row(buf.view());
            buf.clear();
        }

        co_await writer->finish();
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
    Mapper<Table> mapper_;
    BackpressureConfig config_;

    std::deque<std::vector<Record>> pending_batches_;
    bool writing_ = false;
    bool suspended_ = false;
    std::atomic<bool> stopping_{false};
    ISuspendable* suspendable_ = nullptr;
    ErrorHandler error_handler_;
};

}  // namespace dbwriter
