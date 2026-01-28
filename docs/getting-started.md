# Getting Started

Use dbn-pipe with the built-in `EpollEventLoop`. For libuv or asio integration, see [libuv-integration.md](libuv-integration.md).

## Overview

dbn-pipe streams market data from Databento:

- **LiveClient** — real-time streaming
- **HistoricalClient** — historical downloads

Both use zero-copy pipelines with backpressure.

## Quick Start

### Live Streaming

```cpp
#include <iostream>
#include "src/client.hpp"

int main() {
    dbn_pipe::EpollEventLoop reactor;
    auto client = dbn_pipe::LiveClient::Create(reactor, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "trades"
    });

    client->OnRecord([](const dbn_pipe::RecordRef& ref) {
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

### Historical Download

```cpp
#include <iostream>
#include "src/client.hpp"

int main() {
    dbn_pipe::EpollEventLoop reactor;
    auto client = dbn_pipe::HistoricalClient::Create(reactor, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "trades",
        .start = 1704067200000000000,  // 2024-01-01 00:00:00 UTC (ns)
        .end = 1704153600000000000     // 2024-01-02 00:00:00 UTC (ns)
    });

    client->OnRecord([](const dbn_pipe::RecordRef& ref) {
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

### Per-Record

```cpp
client->OnRecord([](const dbn_pipe::RecordRef& ref) {
    const auto& trade = ref.As<databento::TradeMsg>();
    std::cout << "Price: " << trade.price << "\n";
});
```

### Batch (Faster)

Batches deliver multiple records per callback:

```cpp
client->OnRecord([](dbn_pipe::RecordBatch&& batch) {
    std::cout << "Received " << batch.size() << " records\n";

    for (const auto& ref : batch) {
        auto rtype = ref.Header().rtype;

        switch (rtype) {
            case databento::RType::Mbp0: {
                const auto& trade = ref.As<databento::TradeMsg>();
                break;
            }
            case databento::RType::Mbp1: {
                const auto& mbp = ref.As<databento::Mbp1Msg>();
                break;
            }
            default:
                break;
        }
    }
});
```

Setting a batch callback bypasses the per-record callback.

### RecordRef

`RecordRef` provides zero-copy access to records:

| Field | Description |
|-------|-------------|
| `data` | Pointer to raw record bytes |
| `size` | Record size in bytes |
| `keepalive` | Shared pointer ensuring buffer validity |

Methods:
- `Header()` — access `RecordHeader`
- `As<T>()` — typed access (e.g., `As<databento::TradeMsg>()`)

## Error Handling

```cpp
client->OnError([](const dbn_pipe::Error& e) {
    std::cerr << "Error [" << dbn_pipe::error_category(e.code) << "]: "
              << e.message << "\n";

    if (e.os_errno != 0) {
        std::cerr << "OS error: " << strerror(e.os_errno) << "\n";
    }
});
```

### Error Categories

| Category | Codes | Scope |
|----------|-------|-------|
| `connection` | `ConnectionFailed`, `ConnectionClosed`, `DnsResolutionFailed` | Both |
| `auth` | `AuthFailed`, `InvalidApiKey` | Live |
| `protocol` | `InvalidGreeting`, `InvalidChallenge`, `ParseError`, `BufferOverflow` | Mixed |
| `subscription` | `InvalidDataset`, `InvalidSymbol`, `InvalidSchema`, `InvalidTimeRange` | Mixed |
| `state` | `InvalidState` | Both |
| `tls` | `TlsHandshakeFailed`, `CertificateError` | Historical |
| `http` | `HttpError` | Historical |
| `decompression` | `DecompressionError` | Historical |

**Scope key:**
- **Live** — CRAM authentication (live gateway only)
- **Historical** — TLS, HTTP, zstd decompression
- **Both** — TCP, DNS, DBN parsing, state machine
- **Mixed** — `InvalidGreeting`/`InvalidChallenge` are Live; `ParseError`/`BufferOverflow` are Both; `InvalidTimeRange` is Historical

## Backpressure

Suspend reading when falling behind; resume when ready:

```cpp
client->OnRecord([&client](dbn_pipe::RecordBatch&& batch) {
    if (queue_depth > high_water_mark) {
        client->Suspend();
    }
    process(std::move(batch));
});

void on_queue_drained() {
    if (client->IsSuspended()) {
        client->Resume();
    }
}
```

- `Suspend()` / `Resume()` — call from event loop thread only
- `IsSuspended()` — thread-safe
- Backpressure propagates to the TCP socket

## Pipeline State

The pipeline uses a state machine to enforce correct sequencing:

```cpp
auto state = client->GetState();

switch (state) {
    case dbn_pipe::Pipeline::State::Created:     // Initial state
    case dbn_pipe::Pipeline::State::Connecting:  // Connect() called, awaiting handshake
    case dbn_pipe::Pipeline::State::Ready:       // Handshake complete, can call Start()
    case dbn_pipe::Pipeline::State::Started:     // Streaming active
    case dbn_pipe::Pipeline::State::TornDown:    // Stopped or error
        break;
}

// Check if ready to start
if (client->IsReady()) {
    client->Start();
}
```

State transitions:
```
Created -> Connecting -> Ready -> Started -> TornDown
              |            |         |
              +------------+---------+-> TornDown (on error or Stop)
```

`Start()` can only be called in the `Ready` state—typically from the ready callback after the TLS handshake completes.

## Timers

```cpp
dbn_pipe::EpollEventLoop reactor;
dbn_pipe::Timer stats_timer(reactor);

// Periodic: print stats every 5 seconds
stats_timer.OnTimer([&]() {
    std::cout << "Records: " << count << "\n";
});
stats_timer.Start(5000, 5000);  // delay_ms, interval_ms

// One-shot: 30-second timeout
dbn_pipe::Timer timeout(reactor);
timeout.OnTimer([&]() {
    std::cerr << "Timeout\n";
    client->Stop();
});
timeout.Start(30000);

reactor.Run();
```

## Events (Low-Level)

Monitor custom file descriptors:

```cpp
dbn_pipe::EpollEventLoop reactor;
int my_fd = /* ... */;
dbn_pipe::Event event(reactor, my_fd, EPOLLIN);

event.OnEvent([](uint32_t events) {
    if (events & EPOLLIN)  { /* readable */ }
    if (events & EPOLLOUT) { /* writable */ }
    if (events & (EPOLLERR | EPOLLHUP)) { /* error */ }
});

event.Modify(EPOLLIN | EPOLLOUT);  // Change watched events
event.Remove();                    // Unregister
```

## DNS Resolution

```cpp
// Automatic (recommended)
client->Connect();  // Resolves from dataset

// Manual
auto addr = dbn_pipe::ResolveHostname("glbx-mdp3.lsg.databento.com", 13000);
if (!addr) {
    std::cerr << "DNS failed\n";
    return 1;
}
client->Connect(*addr);
```

## Multiple Clients

One event loop drives many clients:

```cpp
dbn_pipe::EpollEventLoop reactor;

auto es = dbn_pipe::LiveClient::Create(reactor, api_key);
es->SetRequest({.dataset = "GLBX.MDP3", .symbols = "ESZ4", .schema = "mbp-1"});
es->OnRecord([](dbn_pipe::RecordBatch&& b) { /* ES data */ });
es->Connect();
es->Start();

auto nq = dbn_pipe::LiveClient::Create(reactor, api_key);
nq->SetRequest({.dataset = "GLBX.MDP3", .symbols = "NQZ4", .schema = "mbp-1"});
nq->OnRecord([](dbn_pipe::RecordBatch&& b) { /* NQ data */ });
nq->Connect();
nq->Start();

auto hist = dbn_pipe::HistoricalClient::Create(reactor, api_key);
hist->SetRequest({.dataset = "GLBX.MDP3", .symbols = "ESZ4", .schema = "trades",
                  .start = start_ns, .end = end_ns});
hist->OnRecord([](dbn_pipe::RecordBatch&& b) { /* historical */ });
hist->OnComplete([]() { std::cout << "Backfill done\n"; });
hist->Connect();
hist->Start();

reactor.Run();
```

## Deferred Execution

`Defer()` schedules a callback on the event loop thread—the only thread-safe event loop method.

### Signal Handler

Signal handlers run in interrupt context; call `Defer()` to reach the event loop thread:

```cpp
dbn_pipe::EpollEventLoop* g_reactor = nullptr;

void signal_handler(int) {
    if (g_reactor) {
        g_reactor->Defer([]() { g_reactor->Stop(); });
    }
}
```

### Worker Thread

Background threads (e.g., database writers) use `Defer()` to signal the client:

```cpp
void DbWriter::OnQueueDrained() {
    reactor_.Defer([this]() {
        if (client_->IsSuspended()) {
            client_->Resume();
        }
    });
}
```

### Avoiding Reentrancy

Defer teardown to run after the current callback completes:

```cpp
client->OnError([&](const dbn_pipe::Error& e) {
    reactor.Defer([&]() {
        client->Stop();
        reconnect();
    });
});
```

## Complete Example

```cpp
#include <atomic>
#include <iostream>
#include <csignal>
#include <databento/record.hpp>
#include "src/client.hpp"
#include "src/stream.hpp"

std::atomic<bool> g_shutdown{false};
dbn_pipe::EpollEventLoop* g_reactor = nullptr;

void signal_handler(int) {
    g_shutdown = true;
    if (g_reactor) {
        g_reactor->Defer([&]() { g_reactor->Stop(); });
    }
}

int main() {
    dbn_pipe::EpollEventLoop reactor;
    g_reactor = &reactor;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    auto client = dbn_pipe::LiveClient::Create(reactor, "your-api-key");
    client->SetRequest({.dataset = "GLBX.MDP3", .symbols = "ES.FUT", .schema = "trades"});

    std::atomic<uint64_t> records{0}, batches{0};

    client->OnRecord([&](dbn_pipe::RecordBatch&& batch) {
        batches++;
        records += batch.size();
        for (const auto& ref : batch) {
            if (ref.Header().rtype == databento::RType::Mbp0) {
                const auto& trade = ref.As<databento::TradeMsg>();
                // process trade
            }
        }
    });

    client->OnError([](const dbn_pipe::Error& e) {
        std::cerr << "Error: " << e.message << "\n";
    });

    client->OnComplete([&]() {
        std::cout << "Done\n";
        reactor.Stop();
    });

    dbn_pipe::Timer stats(reactor);
    stats.OnTimer([&]() {
        std::cout << batches << " batches, " << records << " records\n";
    });
    stats.Start(1000, 1000);

    client->Connect();
    client->Start();

    std::cout << "Streaming (Ctrl+C to stop)...\n";
    reactor.Run();

    std::cout << "Final: " << records << " records in " << batches << " batches\n";
}
```

## Thread Safety

| Method | Thread-safe |
|--------|-------------|
| `EpollEventLoop::Defer()` | Yes |
| `Pipeline::IsSuspended()` | Yes |
| All others | No—event loop thread only |

## API Endpoints

| Protocol | Gateway | Port |
|----------|---------|------|
| Live | `{dataset}.lsg.databento.com` | 13000 |
| Historical | `hist.databento.com` | 443 |

`GLBX.MDP3` → `glbx-mdp3.lsg.databento.com`

## Next Steps

- [api-guide.md](api-guide.md) — symbol resolution, storage, table definitions, retry policy
- [libuv-integration.md](libuv-integration.md) — libuv adapter
- [REST API example](../example/rest_api/README.md) — coroutine-based REST API pipeline
- [dbWriter design](../example/dbwriter/DESIGN.md) — PostgreSQL COPY sink
- [Databento Docs](https://docs.databento.com/) — API reference
