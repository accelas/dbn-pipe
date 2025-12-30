// src/client.hpp
#pragma once

#include "unified_pipeline.hpp"
#include "live_protocol.hpp"
#include "historical_protocol.hpp"

namespace databento_async {

// Default record type - placeholder for backward compatibility aliases.
// In production code, users would specify a concrete record type
// (e.g., databento::TradeMsg, databento::MboMsg) when using Pipeline directly.
struct DbnRecord {
    // Placeholder - DBN records are polymorphic via RecordHeader.
    // The actual record type is determined by rtype in the header.
};

// Type aliases for backward compatibility
using LiveClient = Pipeline<LiveProtocol, DbnRecord>;
using HistoricalClient = Pipeline<HistoricalProtocol, DbnRecord>;

}  // namespace databento_async
