# databento-async

Async Databento client using epoll with zero-copy pipeline architecture.

## Features

- Epoll-based async I/O with custom Reactor
- Zero-copy DBN record parsing with BufferChain
- Template-based static dispatch (no virtual calls in hot path)
- Backpressure via Suspend/Resume propagation
- Support for Live and Historical APIs

## Quick Start

### Prerequisites

- [Bazelisk](https://github.com/bazelbuild/bazelisk) (recommended) or Bazel 7.4.1+
- GCC 14+ or Clang 19+
- OpenSSL (for TLS)
- zstd (for decompression)

### Build

```bash
# Build all targets
bazel build //...

# Run tests
bazel test //tests/...
```

### Usage

#### Live Streaming

```cpp
#include "src/client.hpp"

using namespace databento_async;

int main() {
    Reactor reactor;
    auto client = LiveClient::Create(reactor, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "mbp-1"
    });

    client->OnRecord([](const DbnRecord& rec) {
        // Process record
    });

    client->OnError([](const Error& e) {
        std::cerr << "Error: " << e.message << std::endl;
    });

    client->Connect();  // Resolves glbx-mdp3.lsg.databento.com:13000
    client->Start();

    reactor.Run();
}
```

#### Historical Download

```cpp
#include "src/client.hpp"

using namespace databento_async;

int main() {
    Reactor reactor;
    auto client = HistoricalClient::Create(reactor, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "mbp-1",
        .start = 1704067200000000000,  // 2024-01-01 00:00:00 UTC (nanoseconds)
        .end = 1704153600000000000     // 2024-01-02 00:00:00 UTC (nanoseconds)
    });

    client->OnRecord([](const DbnRecord& rec) {
        // Process record
    });

    client->OnComplete([]() {
        std::cout << "Download complete" << std::endl;
    });

    client->Connect();  // Connects to hist.databento.com:443
    client->Start();

    reactor.Run();
}
```

## Architecture

```
Live Pipeline:
  TcpSocket -> CramAuth -> DbnParser -> Sink -> User Callback

Historical Pipeline:
  TcpSocket -> TlsTransport -> HttpClient -> ZstdDecompressor -> DbnParser -> Sink
```

### Key Components

| Component | Description |
|-----------|-------------|
| `Reactor` | Epoll-based event loop |
| `LiveClient` | Alias for `Pipeline<LiveProtocol, DbnRecord>` |
| `HistoricalClient` | Alias for `Pipeline<HistoricalProtocol, DbnRecord>` |
| `BufferChain` | Zero-copy buffer management with segment pooling |
| `DbnParserComponent` | Zero-copy DBN record parser |

### Data Flow

```
Network -> TcpSocket -> [Protocol Chain] -> DbnParser -> RecordBatch -> User
                                                              |
Backpressure <-- Suspend/Resume <-- Suspend/Resume <----------+
```

## API Endpoints

| Protocol | Gateway | Port |
|----------|---------|------|
| Live | `{dataset}.lsg.databento.com` | 13000 |
| Historical | `hist.databento.com` | 443 |

## License

MIT
