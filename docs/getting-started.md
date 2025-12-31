# Getting Started with dbn-pipe

This guide covers using dbn-pipe with the built-in `Reactor` event loop. For integration with existing event loops (libuv, asio), see [libuv-integration.md](libuv-integration.md).

## Overview

dbn-pipe provides async clients for Databento's Live and Historical APIs:

- **LiveClient** - Real-time market data streaming
- **HistoricalClient** - Historical data downloads

Both use a zero-copy pipeline architecture with backpressure support.

## Quick Start

### Minimal Live Streaming Example

```cpp
#include <iostream>
#include "src/client.hpp"

int main() {
    dbn_pipe::Reactor reactor;

    auto client = dbn_pipe::LiveClient::Create(reactor, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "trades"
    });

    client->OnRecord([](const dbn_pipe::DbnRecord& rec) {
        std::cout << "Record received\n";
    });

    client->OnError([](const dbn_pipe::Error& e) {
        std::cerr << "Error: " << e.message << "\n";
    });

    client->Connect();
    client->Start();

    reactor.Run();
}
```

### Minimal Historical Download Example

```cpp
#include <iostream>
#include "src/client.hpp"

int main() {
    dbn_pipe::Reactor reactor;

    auto client = dbn_pipe::HistoricalClient::Create(reactor, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "trades",
        .start = 1704067200000000000,  // 2024-01-01 00:00:00 UTC (nanoseconds)
        .end = 1704153600000000000     // 2024-01-02 00:00:00 UTC (nanoseconds)
    });

    client->OnRecord([](const dbn_pipe::DbnRecord& rec) {
        std::cout << "Record received\n";
    });

    client->OnComplete([]() {
        std::cout << "Download complete\n";
    });

    client->OnError([](const dbn_pipe::Error& e) {
        std::cerr << "Error: " << e.message << "\n";
    });

    client->Connect();
    client->Start();

    reactor.Run();
}
```

## Record Callbacks

### Per-Record Callback (Simple API)

Process records one at a time:

```cpp
client->OnRecord([](const dbn_pipe::DbnRecord& rec) {
    // Access as specific type
    const auto& trade = rec.As<databento::TradeMsg>();
    std::cout << "Price: " << trade.price << "\n";
});
```

### Batch Callback (Efficient Bulk Delivery)

Process records in batches for better performance:

```cpp
client->OnRecord([](dbn_pipe::RecordBatch&& batch) {
    std::cout << "Received " << batch.size() << " records\n";

    for (const auto& ref : batch) {
        // Common header access
        auto rtype = ref.Header().rtype;

        // Typed access based on rtype
        switch (rtype) {
            case databento::RType::Mbp0: {
                const auto& trade = ref.As<databento::TradeMsg>();
                // Process trade
                break;
            }
            case databento::RType::Mbp1: {
                const auto& mbp = ref.As<databento::Mbp1Msg>();
                // Process MBP-1
                break;
            }
            default:
                break;
        }
    }
});
```

**Note:** If batch callback is set, per-record callback is bypassed.

### RecordRef vs DbnRecord

| Type | Description | Use Case |
|------|-------------|----------|
| `DbnRecord` | Simple wrapper with `header` pointer | Per-record callback |
| `RecordRef` | Zero-copy reference with `data`, `size`, `keepalive` | Batch callback |

Both provide `As<T>()` for typed access. `RecordRef` also provides `Header()` for direct header access.

## Error Handling

### Error Callback

```cpp
client->OnError([](const dbn_pipe::Error& e) {
    std::cerr << "Error [" << dbn_pipe::error_category(e.code) << "]: "
              << e.message << "\n";

    // Optional: check OS errno for system errors
    if (e.os_errno != 0) {
        std::cerr << "OS error: " << strerror(e.os_errno) << "\n";
    }
});
```

### Error Categories

