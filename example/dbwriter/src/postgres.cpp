// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#include "dbwriter/postgres.hpp"
#include <climits>
#include <sstream>
#include <stdexcept>

namespace dbwriter {

namespace {
// Quote a PostgreSQL identifier to prevent SQL injection.
// Doubles any embedded double-quotes and wraps in double-quotes.
std::string quote_identifier(std::string_view ident) {
    std::string result;
    result.reserve(ident.size() + 2);
    result += '"';
    for (char c : ident) {
        if (c == '"') {
            result += '"';  // Double the quote
        }
        result += c;
    }
    result += '"';
    return result;
}

// Escape a connection string value (single quotes, backslashes).
// Per libpq docs, values containing spaces/special chars need quoting.
std::string escape_conninfo_value(std::string_view val) {
    // If value contains no special characters, return as-is
    bool needs_quoting = false;
    for (char c : val) {
        if (c == ' ' || c == '\'' || c == '\\' || c == '=' || c == '\0') {
            needs_quoting = true;
            break;
        }
    }
    if (!needs_quoting) {
        return std::string(val);
    }

    std::string result;
    result.reserve(val.size() + 2);
    result += '\'';
    for (char c : val) {
        if (c == '\'' || c == '\\') {
            result += '\\';  // Escape with backslash
        }
        result += c;
    }
    result += '\'';
    return result;
}

// Concrete IRow implementation backed by copied string data
class PostgresRow : public IRow {
public:
    PostgresRow(std::vector<std::string> values, std::vector<bool> nulls)
        : values_(std::move(values)), nulls_(std::move(nulls)) {}

    int64_t get_int64(std::size_t col) const override {
        if (col >= values_.size() || nulls_[col]) {
            return 0;
        }
        return std::stoll(values_[col]);
    }

    int32_t get_int32(std::size_t col) const override {
        if (col >= values_.size() || nulls_[col]) {
            return 0;
        }
        return std::stoi(values_[col]);
    }

    std::string_view get_string(std::size_t col) const override {
        if (col >= values_.size() || nulls_[col]) {
            return {};
        }
        return values_[col];
    }

    bool is_null(std::size_t col) const override {
        return col >= nulls_.size() || nulls_[col];
    }

private:
    std::vector<std::string> values_;
    std::vector<bool> nulls_;
};
}  // namespace

std::string PostgresConfig::connection_string() const {
    std::ostringstream ss;
    ss << "host=" << escape_conninfo_value(host)
       << " port=" << port
       << " dbname=" << escape_conninfo_value(database)
       << " user=" << escape_conninfo_value(user)
       << " password=" << escape_conninfo_value(password);
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
    // Build COPY command with quoted identifiers to prevent SQL injection
    std::ostringstream ss;
    ss << "COPY " << quote_identifier(table_) << " (";
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << quote_identifier(columns_[i]);
    }
    ss << ") FROM STDIN WITH (FORMAT binary)";

    // Send command
    if (!pq_.sendQuery(conn_, ss.str().c_str())) {
        throw std::runtime_error(pq_.errorMessage(conn_));
    }

    // Flush command to server (required for nonblocking mode)
    while (true) {
        int flush_result = pq_.flush(conn_);
        if (flush_result == 0) break;  // All data flushed
        if (flush_result == -1) {
            throw std::runtime_error(pq_.errorMessage(conn_));
        }
        // flush_result == 1: More to flush, wait for writable
        co_await socket_.async_wait(
            asio::posix::stream_descriptor::wait_write,
            asio::use_awaitable);
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
            // Error - drain any pending results and clear state before throwing
            std::string err = pq_.errorMessage(conn_);
            PGresult* res;
            while ((res = pq_.getResult(conn_)) != nullptr) {
                pq_.clear(res);
            }
            in_copy_ = false;
            throw std::runtime_error(err);
        }
        // result == 0: Would block
        co_await wait_writable();
    }

    // Note: Keep in_copy_ true until we've successfully drained all results.
    // This ensures destructor will attempt cleanup if we throw before completion.

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
        in_copy_ = false;  // Cleanup complete
        throw std::runtime_error("COPY finish: no result received");
    }
    if (pq_.resultStatus(res) != PGRES_COMMAND_OK) {
        std::string err = pq_.resultErrorMessage(res);
        pq_.clear(res);
        // Drain any remaining results before throwing
        while ((res = pq_.getResult(conn_)) != nullptr) {
            pq_.clear(res);
        }
        in_copy_ = false;  // Cleanup complete
        throw std::runtime_error("COPY finish failed: " + err);
    }
    pq_.clear(res);

    // Drain any remaining results
    while ((res = pq_.getResult(conn_)) != nullptr) {
        pq_.clear(res);
    }

    in_copy_ = false;  // Cleanup complete
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
    if (data.size() > static_cast<size_t>(INT_MAX)) {
        throw std::runtime_error("Data size exceeds maximum allowed for PQputCopyData");
    }

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
//
// IMPORTANT: Lifetime contract
// PostgresDatabase must outlive all PostgresCopyWriter instances created via
// begin_copy(). The writers hold a raw PGconn* pointer that becomes invalid
// when PostgresDatabase is destroyed. Destroying PostgresDatabase while a
// writer exists results in undefined behavior.
//
// IMPORTANT: Concurrency
// PostgresDatabase is NOT thread-safe. All operations (query, execute, begin_copy)
// must be serialized - only one operation may be in flight at a time. Concurrent
// calls on the same connection corrupt libpq protocol state. Use a single
// io_context thread or external synchronization.

