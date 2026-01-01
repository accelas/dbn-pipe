// src/client.hpp
#pragma once

#include "pipeline.hpp"
#include "live_protocol.hpp"
#include "historical_protocol.hpp"
#include "lib/stream/event_loop.hpp"
#include "record_batch.hpp"

namespace dbn_pipe {

// Type aliases for convenience.
// RecordRef provides zero-copy access to records with lifetime management.
using LiveClient = Pipeline<LiveProtocol, RecordRef>;
using HistoricalClient = Pipeline<HistoricalProtocol, RecordRef>;

}  // namespace dbn_pipe
