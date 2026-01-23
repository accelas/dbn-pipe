// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#include "dbwriter/postgres.hpp"
#include <sstream>
#include <stdexcept>

namespace dbwriter {

std::string PostgresConfig::connection_string() const {
    std::ostringstream ss;
    ss << "host=" << host
       << " port=" << port
       << " dbname=" << database
       << " user=" << user
       << " password=" << password;
    return ss.str();
}

// PostgresCopyWriter implementation
//
// IMPORTANT: Coroutine lifetime requirement
// The caller must ensure all coroutine methods (start, write_row, finish)
// are fully awaited before destroying this object. Destroying while a
// coroutine is suspended will result in undefined behavior.
//
// IMPORTANT: COPY cleanup requirement
// The caller should call co_await finish() on success or co_await abort()
// on error BEFORE destruction. The destructor performs best-effort cleanup
// but cannot properly handle nonblocking I/O (no coroutine context).
// Destroying with in_copy_==true may leave the connection in an unusable state.

PostgresCopyWriter::PostgresCopyWriter(
    PGconn* conn, asio::io_context& ctx,
    std::string_view table,
    std::span<const std::string_view> columns,
    ILibPq& pq)
    : conn_(conn)
    , socket_(ctx, pq.socket(conn))
    , table_(table)
    , pq_(pq) {
    for (const auto& col : columns) {
        columns_.emplace_back(col);
    }
}

PostgresCopyWriter::~PostgresCopyWriter() {
    // Release the fd so ASIO doesn't close it - libpq owns the socket
    socket_.release();

    if (in_copy_) {
        // Best effort abort - must clear results to avoid leaks
        pq_.putCopyEnd(conn_, "aborted");
        PGresult* res = pq_.getResult(conn_);
        if (res) pq_.clear(res);
        // Drain any remaining results
        while ((res = pq_.getResult(conn_)) != nullptr) {
            pq_.clear(res);
        }
    }
}

asio::awaitable<void> PostgresCopyWriter::start() {
    // Build COPY command
    std::ostringstream ss;
    ss << "COPY " << table_ << " (";
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << columns_[i];
    }
    ss << ") FROM STDIN WITH (FORMAT binary)";

    // Send command
    if (!pq_.sendQuery(conn_, ss.str().c_str())) {
        throw std::runtime_error(pq_.errorMessage(conn_));
    }

    // Wait for result: consume → check busy → wait if needed
    do {
        if (!pq_.consumeInput(conn_)) {
            throw std::runtime_error(pq_.errorMessage(conn_));
        }
        if (!pq_.isBusy(conn_)) break;
        co_await socket_.async_wait(
            asio::posix::stream_descriptor::wait_read,
            asio::use_awaitable);
    } while (pq_.isBusy(conn_));

    PGresult* res = pq_.getResult(conn_);
    if (!res) {
        // Drain any remaining results before throwing
        while ((res = pq_.getResult(conn_)) != nullptr) {
            pq_.clear(res);
        }
        throw std::runtime_error("COPY: no result received");
    }
    if (pq_.resultStatus(res) != PGRES_COPY_IN) {
        std::string err = pq_.resultErrorMessage(res);
        pq_.clear(res);
        // Drain any remaining results before throwing
        while ((res = pq_.getResult(conn_)) != nullptr) {
            pq_.clear(res);
        }
        throw std::runtime_error("COPY failed: " + err);
    }
    pq_.clear(res);

    in_copy_ = true;

    // Write binary COPY header
    static constexpr char header[] = "PGCOPY\n\377\r\n\0\0\0\0\0\0\0\0\0";
    co_await send_data({reinterpret_cast<const std::byte*>(header), 19});
}

asio::awaitable<void> PostgresCopyWriter::write_row(std::span<const std::byte> data) {
    co_await send_data(data);
}

