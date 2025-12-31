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
#include <thread>
#include <memory>
#include "src/i_event_loop.hpp"
#include "src/client.hpp"

// Handle wrapper for automatic cleanup when unregistering
class LibuvEventHandle : public dbn_pipe::IEventHandle {
public:
    LibuvEventHandle(uv_poll_t* poll, uv_loop_t* loop)
        : poll_(poll), loop_(loop) {}

    ~LibuvEventHandle() {
        if (poll_) {
            uv_poll_stop(poll_);
            uv_close(reinterpret_cast<uv_handle_t*>(poll_),
                [](uv_handle_t* h) { delete reinterpret_cast<uv_poll_t*>(h); });
        }
    }

    void Update(bool want_read, bool want_write) override {
        int events = 0;
        if (want_read) events |= UV_READABLE;
        if (want_write) events |= UV_WRITABLE;
        uv_poll_start(poll_, events, poll_callback);
    }

private:
    static void poll_callback(uv_poll_t* handle, int status, int events) {
        auto* ctx = static_cast<PollContext*>(handle->data);
        if (status < 0) {
            ctx->on_error(status);
            return;
        }
        if (events & UV_READABLE) ctx->on_read();
        if (events & UV_WRITABLE) ctx->on_write();
    }

    struct PollContext {
        std::function<void()> on_read;
        std::function<void()> on_write;
        std::function<void(int)> on_error;
    };

    uv_poll_t* poll_;
    uv_loop_t* loop_;
};

// Adapter wrapping existing uv_loop_t*
class LibuvEventLoop : public dbn_pipe::IEventLoop {
public:
    explicit LibuvEventLoop(uv_loop_t* loop)
        : loop_(loop), thread_id_(std::this_thread::get_id()) {}

    std::unique_ptr<dbn_pipe::IEventHandle> Register(
            int fd, bool want_read, bool want_write,
            ReadCallback on_read, WriteCallback on_write,
            ErrorCallback on_error) override {
        auto* poll = new uv_poll_t;
        uv_poll_init(loop_, poll, fd);

        // Store callbacks in handle data
        auto* ctx = new LibuvEventHandle::PollContext{
            std::move(on_read), std::move(on_write), std::move(on_error)};
        poll->data = ctx;

        int events = 0;
        if (want_read) events |= UV_READABLE;
        if (want_write) events |= UV_WRITABLE;
        uv_poll_start(poll, events, [](uv_poll_t* h, int status, int events) {
            auto* ctx = static_cast<LibuvEventHandle::PollContext*>(h->data);
            if (status < 0) { ctx->on_error(status); return; }
            if (events & UV_READABLE) ctx->on_read();
            if (events & UV_WRITABLE) ctx->on_write();
        });

        return std::make_unique<LibuvEventHandle>(poll, loop_);
    }

    void Defer(std::function<void()> fn) override {
        auto* async = new uv_async_t;
        async->data = new std::function<void()>(std::move(fn));
        uv_async_init(loop_, async, [](uv_async_t* h) {
            auto* fn = static_cast<std::function<void()>*>(h->data);
            (*fn)();
            delete fn;
            uv_close(reinterpret_cast<uv_handle_t*>(h),
                [](uv_handle_t* h) { delete reinterpret_cast<uv_async_t*>(h); });
        });
        uv_async_send(async);
    }

    bool IsInEventLoopThread() const override {
        return std::this_thread::get_id() == thread_id_;
    }

private:
    uv_loop_t* loop_;  // Non-owning
    std::thread::id thread_id_;
};

int main() {
    uv_loop_t* loop = uv_default_loop();
    LibuvEventLoop adapter(loop);

    auto client = dbn_pipe::LiveClient::Create(adapter, "your-api-key");

    client->SetRequest({
        .dataset = "GLBX.MDP3",
        .symbols = "ESZ4",
        .schema = "mbp-1"
    });

    client->OnRecord([](dbn_pipe::RecordBatch&& batch) {
        for (const auto& ref : batch) {
            // Access record: ref.Header(), ref.As<databento::TradeMsg>(), etc.
        }
    });

    client->OnError([](const dbn_pipe::Error& e) {
        std::cerr << "Error: " << e.message << std::endl;
    });

    auto addr = dbn_pipe::ResolveHostname("glbx-mdp3.lsg.databento.com", 13000);
    client->Connect(*addr);
    client->Start();

    uv_run(loop, UV_RUN_DEFAULT);  // Your loop drives everything
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

Apache 2.0 - See [LICENSE](LICENSE) for details.
