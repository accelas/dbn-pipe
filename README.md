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

```cpp
#include "src/client.hpp"

using namespace databento_async;

int main() {
    Reactor reactor;
    auto pipeline = Pipeline<LiveProtocol, DbnRecord>::Create(reactor, "your-api-key");

    pipeline->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "mbp-1"
    });

    pipeline->OnRecord([](const DbnRecord& rec) {
        // Process record
    });

    pipeline->OnError([](const Error& e) {
        std::cerr << "Error: " << e.message << std::endl;
    });

    pipeline->Connect();  // Resolves glbx-mdp3.lsg.databento.com:13000
    pipeline->Start();

    reactor.Run();
}
```

## Architecture

```
Live Pipeline:
  TcpSocket -> CramAuth -> DbnParser -> SinkAdapter -> Sink -> User Callback

Historical Pipeline:
  TcpSocket -> TlsTransport -> HttpClient -> ZstdDecompressor -> DbnParser -> SinkAdapter -> Sink
```

### Key Components

| Component | Description |
|-----------|-------------|
| `Reactor` | Epoll-based event loop |
| `TcpSocket<D>` | Templated TCP client, chain head |
| `Pipeline<P, R>` | Unified pipeline template |
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
