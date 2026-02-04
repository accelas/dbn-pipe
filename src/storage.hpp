// SPDX-License-Identifier: MIT

// src/storage.hpp
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dbn_pipe/trading_date.hpp"

namespace dbn_pipe {

/// Forward declaration for download progress tracking.
struct DownloadProgress;

/// Abstract storage interface for symbol mappings and download progress.
///
/// Implementations persist instrument-id-to-symbol mappings and track
/// resumable download state. See DuckDbStorage for a concrete backend.
class IStorage {
public:
    virtual ~IStorage() = default;

    /// Store a symbol mapping for the given instrument id over a date range.
    /// @param instrument_id  Databento numeric instrument identifier.
    /// @param symbol         Human-readable ticker symbol.
    /// @param start          Start date (inclusive) of the mapping interval.
    /// @param end            End date (exclusive) of the mapping interval.
    virtual void StoreMapping(uint32_t instrument_id, const std::string& symbol,
                              const TradingDate& start, const TradingDate& end) = 0;

    /// Look up the symbol for an instrument id on a specific trading date.
    /// @param instrument_id  Databento numeric instrument identifier.
    /// @param date           Trading date to resolve.
    /// @return The symbol if a mapping exists, or std::nullopt.
    virtual std::optional<std::string> LookupSymbol(uint32_t instrument_id,
                                                     const TradingDate& date) = 0;

    /// Persist download progress for a resumable download job.
    /// @param job_id    Unique identifier for the download job.
    /// @param filename  Name of the file being downloaded.
    /// @param progress  Current progress state to persist.
    virtual void StoreProgress(const std::string& job_id, const std::string& filename,
                               const DownloadProgress& progress) = 0;

    /// Load previously persisted download progress.
    /// @param job_id    Unique identifier for the download job.
    /// @param filename  Name of the file being downloaded.
    /// @return The saved progress, or std::nullopt if none exists.
    virtual std::optional<DownloadProgress> LoadProgress(const std::string& job_id,
                                                          const std::string& filename) = 0;

    /// Remove download progress for a completed or cancelled job.
    /// @param job_id    Unique identifier for the download job.
    /// @param filename  Name of the file whose progress should be cleared.
    virtual void ClearProgress(const std::string& job_id, const std::string& filename) = 0;

    /// List all incomplete downloads that may be resumed.
    /// @return Vector of (job_id, filename) pairs with outstanding progress.
    virtual std::vector<std::pair<std::string, std::string>> ListIncompleteDownloads() = 0;
};

/// State of a resumable download, used to persist and resume partial transfers.
struct DownloadProgress {
    std::string sha256_expected;                           ///< Expected SHA-256 hash of the complete file.
    uint64_t total_size = 0;                               ///< Total file size in bytes.
    std::vector<std::pair<uint64_t, uint64_t>> completed_ranges;  ///< Byte ranges already downloaded (start, end).
};

/// No-op storage implementation used when persistence is not needed.
///
/// All writes are silently discarded; all reads return empty results.
class NoOpStorage : public IStorage {
public:
    void StoreMapping(uint32_t, const std::string&,
                      const TradingDate&, const TradingDate&) override {}

    std::optional<std::string> LookupSymbol(uint32_t, const TradingDate&) override {
        return std::nullopt;
    }

    void StoreProgress(const std::string&, const std::string&,
                       const DownloadProgress&) override {}

    std::optional<DownloadProgress> LoadProgress(const std::string&,
                                                  const std::string&) override {
        return std::nullopt;
    }

    void ClearProgress(const std::string&, const std::string&) override {}

    std::vector<std::pair<std::string, std::string>> ListIncompleteDownloads() override {
        return {};
    }
};

}  // namespace dbn_pipe
