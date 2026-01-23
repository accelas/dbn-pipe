// SPDX-License-Identifier: MIT

// src/client.hpp
#pragma once

#include "streaming_client.hpp"
#include "live_protocol.hpp"
#include "historical_protocol.hpp"

namespace dbn_pipe {

// Type aliases for convenience.
// StreamingClient provides zero-copy access to records with lifetime management.
using LiveClient = StreamingClient<LiveProtocol>;
using HistoricalClient = StreamingClient<HistoricalProtocol>;

}  // namespace dbn_pipe
