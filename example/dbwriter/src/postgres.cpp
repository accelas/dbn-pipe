// SPDX-License-Identifier: MIT

#include "dbwriter/postgres.hpp"
#include <climits>
#include <poll.h>
#include <sstream>
#include <stdexcept>

namespace dbwriter {

namespace {

bool fd_ready(int fd, short events) {
    struct pollfd pfd = {fd, events, 0};
    int rc = ::poll(&pfd, 1, 0);
    if (rc <= 0) return false;
    return (pfd.revents & (events | POLLERR | POLLHUP)) != 0;
}

bool fd_readable(int fd) {
    return fd_ready(fd, POLLIN);
}

bool fd_writable(int fd) {
    return fd_ready(fd, POLLOUT);
}

asio::awaitable<void> wait_fd_readable(asio::posix::stream_descriptor& socket, int fd) {
    while (!fd_readable(fd)) {
        co_await socket.async_wait(asio::posix::stream_descriptor::wait_read, asio::use_awaitable);
    }
}

asio::awaitable<void> wait_fd_writable(asio::posix::stream_descriptor& socket, int fd) {
    while (!fd_writable(fd)) {
        co_await socket.async_wait(asio::posix::stream_descriptor::wait_write, asio::use_awaitable);
    }
}

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
//
// IMPORTANT: Connection lifetime and thread safety
// The writer holds a shared_ptr to the connection state. If PostgresDatabase
// is destroyed while the writer exists, the state->valid flag is set to false.
// All operations check this flag and throw if the connection is no longer valid.
//
// CRITICAL: Both PostgresDatabase and PostgresCopyWriter MUST be used from the
// same io_context thread. The validity check protects against accidental misuse
// but is NOT a thread-safety mechanism. Destroying PostgresDatabase from a
// different thread while a writer coroutine is suspended results in undefined
// behavior. Always ensure both objects are accessed from the io_context thread.

PostgresCopyWriter::PostgresCopyWriter(
    std::shared_ptr<PostgresConnectionState> state,
    asio::io_context& ctx,
    std::string_view table,
    std::span<const std::string_view> columns)
    : state_(std::move(state))
    , socket_(ctx, state_->pq->socket(state_->conn))
    , table_(table) {
    for (const auto& col : columns) {
        columns_.emplace_back(col);
    }
}

PostgresCopyWriter::~PostgresCopyWriter() {
    // Release the fd so ASIO doesn't close it - libpq owns the socket
    socket_.release();

    if (in_copy_ && state_->valid) {
        // Best effort abort - must clear results to avoid leaks
        state_->pq->putCopyEnd(state_->conn, "aborted");
        PGresult* res = state_->pq->getResult(state_->conn);
        if (res) state_->pq->clear(res);
        // Drain any remaining results
        while ((res = state_->pq->getResult(state_->conn)) != nullptr) {
            state_->pq->clear(res);
        }
        state_->copy_in_flight = false;  // Allow other operations
    }
}

void PostgresCopyWriter::check_valid() const {
    if (!state_->valid) {
        throw std::runtime_error("Connection closed - database was destroyed");
    }
}

asio::awaitable<void> PostgresCopyWriter::start() {
    check_valid();

    // Guard against double-start or concurrent COPY operations
    if (in_copy_) {
        throw std::runtime_error("start() already called on this CopyWriter");
    }
    if (state_->copy_in_flight) {
        throw std::runtime_error(
            "Another COPY operation is in progress on this connection");
    }

    // Mark COPY as in-flight early so concurrent operations are blocked.
    // Use scope guard to clear on failure before we set in_copy_.
    state_->copy_in_flight = true;
    struct CopyGuard {
        std::atomic<bool>& flag;
        bool committed = false;
        ~CopyGuard() { if (!committed) flag = false; }
    } guard{state_->copy_in_flight};

    auto* pq = state_->pq;
    auto* conn = state_->conn;

    // Build COPY command with quoted identifiers to prevent SQL injection
    std::ostringstream ss;
    ss << "COPY " << quote_identifier(table_) << " (";
    for (size_t i = 0; i < columns_.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << quote_identifier(columns_[i]);
    }
    ss << ") FROM STDIN WITH (FORMAT binary)";

    // Send command
    if (!pq->sendQuery(conn, ss.str().c_str())) {
        throw std::runtime_error(pq->errorMessage(conn));
    }

    // Flush command to server (required for nonblocking mode)
    int fd = socket_.native_handle();
    while (true) {
        check_valid();
        int flush_result = pq->flush(conn);
        if (flush_result == 0) break;  // All data flushed
        if (flush_result == -1) {
            throw std::runtime_error(pq->errorMessage(conn));
        }
        // flush_result == 1: More to flush, wait for writable
        co_await wait_fd_writable(socket_, fd);
    }

    // Wait for result: consume → check busy → wait if needed
    while (pq->isBusy(conn)) {
        check_valid();
        if (!fd_readable(fd)) {
            co_await wait_fd_readable(socket_, fd);
        }
        if (!pq->consumeInput(conn)) {
            throw std::runtime_error(pq->errorMessage(conn));
        }
    }

    PGresult* res = pq->getResult(conn);
    if (!res) {
        // Drain any remaining results before throwing
        while ((res = pq->getResult(conn)) != nullptr) {
            pq->clear(res);
        }
        throw std::runtime_error("COPY: no result received");
    }
    if (pq->resultStatus(res) != PGRES_COPY_IN) {
        std::string err = pq->resultErrorMessage(res);
        pq->clear(res);
        // Drain any remaining results before throwing
        while ((res = pq->getResult(conn)) != nullptr) {
            pq->clear(res);
        }
        throw std::runtime_error("COPY failed: " + err);
    }
    pq->clear(res);

    in_copy_ = true;
    guard.committed = true;  // COPY started successfully, don't clear copy_in_flight

    // Write binary COPY header
    static constexpr char header[] = "PGCOPY\n\377\r\n\0\0\0\0\0\0\0\0\0";
    co_await send_data({reinterpret_cast<const std::byte*>(header), 19});
}

asio::awaitable<void> PostgresCopyWriter::write_row(std::span<const std::byte> data) {
    check_valid();
    if (!in_copy_) {
        throw std::runtime_error("write_row() called before start() completed");
    }
    co_await send_data(data);
}

asio::awaitable<void> PostgresCopyWriter::finish() {
    check_valid();
    if (!in_copy_) {
        throw std::runtime_error("finish() called without successful start()");
    }

    auto* pq = state_->pq;
    auto* conn = state_->conn;

    // Write trailer (-1 as int16)
    static constexpr char trailer[] = "\xFF\xFF";
    co_await send_data({reinterpret_cast<const std::byte*>(trailer), 2});

    // End COPY - may need to wait for writable in nonblocking mode
    while (true) {
        check_valid();
        int result = pq->putCopyEnd(conn, nullptr);
        if (result == 1) break;
        if (result == -1) {
            // Error - drain any pending results and clear state before throwing
            std::string err = pq->errorMessage(conn);
            PGresult* res;
            while ((res = pq->getResult(conn)) != nullptr) {
                pq->clear(res);
            }
            in_copy_ = false;
            state_->copy_in_flight = false;
            throw std::runtime_error(err);
        }
        // result == 0: Would block
        co_await wait_writable();
    }

    // Flush the end-of-copy marker to ensure server receives it
    // Without this, consumeInput may wait forever for a response
    while (true) {
        check_valid();
        int flush_result = pq->flush(conn);
        if (flush_result == 0) break;  // All data flushed
        if (flush_result == -1) {
            std::string err = pq->errorMessage(conn);
            PGresult* res;
            while ((res = pq->getResult(conn)) != nullptr) {
                pq->clear(res);
            }
            in_copy_ = false;
            state_->copy_in_flight = false;
            throw std::runtime_error(err);
        }
        // flush_result == 1: More to flush, wait for writable
        co_await wait_writable();
    }

    // Note: Keep in_copy_ true until we've successfully drained all results.
    // This ensures destructor will attempt cleanup if we throw before completion.

    // Wait for result: consume → check busy → wait if needed
    int fd = socket_.native_handle();
    while (pq->isBusy(conn)) {
        check_valid();
        if (!fd_readable(fd)) {
            co_await wait_fd_readable(socket_, fd);
        }
        if (!pq->consumeInput(conn)) {
            throw std::runtime_error(pq->errorMessage(conn));
        }
    }

    PGresult* res = pq->getResult(conn);
    if (!res) {
        // Drain any remaining results before throwing
        while ((res = pq->getResult(conn)) != nullptr) {
            pq->clear(res);
        }
        in_copy_ = false;  // Cleanup complete
        state_->copy_in_flight = false;
        throw std::runtime_error("COPY finish: no result received");
    }
    if (pq->resultStatus(res) != PGRES_COMMAND_OK) {
        std::string err = pq->resultErrorMessage(res);
        pq->clear(res);
        // Drain any remaining results before throwing
        while ((res = pq->getResult(conn)) != nullptr) {
            pq->clear(res);
        }
        in_copy_ = false;  // Cleanup complete
        state_->copy_in_flight = false;
        throw std::runtime_error("COPY finish failed: " + err);
    }
    pq->clear(res);

    // Drain any remaining results
    while ((res = pq->getResult(conn)) != nullptr) {
        pq->clear(res);
    }

    in_copy_ = false;  // Cleanup complete
    state_->copy_in_flight = false;  // Allow other operations
    co_return;
}

asio::awaitable<void> PostgresCopyWriter::abort() {
    if (in_copy_ && state_->valid) {
        // Scope guard ensures in_copy_ and copy_in_flight are cleared on ALL
        // exit paths, including exceptions from wait_writable/async_wait.
        // This is critical - if cleanup is skipped, the connection becomes
        // permanently blocked for future operations.
        struct AbortGuard {
            bool& in_copy;
            std::atomic<bool>& copy_in_flight;
            ~AbortGuard() {
                in_copy = false;
                copy_in_flight = false;
            }
        } guard{in_copy_, state_->copy_in_flight};

        auto* pq = state_->pq;
        auto* conn = state_->conn;

        // End COPY with error message - may need to wait for writable
        while (true) {
            if (!state_->valid) break;  // Connection gone
            int result = pq->putCopyEnd(conn, "aborted by client");
            if (result == 1) break;
            if (result == -1) {
                // Error - connection may be broken, drain what we can
                break;
            }
            // result == 0: Would block
            co_await wait_writable();
        }

        // Flush the abort message to server - without this, consumeInput may hang
        while (state_->valid) {
            int flush_result = pq->flush(conn);
            if (flush_result == 0) break;  // All data flushed
            if (flush_result == -1) break;  // Error, can't do more
            // flush_result == 1: More to flush, wait for writable
            co_await wait_writable();
        }

        // Wait for result: consume → check busy → wait if needed
        int fd = socket_.native_handle();
        while (state_->valid && pq->isBusy(conn)) {
            if (fd >= 0 && !fd_readable(fd)) {
                co_await wait_fd_readable(socket_, fd);
            }
            if (!state_->valid) break;
            if (!pq->consumeInput(conn)) {
                break;  // Connection error, can't do more
            }
        }

        // Drain all results
        if (state_->valid) {
            PGresult* res;
            while ((res = pq->getResult(conn)) != nullptr) {
                pq->clear(res);
            }
        }
        // in_copy_ and copy_in_flight cleared by guard destructor
    }
    co_return;
}

asio::awaitable<void> PostgresCopyWriter::wait_writable() {
    int fd = socket_.native_handle();
    if (fd < 0) {
        co_await socket_.async_wait(asio::posix::stream_descriptor::wait_write, asio::use_awaitable);
        co_return;
    }
    co_await wait_fd_writable(socket_, fd);
}

asio::awaitable<void> PostgresCopyWriter::send_data(std::span<const std::byte> data) {
    check_valid();

    if (data.size() > static_cast<size_t>(INT_MAX)) {
        throw std::runtime_error("Data size exceeds maximum allowed for PQputCopyData");
    }

    auto* pq = state_->pq;
    auto* conn = state_->conn;

    while (true) {
        check_valid();
        int result = pq->putCopyData(conn,
            reinterpret_cast<const char*>(data.data()),
            static_cast<int>(data.size()));

        if (result == 1) {
            // Success - now flush to ensure data reaches server
            // In nonblocking mode, data may be buffered until flushed
            while (true) {
                check_valid();
                int flush_result = pq->flush(conn);
                if (flush_result == 0) break;  // All data flushed
                if (flush_result == -1) {
                    throw std::runtime_error(pq->errorMessage(conn));
                }
                // flush_result == 1: More to flush, wait for writable
                co_await wait_writable();
            }
            co_return;
        }

        if (result == -1) {
            throw std::runtime_error(pq->errorMessage(conn));
        }

        // result == 0: Would block, wait for writable and retry
        co_await wait_writable();
    }
}

// PostgresDatabase implementation
//
// IMPORTANT: Lifetime handling
// PostgresDatabase uses shared state with PostgresCopyWriter instances. When
// PostgresDatabase is destroyed, state_->valid is set to false BEFORE closing
// the connection. Writers check this flag and throw if connection is gone.
// This prevents use-after-free when writer outlives database.
//
// IMPORTANT: Concurrency and thread safety
// PostgresDatabase is NOT thread-safe. All operations (query, execute, begin_copy)
// must be serialized - only one operation may be in flight at a time. Concurrent
// calls on the same connection corrupt libpq protocol state.
//
// CRITICAL: PostgresDatabase, all PostgresCopyWriter instances, and the io_context
// MUST all be used from the SAME THREAD. The validity flag is NOT a synchronization
// mechanism - it only catches accidental misuse when destruction happens before
// the writer is done. Cross-thread destruction while a coroutine is suspended
// results in undefined behavior. Typical usage: run io_context on a dedicated
// thread, post all database operations to that thread.

PostgresDatabase::PostgresDatabase(asio::io_context& ctx, const PostgresConfig& config,
                                   ILibPq& pq)
    : ctx_(ctx)
    , config_(config)
    , state_(std::make_shared<PostgresConnectionState>())
    , pq_(pq) {
    state_->pq = &pq_;
}

PostgresDatabase::~PostgresDatabase() {
    // Mark connection as invalid BEFORE closing - writers check this flag
    state_->valid = false;
    if (state_->conn) {
        pq_.finish(state_->conn);
        state_->conn = nullptr;
    }
}

asio::awaitable<void> PostgresDatabase::connect() {
    PGconn* conn = pq_.connectdb(config_.connection_string().c_str());

    if (pq_.status(conn) != CONNECTION_OK) {
        std::string err = pq_.errorMessage(conn);
        pq_.finish(conn);
        throw std::runtime_error("Connection failed: " + err);
    }

    // Set non-blocking mode
    if (pq_.setnonblocking(conn, 1) != 0) {
        std::string err = pq_.errorMessage(conn);
        pq_.finish(conn);
        throw std::runtime_error("Failed to set non-blocking mode: " + err);
    }

    // Populate shared state - now writers can use this connection
    state_->conn = conn;
    state_->valid = true;

    co_return;
}

asio::awaitable<QueryResult> PostgresDatabase::query(std::string_view sql) {
    if (!is_connected()) {
        throw std::runtime_error("Not connected to database");
    }
    check_no_operation_in_flight();

    // Guard to track operation in flight and clear on exit
    operation_in_flight_ = true;
    struct OperationGuard {
        bool& flag;
        ~OperationGuard() { flag = false; }
    } guard{operation_in_flight_};

    PGconn* conn = state_->conn;

    if (!pq_.sendQuery(conn, std::string(sql).c_str())) {
        throw std::runtime_error(pq_.errorMessage(conn));
    }

    // Create socket descriptor for async waits
    int fd = pq_.socket(conn);
    if (fd < 0) {
        throw std::runtime_error("Invalid socket from libpq connection");
    }
    asio::posix::stream_descriptor socket(ctx_, fd);

    // Flush command to server (required for nonblocking mode)
    while (true) {
        int flush_result = pq_.flush(conn);
        if (flush_result == 0) break;  // All data flushed
        if (flush_result == -1) {
            socket.release();
            throw std::runtime_error(pq_.errorMessage(conn));
        }
        // flush_result == 1: More to flush, wait for writable
        co_await wait_fd_writable(socket, fd);
    }

    // Wait for result: consume → check busy → wait if needed

    // Scope guard ensures socket.release() and result draining on all exit paths
    ILibPq* pq = &pq_;
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

    while (pq_.isBusy(conn)) {
        if (!fd_readable(fd)) {
            co_await wait_fd_readable(socket, fd);
        }
        pq_.consumeInput(conn);
    }

    PGresult* res = pq_.getResult(conn);
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
    check_no_operation_in_flight();

    // Guard to track operation in flight and clear on exit
    operation_in_flight_ = true;
    struct OperationGuard {
        bool& flag;
        ~OperationGuard() { flag = false; }
    } guard{operation_in_flight_};

    PGconn* conn = state_->conn;

    if (!pq_.sendQuery(conn, std::string(sql).c_str())) {
        throw std::runtime_error(pq_.errorMessage(conn));
    }

    // Create socket descriptor for async waits
    int fd = pq_.socket(conn);
    if (fd < 0) {
        throw std::runtime_error("Invalid socket from libpq connection");
    }
    asio::posix::stream_descriptor socket(ctx_, fd);

    // Flush command to server (required for nonblocking mode)
    while (true) {
        int flush_result = pq_.flush(conn);
        if (flush_result == 0) break;  // All data flushed
        if (flush_result == -1) {
            socket.release();
            throw std::runtime_error(pq_.errorMessage(conn));
        }
        // flush_result == 1: More to flush, wait for writable
        co_await wait_fd_writable(socket, fd);
    }

    // Wait for result: consume → check busy → wait if needed

    // Scope guard ensures socket.release() and result draining on all exit paths
    ILibPq* pq = &pq_;
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

    while (pq_.isBusy(conn)) {
        if (!fd_readable(fd)) {
            co_await wait_fd_readable(socket, fd);
        }
        pq_.consumeInput(conn);
    }

    PGresult* res = pq_.getResult(conn);
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
    check_no_operation_in_flight();
    // Note: We don't set operation_in_flight_ here because the COPY operation
    // is managed by the returned writer. The writer will hold the connection
    // in COPY mode until finish() or abort() is called. Calling query/execute
    // while a COPY is in progress will fail at the libpq level.
    return std::make_unique<PostgresCopyWriter>(state_, ctx_, table, columns);
}

bool PostgresDatabase::is_connected() const {
    return state_->valid && state_->conn && pq_.status(state_->conn) == CONNECTION_OK;
}

void PostgresDatabase::check_no_operation_in_flight() const {
    if (operation_in_flight_) {
        throw std::runtime_error(
            "Concurrent database operation detected. PostgresDatabase only "
            "supports one operation at a time. Serialize calls to query(), "
            "execute(), and begin_copy().");
    }
    if (state_->copy_in_flight) {
        throw std::runtime_error(
            "COPY operation in progress. Cannot execute query/execute/begin_copy "
            "while a COPY is active. Call finish() or abort() on the CopyWriter first.");
    }
}

}  // namespace dbwriter
