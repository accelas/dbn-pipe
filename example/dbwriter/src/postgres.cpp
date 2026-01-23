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
    std::span<const std::string_view> columns)
    : conn_(conn)
    , socket_(ctx, PQsocket(conn))
    , table_(table) {
    for (const auto& col : columns) {
        columns_.emplace_back(col);
    }
}

PostgresCopyWriter::~PostgresCopyWriter() {
    if (in_copy_) {
        // Best effort abort
        PQputCopyEnd(conn_, "aborted");
        PQgetResult(conn_);
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
    if (!PQsendQuery(conn_, ss.str().c_str())) {
        throw std::runtime_error(PQerrorMessage(conn_));
    }

    // Wait for result
    co_await wait_writable();

    PGresult* res = PQgetResult(conn_);
    if (PQresultStatus(res) != PGRES_COPY_IN) {
        std::string err = PQresultErrorMessage(res);
        PQclear(res);
        throw std::runtime_error("COPY failed: " + err);
    }
    PQclear(res);

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
    if (PQputCopyEnd(conn_, nullptr) != 1) {
        throw std::runtime_error(PQerrorMessage(conn_));
    }

    in_copy_ = false;

    // Get result
    PGresult* res = PQgetResult(conn_);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string err = PQresultErrorMessage(res);
        PQclear(res);
        throw std::runtime_error("COPY finish failed: " + err);
    }
    PQclear(res);

    co_return;
}

asio::awaitable<void> PostgresCopyWriter::abort() {
    if (in_copy_) {
        PQputCopyEnd(conn_, "aborted by client");
        PQgetResult(conn_);
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
    int result = PQputCopyData(conn_,
        reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()));

    if (result == -1) {
        throw std::runtime_error(PQerrorMessage(conn_));
    }

    if (result == 0) {
        // Would block, wait for writable
        co_await wait_writable();
        // Retry
        result = PQputCopyData(conn_,
            reinterpret_cast<const char*>(data.data()),
            static_cast<int>(data.size()));
        if (result != 1) {
            throw std::runtime_error(PQerrorMessage(conn_));
        }
    }

    co_return;
}

// PostgresDatabase implementation

PostgresDatabase::PostgresDatabase(asio::io_context& ctx, const PostgresConfig& config)
    : ctx_(ctx)
    , config_(config) {}

PostgresDatabase::~PostgresDatabase() {
    if (conn_) {
        PQfinish(conn_);
    }
}

asio::awaitable<void> PostgresDatabase::connect() {
    conn_ = PQconnectdb(config_.connection_string().c_str());

    if (PQstatus(conn_) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn_);
        PQfinish(conn_);
        conn_ = nullptr;
        throw std::runtime_error("Connection failed: " + err);
    }

    // Set non-blocking mode
    PQsetnonblocking(conn_, 1);

    co_return;
}

asio::awaitable<QueryResult> PostgresDatabase::query(std::string_view sql) {
    if (!PQsendQuery(conn_, std::string(sql).c_str())) {
        throw std::runtime_error(PQerrorMessage(conn_));
    }

    // Wait for result (simplified - real impl would use async)
    PGresult* res = PQgetResult(conn_);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string err = PQresultErrorMessage(res);
        PQclear(res);
        throw std::runtime_error("Query failed: " + err);
    }

    // Convert to QueryResult (simplified)
    PQclear(res);
    co_return QueryResult{};
}

asio::awaitable<void> PostgresDatabase::execute(std::string_view sql) {
    if (!PQsendQuery(conn_, std::string(sql).c_str())) {
        throw std::runtime_error(PQerrorMessage(conn_));
    }

    PGresult* res = PQgetResult(conn_);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string err = PQresultErrorMessage(res);
        PQclear(res);
        throw std::runtime_error("Execute failed: " + err);
    }
    PQclear(res);

    co_return;
}

std::unique_ptr<ICopyWriter> PostgresDatabase::begin_copy(
    std::string_view table,
    std::span<const std::string_view> columns) {
    return std::make_unique<PostgresCopyWriter>(conn_, ctx_, table, columns);
}

bool PostgresDatabase::is_connected() const {
    return conn_ && PQstatus(conn_) == CONNECTION_OK;
}

}  // namespace dbwriter
