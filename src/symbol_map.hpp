// src/symbol_map.hpp
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <databento/record.hpp>

namespace databento_async {

// SymbolMap tracks instrument_id -> symbol mappings from SymbolMappingMsg records.
// Used for live streaming where records contain instrument_id but callers need symbol.
//
// Thread safety: NOT thread-safe. Caller must synchronize if accessed from multiple threads.
class SymbolMap {
public:
    // Process a record, extracting symbol mapping if it's a SymbolMappingMsg.
    // Call this for every record received from the live stream.
    void OnRecord(const databento::Record& rec) {
        if (const auto* msg = rec.GetIf<databento::SymbolMappingMsg>()) {
            uint32_t id = rec.Header().instrument_id;
            // Use STypeOutSymbol() - the resolved/output symbol - to match official PitSymbolMap.
            // STypeInSymbol() is the input/request symbol, STypeOutSymbol() is the resolved symbol.
            map_[id] = std::string(msg->STypeOutSymbol());
        }
    }

    // Direct insert for batch processing where records are accessed via memcpy
    void Insert(uint32_t instrument_id, std::string symbol) {
        map_[instrument_id] = std::move(symbol);
    }

    // Look up symbol for instrument_id. Returns empty string if not found.
    // Returns const ref to avoid copy; caller should not store the reference
    // across calls that modify the map.
    const std::string& Find(uint32_t instrument_id) const {
        static const std::string kEmpty;
        auto it = map_.find(instrument_id);
        return it != map_.end() ? it->second : kEmpty;
    }

    // Clear all mappings
    void Clear() {
        map_.clear();
    }

    // Get number of mappings
    std::size_t Size() const {
        return map_.size();
    }

private:
    std::unordered_map<uint32_t, std::string> map_;
};

}  // namespace databento_async
