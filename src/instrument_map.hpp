// src/instrument_map.hpp
#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage.hpp"
#include "trading_date.hpp"

namespace dbn_pipe {

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
// Thread safety: Not thread-safe. Use external synchronization if
// accessed from multiple threads.
//
// Overlapping intervals: Not supported. If intervals for the same
// instrument_id overlap, Resolve() behavior is undefined. Callers
// must ensure intervals are non-overlapping.
class InstrumentMap {
public:
    explicit InstrumentMap(std::shared_ptr<IStorage> storage = nullptr)
        : storage_(storage ? storage : std::make_shared<NoOpStorage>()) {}

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

        // Binary search for interval containing date
        auto interval_it = std::lower_bound(
            intervals.begin(), intervals.end(), date,
            [](const MappingInterval& interval, const TradingDate& d) {
                return interval.end_date < d;
            });

        if (interval_it != intervals.end() &&
            interval_it->start_date <= date && date <= interval_it->end_date) {
            return interval_it->symbol;
        }

        return std::nullopt;
    }

    // Clear all mappings
    void Clear() noexcept {
        mappings_.clear();
    }

    // Get number of instrument_ids tracked
    std::size_t Size() const noexcept {
        return mappings_.size();
    }

private:
    std::unordered_map<uint32_t, std::vector<MappingInterval>> mappings_;
    std::shared_ptr<IStorage> storage_;
};

}  // namespace dbn_pipe
