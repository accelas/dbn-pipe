// SPDX-License-Identifier: MIT

// End-to-end test: Download from Databento -> Write to PostgreSQL
// Uses ASIO event loop for everything (both dbn-pipe and PostgreSQL)
//
// Run with environment variables:
//   DATABENTO_API_KEY, POSTGRES_HOST, POSTGRES_PORT, POSTGRES_DB, POSTGRES_USER, POSTGRES_PASSWORD
//
// Required PostgreSQL table schema:
//
//   CREATE TABLE trades (
//       ts_event_ns BIGINT,
//       ts_event TIMESTAMPTZ,
//       instrument_id INTEGER,
//       price BIGINT,
//       size INTEGER
//   );
//
// Example setup with podman:
//   podman run -d --name pg-test -p 15432:5432
//       -e POSTGRES_USER=testuser -e POSTGRES_PASSWORD=testpass -e POSTGRES_DB=testdb
//       postgres:15
//   psql -h localhost -p 15432 -U testuser -d testdb -c "CREATE TABLE trades (...)"

#include "dbwriter/postgres.hpp"
#include "dbwriter/types.hpp"
#include "dbn_pipe/pg/mapper.hpp"
#include "dbn_pipe/pg/pg_types.hpp"
#include "dbwriter/asio_event_loop.hpp"
#include "dbn_pipe/table/table.hpp"

#include "dbn_pipe/client.hpp"

#include <gtest/gtest.h>
#include <iostream>
#include <iomanip>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

namespace dbwriter {
namespace {

// Rate tracker with sliding window for peak/average calculation
class RateTracker {
public:
    explicit RateTracker(std::chrono::milliseconds window = std::chrono::seconds(1))
        : window_(window) {}

    void Record(int64_t count = 1) {
        std::lock_guard lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        samples_.push_back({now, count});
        total_count_ += count;

        // Remove old samples outside window
        while (!samples_.empty() &&
               now - samples_.front().time > window_) {
            samples_.pop_front();
        }

        // Calculate current rate
        if (samples_.size() >= 2) {
            auto duration = samples_.back().time - samples_.front().time;
            auto duration_sec = std::chrono::duration<double>(duration).count();
            if (duration_sec > 0) {
                int64_t window_count = 0;
                for (const auto& s : samples_) {
                    window_count += s.count;
                }
                double rate = window_count / duration_sec;
                if (rate > peak_rate_) {
                    peak_rate_ = rate;
                }
            }
        }
    }

    double GetPeakRate() const {
        std::lock_guard lock(mutex_);
        return peak_rate_;
    }

    double GetAverageRate(std::chrono::steady_clock::time_point start) const {
        std::lock_guard lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto duration_sec = std::chrono::duration<double>(now - start).count();
        if (duration_sec > 0) {
            return total_count_ / duration_sec;
        }
        return 0;
    }

    int64_t GetTotalCount() const {
        std::lock_guard lock(mutex_);
        return total_count_;
    }

private:
    struct Sample {
        std::chrono::steady_clock::time_point time;
        int64_t count;
    };

    std::chrono::milliseconds window_;
    std::deque<Sample> samples_;
    int64_t total_count_ = 0;
    double peak_rate_ = 0;
    mutable std::mutex mutex_;
};

class E2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* api_key = std::getenv("DATABENTO_API_KEY");
        if (!api_key) {
            GTEST_SKIP() << "DATABENTO_API_KEY not set";
        }
        api_key_ = api_key;

        const char* host = std::getenv("POSTGRES_HOST");
        if (!host) {
            GTEST_SKIP() << "POSTGRES_HOST not set";
        }

        pg_config_.host = host;
        pg_config_.port = std::atoi(std::getenv("POSTGRES_PORT") ?: "5432");
        pg_config_.database = std::getenv("POSTGRES_DB") ?: "testdb";
        pg_config_.user = std::getenv("POSTGRES_USER") ?: "testuser";
        pg_config_.password = std::getenv("POSTGRES_PASSWORD") ?: "testpass";
    }

    std::string api_key_;
    PostgresConfig pg_config_;
};

