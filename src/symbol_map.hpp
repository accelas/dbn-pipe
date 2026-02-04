// SPDX-License-Identifier: MIT

// src/symbol_map.hpp
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <databento/record.hpp>

namespace dbn_pipe {

/// Tracks instrument_id to symbol mappings from SymbolMappingMsg records.
///
/// Used for live streaming where records contain numeric instrument_id values
/// but callers need human-readable symbol strings.
///
/// @code
///   dbn_pipe::SymbolMap map;
///   map.OnRecord(record);
///   const std::string& sym = map.Find(record.Header().instrument_id);
/// @endcode
///
/// Thread safety: NOT thread-safe. Caller must synchronize if accessed
/// from multiple threads.
class SymbolMap {
public:
    /// Process a record, extracting the symbol mapping if it is a SymbolMappingMsg.
    ///
    /// Call this for every record received from the live stream.
    ///
    /// @param rec  The record to inspect. Only SymbolMappingMsg records update
    ///             the map; all others are silently ignored.
    void OnRecord(const databento::Record& rec) {
        if (const auto* msg = rec.GetIf<databento::SymbolMappingMsg>()) {
            uint32_t id = rec.Header().instrument_id;
            // Use STypeOutSymbol() - the resolved/output symbol - to match official PitSymbolMap.
            // STypeInSymbol() is the input/request symbol, STypeOutSymbol() is the resolved symbol.
            map_[id] = std::string(msg->STypeOutSymbol());
        }
    }

    /// Insert a mapping directly, bypassing record parsing.
    ///
    /// Useful for batch processing where records are accessed via memcpy
    /// rather than through the Record interface.
    ///
    /// @param instrument_id  Numeric instrument identifier.
    /// @param symbol         Human-readable symbol string.
    void Insert(uint32_t instrument_id, std::string symbol) {
        map_[instrument_id] = std::move(symbol);
    }

    /// Look up the symbol for a given instrument_id.
    ///
    /// @param instrument_id  Numeric instrument identifier to look up.
    /// @return Const reference to the symbol string, or an empty string if the
    ///         id has no mapping. The reference is invalidated by any call that
    ///         modifies the map.
    const std::string& Find(uint32_t instrument_id) const {
        static const std::string kEmpty;
        auto it = map_.find(instrument_id);
        return it != map_.end() ? it->second : kEmpty;
    }

    /// Remove all stored mappings.
    void Clear() {
        map_.clear();
    }

    /// Return the number of stored mappings.
    std::size_t Size() const {
        return map_.size();
    }

private:
    std::unordered_map<uint32_t, std::string> map_;
};

}  // namespace dbn_pipe
