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
        // Best effort abort
        pq_.putCopyEnd(conn_, "aborted");
        pq_.getResult(conn_);
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
    if (pq_.resultStatus(res) != PGRES_COPY_IN) {
        std::string err = pq_.resultErrorMessage(res);
        pq_.clear(res);
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

    // End COPY
    if (pq_.putCopyEnd(conn_, nullptr) != 1) {
        throw std::runtime_error(pq_.errorMessage(conn_));
    }

    in_copy_ = false;

    // Get result
    PGresult* res = pq_.getResult(conn_);
    if (pq_.resultStatus(res) != PGRES_COMMAND_OK) {
        std::string err = pq_.resultErrorMessage(res);
        pq_.clear(res);
        throw std::runtime_error("COPY finish failed: " + err);
    }
    pq_.clear(res);

    co_return;
}

asio::awaitable<void> PostgresCopyWriter::abort() {
    if (in_copy_) {
        pq_.putCopyEnd(conn_, "aborted by client");
        pq_.getResult(conn_);
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
    int result = pq_.putCopyData(conn_,
        reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()));

    if (result == -1) {
        throw std::runtime_error(pq_.errorMessage(conn_));
    }

    if (result == 0) {
        // Would block, wait for writable
        co_await wait_writable();
        // Retry
        result = pq_.putCopyData(conn_,
            reinterpret_cast<const char*>(data.data()),
            static_cast<int>(data.size()));
        if (result != 1) {
            throw std::runtime_error(pq_.errorMessage(conn_));
        }
    }

    co_return;
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
    pq_.setnonblocking(conn_, 1);

    co_return;
}

asio::awaitable<QueryResult> PostgresDatabase::query(std::string_view sql) {
    if (!pq_.sendQuery(conn_, std::string(sql).c_str())) {
        throw std::runtime_error(pq_.errorMessage(conn_));
    }

    // Wait for result (simplified - real impl would use async)
    PGresult* res = pq_.getResult(conn_);
    if (pq_.resultStatus(res) != PGRES_TUPLES_OK) {
        std::string err = pq_.resultErrorMessage(res);
        pq_.clear(res);
        throw std::runtime_error("Query failed: " + err);
    }

    // Convert to QueryResult (simplified)
    pq_.clear(res);
    co_return QueryResult{};
}

asio::awaitable<void> PostgresDatabase::execute(std::string_view sql) {
    if (!pq_.sendQuery(conn_, std::string(sql).c_str())) {
        throw std::runtime_error(pq_.errorMessage(conn_));
    }

    // Wait for result: consume → check busy → wait if needed
    asio::posix::stream_descriptor socket(ctx_, pq_.socket(conn_));
    do {
        if (!pq_.consumeInput(conn_)) {
            socket.release();
            throw std::runtime_error(pq_.errorMessage(conn_));
        }
        if (!pq_.isBusy(conn_)) break;
        co_await socket.async_wait(
            asio::posix::stream_descriptor::wait_read,
            asio::use_awaitable);
    } while (pq_.isBusy(conn_));

    PGresult* res = pq_.getResult(conn_);
    if (pq_.resultStatus(res) != PGRES_COMMAND_OK) {
        std::string err = pq_.resultErrorMessage(res);
        pq_.clear(res);
        socket.release();
        throw std::runtime_error("Execute failed: " + err);
    }
    pq_.clear(res);

    // Consume any remaining results (required by libpq)
    while ((res = pq_.getResult(conn_)) != nullptr) {
        pq_.clear(res);
    }

    socket.release();  // Don't close libpq's socket
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
