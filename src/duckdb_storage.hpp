// SPDX-License-Identifier: MIT

// src/duckdb_storage.hpp
#pragma once

#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <duckdb.hpp>

#include "dbn_pipe/storage.hpp"
#include "dbn_pipe/trading_date.hpp"

namespace dbn_pipe {

/// DuckDB-backed storage for symbol mappings and download progress.
///
/// Uses an in-memory database by default for fast lookups, with optional
/// file persistence for durability across restarts.
///
/// **Schema versioning:** The database includes a schema_meta table that
/// tracks the schema version.  An incompatible version causes an exception
/// on open to prevent data corruption.
///
/// **Cache limit:** Symbol mappings can be limited to a maximum number of
/// entries.  When the limit is reached, LRU (least recently used) entries
/// are evicted.  Set @c max_mappings=0 to disable the limit.
///
/// **Thread safety:** Not thread-safe.  Use external synchronization if
/// accessed from multiple threads.
class DuckDbStorage : public IStorage {
public:
    /// Current schema version.  Increment when making breaking changes.
    static constexpr int kSchemaVersion = 2;  ///< v2: added last_accessed column.

    /// Default maximum number of cached symbol mappings (0 = unlimited).
    static constexpr size_t kDefaultMaxMappings = 100000;

    /// Construct with an optional database path and cache limit.
    ///
    /// An empty @p db_path (the default) creates an in-memory database.
    /// @param db_path       Filesystem path to a DuckDB database file, or
    ///                      empty for in-memory operation.
    /// @param max_mappings  Maximum symbol mappings to retain (0 = unlimited).
    /// @throws std::runtime_error if the schema version is incompatible.
    explicit DuckDbStorage(const std::string& db_path = "",
                           size_t max_mappings = kDefaultMaxMappings)
        : db_(db_path.empty() ? nullptr
                              : std::make_unique<duckdb::DuckDB>(db_path))
        , conn_(db_ ? std::make_unique<duckdb::Connection>(*db_)
                    : nullptr)
        , max_mappings_(max_mappings) {
        // Create in-memory DB if no path
        if (!db_) {
            db_ = std::make_unique<duckdb::DuckDB>(nullptr);
            conn_ = std::make_unique<duckdb::Connection>(*db_);
        }
        InitSchema();
        ValidateSchemaVersion();
    }

    /// Factory method that returns an expected instead of throwing.
    /// @param db_path       Filesystem path, or empty for in-memory.
    /// @param max_mappings  Maximum symbol mappings to retain (0 = unlimited).
    /// @return A unique_ptr on success, or an error string on failure.
    static std::expected<std::unique_ptr<DuckDbStorage>, std::string>
    Create(const std::string& db_path = "", size_t max_mappings = kDefaultMaxMappings) {
        try {
            auto storage = std::unique_ptr<DuckDbStorage>(
                new DuckDbStorage(db_path, max_mappings));
            return storage;
        } catch (const std::exception& e) {
            return std::unexpected(std::string(e.what()));
        }
    }

    void StoreMapping(uint32_t instrument_id, const std::string& symbol,
                      const TradingDate& start, const TradingDate& end) override {
        // Evict LRU entries if cache limit would be exceeded
        if (max_mappings_ > 0) {
            EvictIfNeeded();
        }

        auto sql = fmt::format(
            "INSERT INTO symbol_mappings "
            "(instrument_id, symbol, start_date, end_date, last_accessed) VALUES ("
            "{}, '{}', '{}', '{}', nextval('access_counter')) "
            "ON CONFLICT (instrument_id, start_date) DO UPDATE SET "
            "symbol = EXCLUDED.symbol, "
            "end_date = EXCLUDED.end_date, "
            "last_accessed = nextval('access_counter')",
            instrument_id, EscapeString(symbol),
            start.ToIsoString(), end.ToIsoString());

        auto result = conn_->Query(sql);
        if (result->HasError()) {
            throw std::runtime_error("Failed to execute StoreMapping: " + result->GetError());
        }
    }

    std::optional<std::string> LookupSymbol(uint32_t instrument_id,
                                             const TradingDate& date) override {
        auto sql = fmt::format(
            "SELECT symbol, start_date FROM symbol_mappings "
            "WHERE instrument_id = {} "
            "AND start_date <= '{}' "
            "AND end_date >= '{}' "
            "LIMIT 1",
            instrument_id, date.ToIsoString(), date.ToIsoString());

        auto result = conn_->Query(sql);
        if (result->HasError()) {
            return std::nullopt;
        }

        auto chunk = result->Fetch();
        if (!chunk || chunk->size() == 0) {
            return std::nullopt;
        }

        std::string symbol = chunk->GetValue(0, 0).ToString();
        std::string start_date = chunk->GetValue(1, 0).ToString();

        // Update last_accessed counter (LRU tracking)
        auto update_sql = fmt::format(
            "UPDATE symbol_mappings SET last_accessed = nextval('access_counter') "
            "WHERE instrument_id = {} AND start_date = '{}'",
            instrument_id, start_date);
        conn_->Query(update_sql);

        return symbol;
    }