| Category | Error Codes | Description |
|----------|-------------|-------------|
| `connection` | `ConnectionFailed`, `ConnectionClosed`, `DnsResolutionFailed` | Network issues |
| `auth` | `AuthFailed`, `InvalidApiKey` | Authentication failures |
| `protocol` | `InvalidGreeting`, `InvalidChallenge`, `ParseError`, `BufferOverflow` | Protocol errors |
| `subscription` | `InvalidDataset`, `InvalidSymbol`, `InvalidSchema`, `InvalidTimeRange` | Request validation |
| `state` | `InvalidState` | Invalid operation for current state |
| `tls` | `TlsHandshakeFailed`, `CertificateError` | TLS errors (historical) |
| `http` | `HttpError` | HTTP errors (historical) |
| `decompression` | `DecompressionError` | Zstd errors (historical) |

## Backpressure

Suspend and resume data flow to handle slow consumers:

```cpp
auto client = dbn_pipe::LiveClient::Create(reactor, api_key);

// ... setup ...

client->OnRecord([&client](dbn_pipe::RecordBatch&& batch) {
    // Check if we're falling behind
    if (queue_depth > high_water_mark) {
        client->Suspend();  // Stop reading from network
    }

    // Process batch...
    process(std::move(batch));
});

// Later, when queue drains:
void on_queue_drained() {
    if (client->IsSuspended()) {
        client->Resume();  // Resume reading
    }
}
```

**Key points:**
- `Suspend()` / `Resume()` - Must be called from event loop thread
- `IsSuspended()` - Thread-safe, can be called from any thread
- Backpressure propagates through the pipeline to TCP socket

## Pipeline State

Check the current state of the client:

```cpp
auto state = client->GetState();

switch (state) {
    case dbn_pipe::PipelineState::Disconnected:
        // Initial state or after Stop()
        break;
    case dbn_pipe::PipelineState::Connecting:
        // TCP connect in progress
        break;
    case dbn_pipe::PipelineState::Connected:
        // Connected, ready for Start()
        break;
    case dbn_pipe::PipelineState::Streaming:
        // Actively streaming data
        break;
    case dbn_pipe::PipelineState::Stopping:
        // Stop() called, tearing down
        break;
    case dbn_pipe::PipelineState::Done:
        // Stream completed normally
        break;
    case dbn_pipe::PipelineState::Error:
        // Terminal error occurred
        break;
}
```

## Timers

Use `Timer` for periodic tasks or timeouts:

```cpp
dbn_pipe::Reactor reactor;
dbn_pipe::Timer stats_timer(reactor);

// Print stats every 5 seconds
stats_timer.OnTimer([&]() {
    std::cout << "Records received: " << count << "\n";
});
stats_timer.Start(5000, 5000);  // delay_ms, interval_ms

// One-shot timeout
dbn_pipe::Timer timeout(reactor);
timeout.OnTimer([&]() {
    std::cerr << "Connection timeout\n";
    client->Stop();
});
timeout.Start(30000);  // 30 second timeout (no repeat)

reactor.Run();
```

## Events (Low-Level)

For custom fd monitoring (advanced usage):

```cpp
dbn_pipe::Reactor reactor;

int my_fd = /* ... */;
dbn_pipe::Event event(reactor, my_fd, EPOLLIN);

event.OnEvent([](uint32_t events) {
    if (events & EPOLLIN) {
        // fd is readable
    }
    if (events & EPOLLOUT) {
        // fd is writable
    }
    if (events & (EPOLLERR | EPOLLHUP)) {
        // Error or hangup
    }
});

// Modify watched events
event.Modify(EPOLLIN | EPOLLOUT);

// Remove from reactor
event.Remove();
```

## DNS Resolution

Resolve hostnames manually (blocking):

```cpp
// Automatic resolution (recommended)
client->Connect();  // Resolves hostname from dataset

// Manual resolution
auto addr = dbn_pipe::ResolveHostname("glbx-mdp3.lsg.databento.com", 13000);
if (!addr) {
    std::cerr << "DNS resolution failed\n";
    return 1;
}
client->Connect(*addr);
```

## Multiple Clients

Run multiple clients on the same reactor:

