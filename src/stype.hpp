// SPDX-License-Identifier: MIT

// src/stype.hpp
#pragma once

#include <optional>
#include <string_view>

namespace dbn_pipe {

// Symbology type enum - specifies how symbols are interpreted.
// Matches Databento's SType values for API compatibility.
enum class SType {
    RawSymbol,      // Raw symbol string (e.g., "AAPL", "ES.FUT")
    InstrumentId,   // Numeric instrument ID
    Parent,         // Parent symbol (e.g., "SPY.OPT" for all SPY options)
    Continuous,     // Continuous contract (e.g., "ES.c.0" for front month)
    Smart           // Smart symbology (auto-resolves based on context)
};

inline std::optional<SType> STypeFromString(std::string_view s) {
    if (s == "raw_symbol") return SType::RawSymbol;
    if (s == "instrument_id") return SType::InstrumentId;
    if (s == "parent") return SType::Parent;
    if (s == "continuous") return SType::Continuous;
    if (s == "smart") return SType::Smart;
    return std::nullopt;
}

inline std::string_view STypeToString(SType stype) {
    switch (stype) {
        case SType::RawSymbol: return "raw_symbol";
        case SType::InstrumentId: return "instrument_id";
        case SType::Parent: return "parent";
        case SType::Continuous: return "continuous";
        case SType::Smart: return "smart";
    }
    return "";
}

}  // namespace dbn_pipe