    void StoreProgress(const std::string& job_id, const std::string& filename,
                       const DownloadProgress& progress) override {
        // Serialize completed_ranges to JSON-like string
        // Each range: "[uint64,uint64]" ~40 chars max, plus brackets and commas
        std::string ranges_str;
        ranges_str.reserve(2 + progress.completed_ranges.size() * 45);
        auto ranges_out = std::back_inserter(ranges_str);
        *ranges_out++ = '[';
        for (size_t i = 0; i < progress.completed_ranges.size(); ++i) {
            if (i > 0) *ranges_out++ = ',';
            ranges_out = fmt::format_to(ranges_out, "[{},{}]",
                progress.completed_ranges[i].first,
                progress.completed_ranges[i].second);
        }
        *ranges_out++ = ']';

        auto sql = fmt::format(
            "INSERT INTO download_progress "
            "(job_id, filename, sha256_expected, total_size, completed_ranges) VALUES ("
            "'{}', '{}', '{}', {}, '{}') "
            "ON CONFLICT (job_id, filename) DO UPDATE SET "
            "sha256_expected = EXCLUDED.sha256_expected, "
            "total_size = EXCLUDED.total_size, "
            "completed_ranges = EXCLUDED.completed_ranges",
            EscapeString(job_id), EscapeString(filename),
            EscapeString(progress.sha256_expected), progress.total_size,
            ranges_str);

        conn_->Query(sql);
    }

    std::optional<DownloadProgress> LoadProgress(const std::string& job_id,
                                                  const std::string& filename) override {
        auto sql = fmt::format(
            "SELECT sha256_expected, total_size, completed_ranges "
            "FROM download_progress "
            "WHERE job_id = '{}' AND filename = '{}'",
            EscapeString(job_id), EscapeString(filename));

        auto result = conn_->Query(sql);
        if (result->HasError()) {
            return std::nullopt;
        }

        auto chunk = result->Fetch();
        if (!chunk || chunk->size() == 0) {
            return std::nullopt;
        }

        DownloadProgress progress;
        progress.sha256_expected = chunk->GetValue(0, 0).ToString();
        progress.total_size = static_cast<uint64_t>(
            chunk->GetValue(1, 0).GetValue<int64_t>());

        // Parse completed_ranges from JSON-like string
        std::string ranges_str = chunk->GetValue(2, 0).ToString();
        ParseRanges(ranges_str, progress.completed_ranges);

        return progress;
    }

    void ClearProgress(const std::string& job_id, const std::string& filename) override {
        auto sql = fmt::format(
            "DELETE FROM download_progress "
            "WHERE job_id = '{}' AND filename = '{}'",
            EscapeString(job_id), EscapeString(filename));

        conn_->Query(sql);
    }

    std::vector<std::pair<std::string, std::string>> ListIncompleteDownloads() override {
        auto result = conn_->Query("SELECT job_id, filename FROM download_progress");

        std::vector<std::pair<std::string, std::string>> downloads;

        if (result->HasError()) {
            return downloads;
        }

        while (true) {
            auto chunk = result->Fetch();
            if (!chunk || chunk->size() == 0) {
                break;
            }

            for (duckdb::idx_t i = 0; i < chunk->size(); ++i) {
                downloads.emplace_back(
                    chunk->GetValue(0, i).ToString(),
                    chunk->GetValue(1, i).ToString());
            }
        }

        return downloads;
    }