```cpp
dbn_pipe::Reactor reactor;

// Live streaming for ES futures
auto es_client = dbn_pipe::LiveClient::Create(reactor, api_key);
es_client->SetRequest({.dataset = "GLBX.MDP3", .symbols = "ESZ4", .schema = "mbp-1"});
es_client->OnRecord([](dbn_pipe::RecordBatch&& batch) {
    // Handle ES data
});
es_client->Connect();
es_client->Start();

// Live streaming for NQ futures
auto nq_client = dbn_pipe::LiveClient::Create(reactor, api_key);
nq_client->SetRequest({.dataset = "GLBX.MDP3", .symbols = "NQZ4", .schema = "mbp-1"});
nq_client->OnRecord([](dbn_pipe::RecordBatch&& batch) {
    // Handle NQ data
});
nq_client->Connect();
nq_client->Start();

// Historical backfill
auto hist_client = dbn_pipe::HistoricalClient::Create(reactor, api_key);
hist_client->SetRequest({
    .dataset = "GLBX.MDP3",
    .symbols = "ESZ4",
    .schema = "trades",
    .start = start_ns,
    .end = end_ns
});
hist_client->OnRecord([](dbn_pipe::RecordBatch&& batch) {
    // Handle historical data
});
hist_client->OnComplete([]() {
    std::cout << "Backfill complete\n";
});
hist_client->Connect();
hist_client->Start();

// Single reactor drives all clients
reactor.Run();
```

## Deferred Execution

Schedule callbacks on the event loop thread:

```cpp
dbn_pipe::Reactor reactor;

// From any thread:
reactor.Defer([&]() {
    // This runs on the reactor thread
    client->Stop();
});
```

**Note:** `Defer()` is thread-safe. All other client methods must be called from the event loop thread.

## Complete Example

```cpp
#include <atomic>
#include <iostream>
#include <csignal>

#include <databento/record.hpp>

#include "src/client.hpp"
#include "src/reactor.hpp"

std::atomic<bool> g_shutdown{false};
dbn_pipe::Reactor* g_reactor = nullptr;

void signal_handler(int) {
    g_shutdown = true;
    if (g_reactor) {
        g_reactor->Defer([&]() { g_reactor->Stop(); });
    }
}

int main() {
    dbn_pipe::Reactor reactor;
    g_reactor = &reactor;

    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create client
    auto client = dbn_pipe::LiveClient::Create(reactor, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ES.FUT",
        .schema = "trades"
    });

    // Statistics
    std::atomic<uint64_t> record_count{0};
    std::atomic<uint64_t> batch_count{0};

    // Use batch callback for efficiency
    client->OnRecord([&](dbn_pipe::RecordBatch&& batch) {
        batch_count++;
        record_count += batch.size();

        for (const auto& ref : batch) {
            if (ref.Header().rtype == databento::RType::Mbp0) {
                const auto& trade = ref.As<databento::TradeMsg>();
                // Process trade...
            }
        }
    });

    client->OnError([](const dbn_pipe::Error& e) {
        std::cerr << "Error: " << e.message << "\n";
    });

    client->OnComplete([&]() {
        std::cout << "Stream completed\n";
        reactor.Stop();
    });

    // Stats timer
    dbn_pipe::Timer stats_timer(reactor);
    stats_timer.OnTimer([&]() {
        std::cout << "Batches: " << batch_count
                  << ", Records: " << record_count << "\n";
    });
    stats_timer.Start(1000, 1000);  // Every second

    // Connect and start
    client->Connect();
    client->Start();

    std::cout << "Streaming... (Ctrl+C to stop)\n";
    reactor.Run();

    std::cout << "Final: " << record_count << " records in "
              << batch_count << " batches\n";
    return 0;
}
```

## Thread Safety

| Method | Thread Safety |
|--------|--------------|
| `Reactor::Defer()` | Thread-safe |
| `Pipeline::IsSuspended()` | Thread-safe |
| All other methods | Event loop thread only |

**Rule:** All client operations except `Defer()` and `IsSuspended()` must be called from the event loop thread.

## API Endpoints

| Protocol | Gateway | Port |
|----------|---------|------|
| Live | `{dataset}.lsg.databento.com` | 13000 |
| Historical | `hist.databento.com` | 443 |

Dataset to hostname conversion: `GLBX.MDP3` → `glbx-mdp3.lsg.databento.com`

## Next Steps

- [libuv-integration.md](libuv-integration.md) - Integrate with existing libuv event loop
- [Databento API Documentation](https://docs.databento.com/) - API reference and schemas
