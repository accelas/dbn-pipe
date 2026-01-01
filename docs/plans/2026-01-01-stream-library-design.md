# Stream Library Extraction Design

Extract generic streaming components from dbn-pipe into `lib/stream/` for reuse.

## Decisions

- **Use case**: Personal reuse (simpler API)
- **Scope**: Core streaming + event loop (~8 headers)
- **Structure**: Subdirectory `lib/stream/` now, separate repo later
- **Namespace**: `dbn_pipe` (unchanged, rename during repo split)
- **Design**: Header-only
- **Error handling**: `std::error_code` with `StreamError` enum
- **Component base**: Monolithic, replace CRTP with C++23 deducing this

## Library Structure

```
lib/stream/
├── buffer_chain.hpp    # Segment, SegmentPool, BufferChain
├── component.hpp       # Component base with deducing this
├── concepts.hpp        # Suspendable, Downstream, Upstream, TerminalDownstream
├── error.hpp           # StreamError enum, std::error_code integration
├── reactor.hpp         # IEventLoop, IEventHandle, Reactor, Timer, Event, EventLoop
├── tcp_socket.hpp      # TcpSocket template
└── BUILD.bazel
```

## Error Handling

Replace `Error` struct with `std::error_code`:

```cpp
// lib/stream/error.hpp
namespace dbn_pipe {

enum class StreamError {
    Success = 0,
    ConnectionFailed,
    DnsResolutionFailed,
    BufferOverflow,
    ParseError,
    InvalidState,
    Timeout,
};

const std::error_category& stream_category() noexcept;
std::error_code make_error_code(StreamError e) noexcept;

}  // namespace dbn_pipe

namespace std {
template<>
struct is_error_code_enum<dbn_pipe::StreamError> : true_type {};
}
```

## Component Base

Replace CRTP with deducing this:

```cpp
// lib/stream/component.hpp
class Component : public Suspendable,
                  public std::enable_shared_from_this<Component> {
public:
    explicit Component(IEventLoop& loop) : loop_(loop) {}

    void RequestClose(this auto&& self) {
        if (self.closed_) return;
        self.closed_ = true;
        self.DisableWatchers();
        // ...
    }

    void Resume(this auto&& self) override {
        // Uses self.ProcessPending(), self.FlushAndComplete()
    }
};
```

## Concepts

```cpp
// lib/stream/concepts.hpp
namespace dbn_pipe {

class Suspendable {
public:
    virtual ~Suspendable() = default;
    virtual void Suspend() = 0;
    virtual void Resume() = 0;
    virtual void Close() = 0;
    virtual bool IsSuspended() const = 0;
};

template<typename D>
concept TerminalDownstream = requires(D& d, std::error_code ec) {
    { d.OnError(ec) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

template<typename D>
concept Downstream = requires(D& d, BufferChain& chain, std::error_code ec) {
    { d.OnData(chain) } -> std::same_as<void>;
    { d.OnError(ec) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

template<typename U>
concept Upstream = requires(U& u, BufferChain chain) {
    { u.Write(std::move(chain)) } -> std::same_as<void>;
    { u.Suspend() } -> std::same_as<void>;
    { u.Resume() } -> std::same_as<void>;
    { u.Close() } -> std::same_as<void>;
};

}  // namespace dbn_pipe
```

## Header-Only Reactor

Merge all event loop code into `reactor.hpp`:

```cpp
// lib/stream/reactor.hpp
namespace dbn_pipe {

class IEventHandle { /* ... */ };
class IEventLoop { /* ... */ };

class Reactor : public IEventLoop {
public:
    Reactor() : epoll_fd_(epoll_create1(EPOLL_CLOEXEC)) { /* inline */ }
    int Poll(int timeout_ms = -1) { /* inline */ }
    void Run() { /* inline */ }
    // ...
};

class Timer { /* inline */ };
class Event { /* inline */ };

class EventLoop {
    std::unique_ptr<Reactor> impl_;
public:
    EventLoop() : impl_(std::make_unique<Reactor>()) {}
    operator IEventLoop&() { return *impl_; }
    // ...
};

}  // namespace dbn_pipe
```

## dbn-pipe Integration

### Re-export header

```cpp
// src/stream.hpp
#pragma once

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/component.hpp"
#include "lib/stream/concepts.hpp"
#include "lib/stream/error.hpp"
#include "lib/stream/reactor.hpp"
#include "lib/stream/tcp_socket.hpp"
```

### Files remaining in src/

```
src/
├── stream.hpp              # Re-export header
├── client.hpp
├── cram_auth.hpp
├── dbn_parser_component.hpp
├── dns_resolver.hpp
├── historical_protocol.hpp
├── http_client.hpp
├── live_protocol.hpp
├── pipeline.hpp
├── pipeline_sink.hpp
├── protocol_driver.hpp
├── record_batch.hpp
├── symbol_map.hpp
├── tls_transport.hpp
├── zstd_decompressor.hpp
└── BUILD.bazel
```

### Files to delete from src/

- `buffer_chain.hpp`
- `pipeline_component.hpp`
- `tcp_socket.hpp`
- `event_loop.hpp`, `event_loop.cpp`
- `reactor.hpp`, `reactor.cpp`
- `epoll_event_loop.hpp`, `epoll_event_loop.cpp`
- `error.hpp`

### Include updates

Internal src/ files:
```cpp
// Before
#include "event_loop.hpp"
#include "pipeline_component.hpp"

// After
#include "lib/stream/reactor.hpp"
#include "lib/stream/component.hpp"
```

### Error migration

```cpp
// Before
downstream.OnError(Error{ErrorCode::ConnectionFailed, "read() failed", errno});

// After
downstream.OnError(std::error_code{errno, std::system_category()});
// Or:
downstream.OnError(StreamError::ConnectionFailed);
```

## Documentation Updates

Update `docs/libuv-integration.md`:

```cpp
// Before
#include "event_loop.hpp"
client->OnError([](const dbn_pipe::Error& error) {
    std::cerr << "Error: " << error.message << "\n";
});

// After
#include "src/stream.hpp"
client->OnError([](std::error_code ec) {
    std::cerr << "Error: " << ec.message() << "\n";
});
```

## Implementation Order

1. Create `lib/stream/` directory and `BUILD.bazel`
2. Create `lib/stream/error.hpp` with `StreamError` enum
3. Create `lib/stream/concepts.hpp` with extracted concepts
4. Create `lib/stream/reactor.hpp` (merge and inline)
5. Create `lib/stream/buffer_chain.hpp` (move, no changes)
6. Create `lib/stream/component.hpp` (refactor to deducing this)
7. Create `lib/stream/tcp_socket.hpp` (move, update error handling)
8. Create `src/stream.hpp` re-export header
9. Update src/ files to use new includes and `std::error_code`
10. Update docs/libuv-integration.md
11. Delete old files from src/
12. Run tests and fix any issues
