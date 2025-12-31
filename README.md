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
#include "src/client.hpp"

using namespace dbn_pipe;

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

using namespace dbn_pipe;

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

#### Integration with Existing Event Loop (libuv)

For applications with an existing event loop, implement the `IEventLoop` interface:

```cpp
#include <uv.h>
#include "src/client.hpp"

// Adapter wrapping existing uv_loop_t* (see docs/libuv-integration.md)
class LibuvEventLoop : public dbn_pipe::IEventLoop {
public:
    explicit LibuvEventLoop(uv_loop_t* loop);

    std::unique_ptr<IEventHandle> Register(
        int fd, bool want_read, bool want_write,
        ReadCallback on_read, WriteCallback on_write,
        ErrorCallback on_error) override;

    void Defer(std::function<void()> fn) override;
    bool IsInEventLoopThread() const override;

private:
    uv_loop_t* loop_;  // Non-owning
};

int main() {
    uv_loop_t* loop = uv_default_loop();
    LibuvEventLoop adapter(loop);  // Wrap existing loop

    auto client = dbn_pipe::LiveClient::Create(adapter, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "mbp-1"
    });

    client->OnRecord([](const dbn_pipe::DbnRecord& rec) {
        // Process alongside other libuv handlers
    });

    client->Connect();
    client->Start();

    uv_run(loop, UV_RUN_DEFAULT);  // Your loop drives everything
}
```

See [docs/libuv-integration.md](docs/libuv-integration.md) for complete implementation.

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
| `IEventLoop` | Event loop interface for custom integrations (libuv, asio) |
| `EventLoop` | Built-in epoll-based event loop |
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

## Versioning

This project follows [Semantic Versioning](https://semver.org/).

## License

MIT