asio::awaitable<void> PostgresCopyWriter::finish() {
    // Write trailer (-1 as int16)
    static constexpr char trailer[] = "\xFF\xFF";
    co_await send_data({reinterpret_cast<const std::byte*>(trailer), 2});

    // End COPY - may need to wait for writable in nonblocking mode
    while (true) {
        int result = pq_.putCopyEnd(conn_, nullptr);
        if (result == 1) break;
        if (result == -1) {
            throw std::runtime_error(pq_.errorMessage(conn_));
        }
        // result == 0: Would block
        co_await wait_writable();
    }

    in_copy_ = false;

    // Wait for result: consume → check busy → wait if needed
    do {
        if (!pq_.consumeInput(conn_)) {
            throw std::runtime_error(pq_.errorMessage(conn_));
        }
        if (!pq_.isBusy(conn_)) break;
        co_await socket_.async_wait(
            asio::posix::stream_descriptor::wait_read,
            asio::use_awaitable);
    } while (pq_.isBusy(conn_));

    PGresult* res = pq_.getResult(conn_);
    if (!res) {
        throw std::runtime_error("COPY finish: no result received");
    }
    if (pq_.resultStatus(res) != PGRES_COMMAND_OK) {
        std::string err = pq_.resultErrorMessage(res);
        pq_.clear(res);
        throw std::runtime_error("COPY finish failed: " + err);
    }
    pq_.clear(res);

    // Drain any remaining results
    while ((res = pq_.getResult(conn_)) != nullptr) {
        pq_.clear(res);
    }

    co_return;
}

asio::awaitable<void> PostgresCopyWriter::abort() {
    if (in_copy_) {
        // End COPY with error message - may need to wait for writable
        while (true) {
            int result = pq_.putCopyEnd(conn_, "aborted by client");
            if (result == 1) break;
            if (result == -1) {
                // Error - connection may be broken, drain what we can
                break;
            }
            // result == 0: Would block
            co_await wait_writable();
        }

        // Wait for result: consume → check busy → wait if needed
        do {
            if (!pq_.consumeInput(conn_)) {
                break;  // Connection error, can't do more
            }
            if (!pq_.isBusy(conn_)) break;
            co_await socket_.async_wait(
                asio::posix::stream_descriptor::wait_read,
                asio::use_awaitable);
        } while (pq_.isBusy(conn_));

        // Drain all results
        PGresult* res;
        while ((res = pq_.getResult(conn_)) != nullptr) {
            pq_.clear(res);
        }
        in_copy_ = false;
    }
    co_return;
}

asio::awaitable<void> PostgresCopyWriter::wait_writable() {
    co_await socket_.async_wait(
        asio::posix::stream_descriptor::wait_write,
        asio::use_awaitable);
}

asio::awaitable<void> PostgresCopyWriter::send_data(std::span<const std::byte> data) {
    while (true) {
        int result = pq_.putCopyData(conn_,
            reinterpret_cast<const char*>(data.data()),
            static_cast<int>(data.size()));

        if (result == 1) {
            // Success
            co_return;
        }

        if (result == -1) {
            throw std::runtime_error(pq_.errorMessage(conn_));
        }

        // result == 0: Would block, wait for writable and retry
        co_await wait_writable();
    }
}

// PostgresDatabase implementation

PostgresDatabase::PostgresDatabase(asio::io_context& ctx, const PostgresConfig& config,
                                   ILibPq& pq)
    : ctx_(ctx)
    , config_(config)
    , pq_(pq) {}

PostgresDatabase::~PostgresDatabase() {
    if (conn_) {
        pq_.finish(conn_);
    }
}

asio::awaitable<void> PostgresDatabase::connect() {
    conn_ = pq_.connectdb(config_.connection_string().c_str());

    if (pq_.status(conn_) != CONNECTION_OK) {
        std::string err = pq_.errorMessage(conn_);
        pq_.finish(conn_);
        conn_ = nullptr;
        throw std::runtime_error("Connection failed: " + err);
    }

    // Set non-blocking mode
    if (pq_.setnonblocking(conn_, 1) != 0) {
        std::string err = pq_.errorMessage(conn_);
        pq_.finish(conn_);
        conn_ = nullptr;
        throw std::runtime_error("Failed to set non-blocking mode: " + err);
    }

    co_return;
}

