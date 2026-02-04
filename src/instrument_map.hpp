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

/// Validate an IANA timezone name against the system timezone database.
/// @param timezone  IANA timezone name (e.g. "America/New_York", "UTC").
/// @throws std::runtime_error if the timezone is not recognized.
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

/// A single symbol-mapping interval for an instrument id.
struct MappingInterval {
    TradingDate start_date;  ///< Start date (inclusive) of the mapping.
    TradingDate end_date;    ///< End date (exclusive) of the mapping.
    std::string symbol;      ///< Human-readable ticker symbol.
};

/// Date-interval instrument map for resolving instrument ids to symbols.
///
/// Critical for OPRA options where instrument_ids can be recycled across
/// different trading days.
///
/// **Design:** In-memory cache populated via OnSymbolMappingMsg().  When
/// Resolve() returns nullopt, the caller should fetch missing data from
/// the Databento API asynchronously, then retry after the map is populated.
///
/// **Storage behavior:** Storage is queried only when an instrument_id is
/// entirely absent from memory (cold-start).  If an ID exists but the
/// requested date falls outside cached intervals, storage is NOT queried
/// and nullopt is returned -- the caller handles missing data via async
/// API fetch.
///
/// **Thread safety:** Not thread-safe.  Use external synchronization if
/// accessed from multiple threads.
///
/// **Overlapping intervals:** Not supported.  If intervals for the same
/// instrument_id overlap, Resolve() behavior is undefined.
class InstrumentMap {
public:
    /// Construct with optional persistent storage and timezone.
    ///
    /// @param storage   Backing store for symbol mappings (nullptr for no persistence).
    /// @param timezone  IANA timezone name (e.g. "America/New_York").
    /// @throws std::runtime_error if @p timezone is invalid.
    explicit InstrumentMap(
        std::shared_ptr<IStorage> storage = nullptr,
        std::string timezone = "America/New_York")
        : storage_(storage ? storage : std::make_shared<NoOpStorage>())
        , timezone_(std::move(timezone)) {
        ValidateTimezone(timezone_);
    }

    /// Insert a symbol mapping for the given instrument id over a date range.
    /// @param instrument_id  Databento numeric instrument identifier.
    /// @param symbol         Human-readable ticker symbol.
    /// @param start          Start date (inclusive).
    /// @param end            End date (exclusive).
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

    /// Resolve a symbol for a specific trading date using O(log n) binary search.
    /// @param instrument_id  Databento numeric instrument identifier.
    /// @param date           Trading date to resolve.
    /// @return The symbol if a mapping exists, or std::nullopt.
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

    /// Populate the map from a Databento SymbolMappingMsg record.
    ///
    /// Converts the message timestamps to TradingDates using the configured
    /// timezone, then inserts the mapping.
    /// @param msg  A SymbolMappingMsg from the DBN stream.
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

    /// Remove all cached mappings from memory.
    void Clear() noexcept {
        mappings_.clear();
    }

    /// @return Number of distinct instrument ids tracked.
    std::size_t Size() const noexcept {
        return mappings_.size();
    }

    /// @return The IANA timezone name used for timestamp conversion.
    const std::string& Timezone() const noexcept { return timezone_; }

private:
    std::unordered_map<uint32_t, std::vector<MappingInterval>> mappings_;
    std::shared_ptr<IStorage> storage_;
    std::string timezone_;
};

}  // namespace dbn_pipe