PostgresDatabase::PostgresDatabase(asio::io_context& ctx, const PostgresConfig& config,
                                   ILibPq& pq)
    : ctx_(ctx)
    , config_(config)
    , pq_(pq) {}

PostgresDatabase::~PostgresDatabase() {
    // Note: Caller must ensure all PostgresCopyWriter instances from begin_copy()
    // are destroyed before this destructor runs.
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
    if (!is_connected()) {
        throw std::runtime_error("Not connected to database");
    }
    if (!pq_.sendQuery(conn_, std::string(sql).c_str())) {
        throw std::runtime_error(pq_.errorMessage(conn_));
    }

    // Create socket descriptor for async waits
    asio::posix::stream_descriptor socket(ctx_, pq_.socket(conn_));

    // Flush command to server (required for nonblocking mode)
    while (true) {
        int flush_result = pq_.flush(conn_);
        if (flush_result == 0) break;  // All data flushed
        if (flush_result == -1) {
            socket.release();
            throw std::runtime_error(pq_.errorMessage(conn_));
        }
        // flush_result == 1: More to flush, wait for writable
        co_await socket.async_wait(
            asio::posix::stream_descriptor::wait_write,
            asio::use_awaitable);
    }

    // Wait for result: consume → check busy → wait if needed

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

    // Convert PGresult to QueryResult
    int nrows = pq_.ntuples(res);
    int ncols = pq_.nfields(res);

    std::vector<std::unique_ptr<IRow>> rows;
    rows.reserve(static_cast<size_t>(nrows));

    for (int r = 0; r < nrows; ++r) {
        std::vector<std::string> values;
        std::vector<bool> nulls;
        values.reserve(static_cast<size_t>(ncols));
        nulls.reserve(static_cast<size_t>(ncols));

        for (int c = 0; c < ncols; ++c) {
            nulls.push_back(pq_.getisnull(res, r, c) != 0);
            if (nulls.back()) {
                values.emplace_back();
            } else {
                values.emplace_back(pq_.getvalue(res, r, c));
            }
        }
        rows.push_back(std::make_unique<PostgresRow>(
            std::move(values), std::move(nulls)));
    }

    pq_.clear(res);

    // Remaining results drained by scope_guard
    co_return QueryResult{std::move(rows)};
}

asio::awaitable<void> PostgresDatabase::execute(std::string_view sql) {
    if (!is_connected()) {
        throw std::runtime_error("Not connected to database");
    }
    if (!pq_.sendQuery(conn_, std::string(sql).c_str())) {
        throw std::runtime_error(pq_.errorMessage(conn_));
    }

    // Create socket descriptor for async waits
    asio::posix::stream_descriptor socket(ctx_, pq_.socket(conn_));

    // Flush command to server (required for nonblocking mode)
    while (true) {
        int flush_result = pq_.flush(conn_);
        if (flush_result == 0) break;  // All data flushed
        if (flush_result == -1) {
            socket.release();
            throw std::runtime_error(pq_.errorMessage(conn_));
        }
        // flush_result == 1: More to flush, wait for writable
        co_await socket.async_wait(
            asio::posix::stream_descriptor::wait_write,
            asio::use_awaitable);
    }

    // Wait for result: consume → check busy → wait if needed

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
    if (!is_connected()) {
        throw std::runtime_error("Not connected to database");
    }
    return std::make_unique<PostgresCopyWriter>(conn_, ctx_, table, columns, pq_);
}

bool PostgresDatabase::is_connected() const {
    return conn_ && pq_.status(conn_) == CONNECTION_OK;
}

}  // namespace dbwriter
