// SPDX-License-Identifier: MIT

// src/instrument_map.hpp
#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <databento/record.hpp>

#include "dbn_pipe/storage.hpp"
#include "dbn_pipe/trading_date.hpp"

namespace dbn_pipe {

// Validate IANA timezone name. Throws std::runtime_error if invalid.
inline void ValidateTimezone(const std::string& timezone) {
    try {
        // Attempt to locate the timezone in the system tzdb
        std::chrono::locate_zone(timezone);
    } catch (const std::runtime_error&) {
        throw std::runtime_error(
            "Invalid timezone: '" + timezone + "'. "
            "Use IANA timezone names (e.g., 'America/New_York', 'UTC').");
    }
}

// Single mapping interval
struct MappingInterval {
    TradingDate start_date;
    TradingDate end_date;
    std::string symbol;
};

// InstrumentMap with date-interval tracking for symbol resolution.
// Critical for OPRA options where instrument_ids can be recycled
// across different trading days.
//
// Design: In-memory cache populated via OnSymbolMappingMsg(). When
// Resolve() returns nullopt, the caller should fetch missing data
// from the Databento API asynchronously, then retry after the map
// is populated. This enables non-blocking async resolution patterns.
//
// Storage behavior: Storage is queried only when an instrument_id is
// entirely absent from memory (cold-start). If an ID exists but the
// requested date falls outside cached intervals, storage is NOT queried
// and nullopt is returned. This is intentional - the caller handles
// missing data via async API fetch. See issue #50 for timeout handling.
//
// Thread safety: Not thread-safe. Use external synchronization if
// accessed from multiple threads.
//
// Overlapping intervals: Not supported. If intervals for the same
// instrument_id overlap, Resolve() behavior is undefined. Callers
// must ensure intervals are non-overlapping.
class InstrumentMap {
public:
    // Construct with optional storage and timezone for timestamp conversion.
    // Timezone uses IANA names: "America/New_York", "America/Chicago", "UTC", etc.
    // Throws std::runtime_error if timezone is invalid.
    explicit InstrumentMap(
        std::shared_ptr<IStorage> storage = nullptr,
        std::string timezone = "America/New_York")
        : storage_(storage ? storage : std::make_shared<NoOpStorage>())
        , timezone_(std::move(timezone)) {
        ValidateTimezone(timezone_);
    }

    // Insert mapping with date range
    void Insert(uint32_t instrument_id, const std::string& symbol,
                const TradingDate& start, const TradingDate& end) {
        assert(start <= end && "Insert: start date must be <= end date");
        auto& intervals = mappings_[instrument_id];
        intervals.push_back({start, end, symbol});

        // Keep sorted by start_date for binary search
        std::sort(intervals.begin(), intervals.end(),
                  [](const MappingInterval& a, const MappingInterval& b) {
                      return a.start_date < b.start_date;
                  });

        // Persist to storage
        storage_->StoreMapping(instrument_id, symbol, start, end);
    }

    // Resolve symbol for specific date - O(log n) binary search
    std::optional<std::string> Resolve(uint32_t instrument_id,
                                        const TradingDate& date) const {
        auto it = mappings_.find(instrument_id);
        if (it == mappings_.end()) {
            // Try storage fallback
            return storage_->LookupSymbol(instrument_id, date);
        }

        const auto& intervals = it->second;

        // Binary search: find first interval where start_date > date
        // Then check the preceding interval (if any) to see if it contains date
        auto interval_it = std::upper_bound(
            intervals.begin(), intervals.end(), date,
            [](const TradingDate& d, const MappingInterval& interval) {
                return d < interval.start_date;
            });

        if (interval_it == intervals.begin()) {
            return std::nullopt;
        }
        --interval_it;

        if (interval_it->start_date <= date && date < interval_it->end_date) {
            return interval_it->symbol;
        }

        return std::nullopt;
    }

    // Populate from DBN stream records
    void OnSymbolMappingMsg(const databento::SymbolMappingMsg& msg) {
        uint32_t id = msg.hd.instrument_id;
        std::string symbol(msg.STypeOutSymbol());

        // Convert timestamps to TradingDates using configured timezone
        auto start = TradingDate::FromNanoseconds(
            static_cast<uint64_t>(msg.start_ts.time_since_epoch().count()),
            timezone_);
        auto end = TradingDate::FromNanoseconds(
            static_cast<uint64_t>(msg.end_ts.time_since_epoch().count()),
            timezone_);

        Insert(id, symbol, start, end);
    }

    // Clear all mappings
    void Clear() noexcept {
        mappings_.clear();
    }

    // Get number of instrument_ids tracked
    std::size_t Size() const noexcept {
        return mappings_.size();
    }

    // Get configured timezone
    const std::string& Timezone() const noexcept { return timezone_; }

private:
    std::unordered_map<uint32_t, std::vector<MappingInterval>> mappings_;
    std::shared_ptr<IStorage> storage_;
    std::string timezone_;
};

}  // namespace dbn_pipe