// Define the trades table schema matching what we'll store
constexpr auto trades_table = dbn_pipe::Table{"trades",
    dbn_pipe::Column<"ts_event_ns", dbn_pipe::Int64>{},
    dbn_pipe::Column<"ts_event", dbn_pipe::Timestamp>{},
    dbn_pipe::Column<"instrument_id", dbn_pipe::Int32>{},
    dbn_pipe::Column<"price", dbn_pipe::Int64>{},
    dbn_pipe::Column<"size", dbn_pipe::Int32>{},
};

using TradesRow = decltype(trades_table)::RowType;

TEST_F(E2ETest, DownloadAndWriteTrades) {
    using namespace dbn_pipe;

    // Single ASIO context for everything
    asio::io_context ctx;

    // ASIO-based event loop that implements IEventLoop
    AsioEventLoop loop(ctx);
    loop.Poll();  // Initialize thread ID

    // Connect to PostgreSQL (synchronous since PQconnectdb is blocking anyway)
    std::cout << "Connecting to PostgreSQL at " << pg_config_.host
              << ":" << pg_config_.port << std::endl;

    PostgresDatabase db(ctx, pg_config_);
    try {
        // Call connect directly without coroutine wrapper
        // since PQconnectdb is synchronous
        auto conn_str = pg_config_.connection_string();
        std::cout << "Connection string: " << conn_str << std::endl;

        // Run the connect coroutine synchronously
        std::exception_ptr ex;
        bool done = false;
        asio::co_spawn(ctx, db.connect(),
            [&](std::exception_ptr e) {
                ex = e;
                done = true;
            });

        // Poll until done (PQconnectdb is sync so should complete quickly)
        while (!done) {
            if (ctx.run_one() == 0) {
                ctx.restart();
            }
        }

        if (ex) {
            std::rethrow_exception(ex);
        }
    } catch (const std::exception& e) {
        FAIL() << "Failed to connect to PostgreSQL: " << e.what();
    }
    ASSERT_TRUE(db.is_connected()) << "Failed to connect to PostgreSQL (is_connected=false)";
    std::cout << "PostgreSQL connected!" << std::endl;

    // Truncate table for clean test
    auto truncate_task = [&]() -> asio::awaitable<void> {
        co_await db.execute("TRUNCATE TABLE trades");
    };
    asio::co_spawn(ctx, truncate_task(), asio::detached);
    ctx.run();
    ctx.restart();

    // Create HistoricalClient using ASIO event loop
    auto client = HistoricalClient::Create(loop, api_key_);

    // Request: Download trades for QQQ (NASDAQ-100 ETF) for 1 minute
    // Using 2025-12-01 14:30:00 UTC (9:30 AM ET, Monday - market open)
    HistoricalRequest req{
        "XNAS.ITCH",           // dataset (NASDAQ TotalView)
        "QQQ",                 // symbols
        "trades",              // schema
        1764599400000000000ULL, // start: 2025-12-01 14:30:00 UTC
        1764599460000000000ULL, // end: 2025-12-01 14:31:00 UTC (1 minute)
        "",                    // stype_in
        ""                     // stype_out
    };
    client->SetRequest(req);

    // Rate trackers
    RateTracker download_rate;
    RateTracker insert_rate;
    auto start_time = std::chrono::steady_clock::now();

    // State
    std::atomic<bool> complete{false};
    std::atomic<bool> error_occurred{false};
    std::string error_msg;

    // Create mapper
    auto mapper = dbn_pipe::pg::make_mapper(trades_table);

    // Batch buffer - collect all records, flush after download completes
    std::vector<TradesRow> batch_buffer;

    auto flush_batch = [&]() {
        if (batch_buffer.empty()) return;

        auto columns = trades_table.column_names();
        std::vector<std::string_view> col_views(columns.begin(), columns.end());
        auto writer = db.begin_copy("trades", col_views);

        size_t batch_count = batch_buffer.size();
        bool write_done = false;
        std::exception_ptr write_error;

        auto write_task = [&]() -> asio::awaitable<void> {
            try {
                co_await writer->start();
                dbn_pipe::pg::ByteBuffer buf;
                for (const auto& row : batch_buffer) {
                    mapper.encode_row(row, buf);
                    co_await writer->write_row(buf.view());
                    buf.clear();
                }
                co_await writer->finish();
                insert_rate.Record(batch_count);
            } catch (...) {
                write_error = std::current_exception();
            }
            write_done = true;
        };

        asio::co_spawn(ctx, write_task(), asio::detached);

        // Wait for write to complete
        while (!write_done) {
            ctx.run_one();
        }

        if (write_error) {
            std::rethrow_exception(write_error);
        }

        batch_buffer.clear();
    };

    // Set up record handler
    client->OnRecord([&](const RecordRef& ref) {
        download_rate.Record(1);
        if (download_rate.GetTotalCount() <= 5) {
            std::cout << "Record received #" << download_rate.GetTotalCount() << std::endl;
        }

        // Convert chrono time_point to raw nanoseconds
        auto ts_ns = ref.Header().ts_event.time_since_epoch().count();

        TradesRow row;
        row.get<"ts_event_ns">() = static_cast<int64_t>(ts_ns);
        row.get<"ts_event">() = static_cast<int64_t>(ts_ns);  // raw int64_t unix nanoseconds
        row.get<"instrument_id">() = ref.Header().instrument_id;
        row.get<"price">() = 0;  // Would need TradeMsg cast for real price
        row.get<"size">() = 0;

        batch_buffer.push_back(row);
        // Note: Don't flush during download to avoid "another command in progress"
        // PostgreSQL single connection can't handle concurrent commands.
        // We flush everything after download completes.
    });

    client->OnError([&](const Error& e) {
        std::cout << "ERROR: " << e.message << std::endl;
        error_occurred = true;
        error_msg = e.message;
        loop.Stop();
    });

    client->OnComplete([&]() {
        std::cout << "Download complete!" << std::endl;
        complete = true;
        loop.Stop();
    });

    // Connect and start
    std::cout << "Connecting to Databento..." << std::endl;
    client->Connect();
    std::cout << "Starting stream..." << std::endl;
    client->Start();
    std::cout << "Client state: " << static_cast<int>(client->GetState()) << std::endl;

    // Run unified event loop with timeout
    while (!complete && !error_occurred) {
        ctx.run_for(std::chrono::milliseconds(100));

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > std::chrono::seconds(120)) {
            FAIL() << "Timeout waiting for download to complete";
        }
    }

    // Flush remaining batch
    std::cout << "Flushing remaining batch, size=" << batch_buffer.size() << std::endl;
    std::cout << "DB connected: " << db.is_connected() << std::endl;
    ctx.restart();  // Reset stopped state before spawning new work
    if (!batch_buffer.empty()) {
        flush_batch();
    }

    // Drain any remaining ASIO work
    ctx.run();

    if (error_occurred) {
        FAIL() << "Error during download: " << error_msg;
    }

    // Print rate statistics
    std::cout << "\n========== Rate Statistics ==========\n";
    std::cout << "Download:\n";
    std::cout << "  Total records: " << download_rate.GetTotalCount() << "\n";
    std::cout << "  Peak rate:     " << std::fixed << std::setprecision(0)
              << download_rate.GetPeakRate() << " records/sec\n";
    std::cout << "  Avg rate:      " << std::fixed << std::setprecision(0)
              << download_rate.GetAverageRate(start_time) << " records/sec\n";
    std::cout << "\nInsert:\n";
    std::cout << "  Total records: " << insert_rate.GetTotalCount() << "\n";
    std::cout << "  Peak rate:     " << std::fixed << std::setprecision(0)
              << insert_rate.GetPeakRate() << " records/sec\n";
    std::cout << "  Avg rate:      " << std::fixed << std::setprecision(0)
              << insert_rate.GetAverageRate(start_time) << " records/sec\n";
    std::cout << "=====================================\n";

    EXPECT_GT(download_rate.GetTotalCount(), 0) << "Expected to receive some records";
}

}  // namespace
}  // namespace dbwriter