asio::awaitable<QueryResult> PostgresDatabase::query(std::string_view sql) {
    if (!pq_.sendQuery(conn_, std::string(sql).c_str())) {
        throw std::runtime_error(pq_.errorMessage(conn_));
    }

    // Wait for result: consume → check busy → wait if needed
    asio::posix::stream_descriptor socket(ctx_, pq_.socket(conn_));

    // Scope guard ensures socket.release() and result draining on all exit paths
    ILibPq* pq = &pq_;
    PGconn* conn = conn_;
    auto cleanup = [&socket, pq, conn]() {
        socket.release();
        // Drain any remaining results to keep connection usable
        PGresult* r;
        while ((r = pq->getResult(conn)) != nullptr) {
            pq->clear(r);
        }
    };
    struct ScopeGuard {
        std::function<void()> fn;
        ~ScopeGuard() { fn(); }
    } scope_guard{cleanup};

    do {
        if (!pq_.consumeInput(conn_)) {
            throw std::runtime_error(pq_.errorMessage(conn_));
        }
        if (!pq_.isBusy(conn_)) break;
        co_await socket.async_wait(
            asio::posix::stream_descriptor::wait_read,
            asio::use_awaitable);
    } while (pq_.isBusy(conn_));

    PGresult* res = pq_.getResult(conn_);
    if (!res) {
        throw std::runtime_error("Query: no result received");
    }
    if (pq_.resultStatus(res) != PGRES_TUPLES_OK) {
        std::string err = pq_.resultErrorMessage(res);
        pq_.clear(res);
        throw std::runtime_error("Query failed: " + err);
    }

    // Convert to QueryResult (simplified)
    pq_.clear(res);

    // Remaining results drained by scope_guard
    co_return QueryResult{};
}

asio::awaitable<void> PostgresDatabase::execute(std::string_view sql) {
    if (!pq_.sendQuery(conn_, std::string(sql).c_str())) {
        throw std::runtime_error(pq_.errorMessage(conn_));
    }

    // Wait for result: consume → check busy → wait if needed
    asio::posix::stream_descriptor socket(ctx_, pq_.socket(conn_));

    // Scope guard ensures socket.release() and result draining on all exit paths
    ILibPq* pq = &pq_;
    PGconn* conn = conn_;
    auto cleanup = [&socket, pq, conn]() {
        socket.release();
        // Drain any remaining results to keep connection usable
        PGresult* r;
        while ((r = pq->getResult(conn)) != nullptr) {
            pq->clear(r);
        }
    };
    struct ScopeGuard {
        std::function<void()> fn;
        ~ScopeGuard() { fn(); }
    } scope_guard{cleanup};

    do {
        if (!pq_.consumeInput(conn_)) {
            throw std::runtime_error(pq_.errorMessage(conn_));
        }
        if (!pq_.isBusy(conn_)) break;
        co_await socket.async_wait(
            asio::posix::stream_descriptor::wait_read,
            asio::use_awaitable);
    } while (pq_.isBusy(conn_));

    PGresult* res = pq_.getResult(conn_);
    if (!res) {
        throw std::runtime_error("Execute: no result received");
    }
    if (pq_.resultStatus(res) != PGRES_COMMAND_OK) {
        std::string err = pq_.resultErrorMessage(res);
        pq_.clear(res);
        throw std::runtime_error("Execute failed: " + err);
    }
    pq_.clear(res);

    // Remaining results drained by scope_guard
    co_return;
}

std::unique_ptr<ICopyWriter> PostgresDatabase::begin_copy(
    std::string_view table,
    std::span<const std::string_view> columns) {
    return std::make_unique<PostgresCopyWriter>(conn_, ctx_, table, columns, pq_);
}

bool PostgresDatabase::is_connected() const {
    return conn_ && pq_.status(conn_) == CONNECTION_OK;
}

}  // namespace dbwriter
