// src/client.hpp
#pragma once

#include <databento/record.hpp>

#include "pipeline.hpp"
#include "live_protocol.hpp"
#include "historical_protocol.hpp"
#include "epoll_event_loop.hpp"

namespace databento_async {

// Default record type for per-record callbacks.
// Wraps a pointer to raw record data for zero-copy access.
// The record is only valid during the callback - do not store.
struct DbnRecord {
    const databento::RecordHeader* header = nullptr;

    DbnRecord() = default;
    explicit DbnRecord(const databento::RecordHeader* h) : header(h) {}

    // Access the record as a specific type
    template <typename T>
    const T& As() const {
        return *reinterpret_cast<const T*>(header);
    }
};

// Type aliases for backward compatibility
using LiveClient = Pipeline<LiveProtocol, DbnRecord>;
using HistoricalClient = Pipeline<HistoricalProtocol, DbnRecord>;

// Backward compatibility - Reactor is now EpollEventLoop
using Reactor = EpollEventLoop;

}  // namespace databento_async
