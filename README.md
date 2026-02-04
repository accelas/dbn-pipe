# dbn-pipe

Async Databento client with zero-copy pipeline architecture and pluggable event loop.

## Features

- Pluggable event loop via `IEventLoop` interface (integrate with libuv, asio, etc.)
- Built-in epoll-based `EventLoop` for standalone usage
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
#include "dbn_pipe/client.hpp"

using namespace dbn_pipe;

int main() {
    EpollEventLoop reactor;
    auto client = LiveClient::Create(reactor, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "mbp-1"
    });

    client->OnRecord([](const RecordRef& ref) {
        // ref.Header() for record header
        // ref.As<databento::TradeMsg>() for typed access
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
#include "dbn_pipe/client.hpp"
#include "dbn_pipe/to_nanos.hpp"

using namespace dbn_pipe;
using namespace std::chrono;

int main() {
    EpollEventLoop reactor;
    auto client = HistoricalClient::Create(reactor, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "mbp-1",
        .start = to_nanos(2024y / January / 1),  // NY midnight (default)
        .end = to_nanos(2024y / January / 2)
    });

    client->OnRecord([](const RecordRef& ref) {
        // ref.Header() for record header
        // ref.As<databento::TradeMsg>() for typed access
    });

    client->OnComplete([]() {
        std::cout << "Download complete" << std::endl;
    });

    client->Connect();  // Connects to hist.databento.com:443
    client->Start();

    reactor.Run();
}
```

#### Integration with Existing Event Loop (libuv)

For applications with an existing event loop, implement the `IEventLoop` interface:

```cpp
#include <uv.h>
#include "dbn_pipe/client.hpp"
#include "libuv_event_loop.hpp"  // Your adapter implementation

int main() {
    uv_loop_t* loop = uv_default_loop();
    LibuvEventLoop adapter(loop);  // Wrap existing loop (non-owning)
    adapter.SetEventLoopThread();  // Record thread ID for assertions

    auto client = dbn_pipe::LiveClient::Create(adapter, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "mbp-1"
    });

    // Option 1: Per-record callback
    client->OnRecord([](const dbn_pipe::RecordRef& ref) {
        // ref.Header() for record header
        // ref.As<databento::Mbp1Msg>() for typed access
    });

    // Option 2: Batch callback (efficient bulk delivery)
    client->OnRecord([](dbn_pipe::RecordBatch&& batch) {
        for (const auto& ref : batch) {
            // ref.Header() for record header
            // ref.As<databento::Mbp1Msg>() for typed access
        }
    });

    client->OnError([](const dbn_pipe::Error& e) {
        std::cerr << "Error: " << e.message << std::endl;
    });

    client->Connect();
    client->Start();

    uv_run(loop, UV_RUN_DEFAULT);  // Your loop drives everything
}
```

The `IEventLoop` interface requires:
- `Register()` - Register fd for read/write events using `uv_poll_t`
- `Defer()` - Schedule callback on event loop thread using `uv_async_t`
- `IsInEventLoopThread()` - Thread safety check

See [docs/libuv-integration.md](docs/libuv-integration.md) for complete adapter implementation.

## Documentation

- **[Getting Started](docs/getting-started.md)** - Complete guide with all features
- **[API Guide](docs/api-guide.md)** - Symbol resolution, storage, table definitions, retry policy
- **[libuv Integration](docs/libuv-integration.md)** - Integrate with existing libuv event loop
- **[REST API Example](example/rest_api/README.md)** - Coroutine-based REST API pipeline
- **[dbWriter Design](example/dbwriter/DESIGN.md)** - PostgreSQL COPY sink with object mapping

## Architecture

The library uses a unified `Pipeline<Protocol>` template with protocol-specific component chains:

```
Live Pipeline:
  TcpSocket -> CramAuth -> DbnParser -> RecordSink -> User Callback

Historical Pipeline:
  TcpSocket -> TlsTransport -> HttpClient -> ZstdDecompressor -> DbnParser -> RecordSink

API Pipeline (Metadata/Symbology):
  TcpSocket -> TlsTransport -> HttpClient -> JsonParser -> ResultSink
```

### Key Components

| Component | Description |
|-----------|-------------|
| `IEventLoop` | Event loop interface for custom integrations (libuv, asio) |
| `EpollEventLoop` | Built-in epoll-based event loop |
| `Timer` | Periodic and one-shot timers on any `IEventLoop` |
| `Event` | Low-level fd monitoring on any `IEventLoop` |
| `RestApiPipeline` | Coroutine-based REST API client with `PathTemplate` |
| `Pipeline<Protocol>` | Generic pipeline template with state machine |
| `LiveClient` | Alias for `Pipeline<LiveProtocol>` |
| `HistoricalClient` | Alias for `Pipeline<HistoricalProtocol>` |
| `MetadataClient` | JSON API client for metadata queries |
| `SymbologyClient` | JSON API client for symbol resolution |
| `RecordRef` | Zero-copy record reference with lifetime management |
| `BufferChain` | Zero-copy buffer management with segment pooling |

### Pipeline State Machine

```
Created -> Connecting -> Ready -> Started -> TornDown
              |            |         |
              +------------+---------+-> TornDown (on error or Stop)
```

### Data Flow

```
Network -> TcpSocket -> [Protocol Chain] -> Parser -> Sink -> User Callback
                                                        |
Backpressure <-- Suspend/Resume <-- Suspend/Resume <----+
```

## API Endpoints

| Protocol | Gateway | Port |
|----------|---------|------|
| Live | `{dataset}.lsg.databento.com` | 13000 |
| Historical | `hist.databento.com` | 443 |

## Versioning

This project follows [Semantic Versioning](https://semver.org/).

## License

MIT - See [LICENSE](LICENSE) for details.
