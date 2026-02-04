// SPDX-License-Identifier: MIT

// src/client.hpp
#pragma once

#include "dbn_pipe/streaming_client.hpp"
#include "dbn_pipe/live_protocol.hpp"
#include "dbn_pipe/historical_protocol.hpp"
#include "dbn_pipe/retry_policy.hpp"

namespace dbn_pipe {

// Type aliases for convenience.
// StreamingClient provides zero-copy access to records with lifetime management.
using LiveClient = StreamingClient<LiveProtocol>;
using HistoricalClient = StreamingClient<HistoricalProtocol>;

}  // namespace dbn_pipe