    /// @return Number of symbol mappings currently stored.
    size_t MappingCount() const {
        auto result = conn_->Query("SELECT COUNT(*) FROM symbol_mappings");
        if (result->HasError()) return 0;
        auto chunk = result->Fetch();
        if (!chunk || chunk->size() == 0) return 0;
        return static_cast<size_t>(chunk->GetValue(0, 0).GetValue<int64_t>());
    }

private:
    void InitSchema() {
        // Schema version tracking
        conn_->Query(R"(
            CREATE TABLE IF NOT EXISTS schema_meta (
                key VARCHAR PRIMARY KEY,
                value VARCHAR NOT NULL
            )
        )");

        // Set schema version if not exists
        auto sql = fmt::format(
            "INSERT INTO schema_meta (key, value) "
            "SELECT 'version', '{}' "
            "WHERE NOT EXISTS (SELECT 1 FROM schema_meta WHERE key = 'version')",
            kSchemaVersion);
        conn_->Query(sql);

        // Symbol mappings table - use VARCHAR for dates (ISO format YYYY-MM-DD)
        // String comparison works correctly for ISO date format
        // last_accessed is a counter for LRU tracking (higher = more recent)
        conn_->Query(R"(
            CREATE TABLE IF NOT EXISTS symbol_mappings (
                instrument_id BIGINT NOT NULL,
                symbol VARCHAR NOT NULL,
                start_date VARCHAR NOT NULL,
                end_date VARCHAR NOT NULL,
                last_accessed BIGINT DEFAULT 0,
                PRIMARY KEY (instrument_id, start_date)
            )
        )");

        // Index for date-range lookups
        conn_->Query(R"(
            CREATE INDEX IF NOT EXISTS idx_mappings_date_range
            ON symbol_mappings (instrument_id, start_date, end_date)
        )");

        // Index for LRU eviction
        conn_->Query(R"(
            CREATE INDEX IF NOT EXISTS idx_mappings_lru
            ON symbol_mappings (last_accessed)
        )");

        // Initialize access counter
        conn_->Query(R"(
            CREATE SEQUENCE IF NOT EXISTS access_counter START 1
        )");

        // Download progress table
        conn_->Query(R"(
            CREATE TABLE IF NOT EXISTS download_progress (
                job_id VARCHAR NOT NULL,
                filename VARCHAR NOT NULL,
                sha256_expected VARCHAR NOT NULL,
                total_size BIGINT NOT NULL,
                completed_ranges VARCHAR NOT NULL,
                PRIMARY KEY (job_id, filename)
            )
        )");
    }

    void ValidateSchemaVersion() {
        auto result = conn_->Query("SELECT value FROM schema_meta WHERE key = 'version'");
        if (result->HasError()) {
            throw std::runtime_error("Failed to check schema version: " + result->GetError());
        }

        auto chunk = result->Fetch();
        if (!chunk || chunk->size() == 0) {
            throw std::runtime_error("Schema version not found in database");
        }

        int db_version = std::stoi(chunk->GetValue(0, 0).ToString());
        if (db_version != kSchemaVersion) {
            throw std::runtime_error(
                "Schema version mismatch: database has version " +
                std::to_string(db_version) + ", expected " +
                std::to_string(kSchemaVersion) +
                ". Delete the database file to recreate with current schema.");
        }
    }

    // Escape single quotes in strings for SQL
    static std::string EscapeString(const std::string& str) {
        std::string result;
        result.reserve(str.size());
        for (char c : str) {
            if (c == '\'') {
                result += "''";  // SQL escaping for single quotes
            } else {
                result += c;
            }
        }
        return result;
    }

    // Parse ranges from "[[start,end],[start,end],...]" format
    static void ParseRanges(const std::string& str,
                            std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
        ranges.clear();
        if (str.size() < 2 || str[0] != '[') return;

        size_t pos = 1;
        while (pos < str.size()) {
            if (str[pos] == '[') {
                // Parse [start,end]
                size_t comma = str.find(',', pos);
                size_t end_bracket = str.find(']', pos);
                if (comma != std::string::npos && end_bracket != std::string::npos) {
                    try {
                        uint64_t start = std::stoull(str.substr(pos + 1, comma - pos - 1));
                        uint64_t end = std::stoull(str.substr(comma + 1, end_bracket - comma - 1));
                        ranges.emplace_back(start, end);
                        pos = end_bracket + 1;
                    } catch (const std::exception&) {
                        ranges.clear();
                        return;
                    }
                } else {
                    break;
                }
            } else {
                ++pos;
            }
        }
    }

    // Evict LRU entries if cache exceeds limit
    void EvictIfNeeded() {
        if (max_mappings_ == 0) return;

        size_t count = MappingCount();
        if (count < max_mappings_) return;

        // Delete oldest entries to get back to 90% of limit
        size_t target = static_cast<size_t>(max_mappings_ * 0.9);
        size_t to_delete = count - target;

        auto sql = fmt::format(
            "DELETE FROM symbol_mappings "
            "WHERE (instrument_id, start_date) IN ("
            "  SELECT instrument_id, start_date FROM symbol_mappings "
            "  ORDER BY last_accessed ASC LIMIT {})",
            to_delete);

        conn_->Query(sql);
    }

    std::unique_ptr<duckdb::DuckDB> db_;
    std::unique_ptr<duckdb::Connection> conn_;
    size_t max_mappings_;
};

}  // namespace dbn_pipe
