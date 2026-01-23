// SPDX-License-Identifier: MIT

// src/storage.hpp
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "trading_date.hpp"

namespace dbn_pipe {

// Forward declaration for Phase 5
struct DownloadProgress;

// Abstract storage interface for symbol mappings and download progress
class IStorage {
public:
    virtual ~IStorage() = default;

    // Symbol map operations
    virtual void StoreMapping(uint32_t instrument_id, const std::string& symbol,
                              const TradingDate& start, const TradingDate& end) = 0;
    virtual std::optional<std::string> LookupSymbol(uint32_t instrument_id,
                                                     const TradingDate& date) = 0;

    // Download progress operations (Phase 5 - stub for now)
    virtual void StoreProgress(const std::string& job_id, const std::string& filename,
                               const DownloadProgress& progress) = 0;
    virtual std::optional<DownloadProgress> LoadProgress(const std::string& job_id,
                                                          const std::string& filename) = 0;
    virtual void ClearProgress(const std::string& job_id, const std::string& filename) = 0;
    virtual std::vector<std::pair<std::string, std::string>> ListIncompleteDownloads() = 0;
};

// Stub for Phase 5
struct DownloadProgress {
    std::string sha256_expected;
    uint64_t total_size = 0;
    std::vector<std::pair<uint64_t, uint64_t>> completed_ranges;
};

// No-op implementation - default when no persistence needed
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
