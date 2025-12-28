# Event Loop Architecture Design

## Overview

Async databento client with user-controlled event loop. No hidden threads, explicit backpressure, Linux-only.

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                       Application                             │
│                  OnRecord() / OnError()                       │
└──────────────────────────────────────────────────────────────┘
                              ▲
                              │ callbacks
┌──────────────────────────────────────────────────────────────┐
│                       DataSource                              │
│              Pause() / Resume() / Start() / Stop()            │
└──────────────────────────────────────────────────────────────┘
                              ▲
                              │ records
┌──────────────────────────────────────────────────────────────┐
│                        DbnParser                              │
│                   Push(bytes) → Pull(records)                 │
└──────────────────────────────────────────────────────────────┘
                              ▲
                              │ raw bytes
┌─────────────────────────────┴────────────────────────────────┐
│  ┌─────────────────────┐       ┌───────────────────────────┐ │
│  │     LiveClient      │       │    HistoricalClient       │ │
│  │  TCP + CRAM auth    │       │  TLS + HTTP + zstd        │ │
│  └──────────┬──────────┘       └────────────┬──────────────┘ │
└─────────────┼───────────────────────────────┼────────────────┘
              │                               │
┌─────────────┴───────────────────────────────┴────────────────┐
│                         Reactor                               │
│                    (thin epoll wrapper)                       │
└──────────────────────────────────────────────────────────────┘
```

## Design Decisions

### Event Loop
- Thin epoll wrapper (`Reactor`), not libuv
- User calls `Poll()` or `Run()` - we never own the loop
- Linux only (no portability layer)
- Easy to replace with io_uring in future

### Record Delivery
Callbacks with Overload pattern for static dispatch:

```cpp
client.OnRecord(Overload{
    [](const MboMsg& m) { /* handle MBO */ },
    [](const TradeMsg& m) { /* handle trade */ },
    [](const auto&) { /* fallback */ }
});
```

Library does runtime switch on `rtype`, compiler dispatches to correct overload.

### Error Handling
Separate error callback:

```cpp
client.OnError([](const Error& e) {
    if (e.code == ErrorCode::ConnectionClosed) {
        // user decides whether to reconnect
    }
});
```

### Backpressure
DataSource provides Pause/Resume for flow control:

```cpp
client.OnRecord([&](const Record& r) {
    if (!queue.TryPush(r)) {
        client.Pause();   // stop reading socket
        queue.Push(r);    // blocking push
        client.Resume();  // resume reading
    }
});
```

### Connection Lifecycle
- No auto-reconnect
- User handles reconnection via OnError callback
- Single-threaded: all calls from reactor thread only

### Live API
- Plain TCP on port 13000
- CRAM challenge-response auth (SHA256)
- No compression

### Historical API
- HTTPS (TLS via OpenSSL)
- HTTP parsing via llhttp
- zstd decompression

## Core Components

### Reactor

```cpp
class Reactor {
public:
    using Callback = std::function<void(uint32_t events)>;

    Reactor();
    ~Reactor();

    void Add(int fd, uint32_t events, Callback cb);
    void Modify(int fd, uint32_t events);
    void Remove(int fd);

    int Poll(int timeout_ms = -1);
    void Run();
    void Stop();

private:
    int epoll_fd_;
    bool running_ = false;
    std::unordered_map<int, Callback> callbacks_;
};
```

### DataSource

```cpp
class DataSource {
public:
    virtual ~DataSource() = default;

    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;

    template<typename Handler>
    void OnRecord(Handler&& h);

    template<typename Handler>
    void OnError(Handler&& h);

    bool IsPaused() const;

protected:
    void DeliverBytes(std::span<const std::byte> data);
    void DeliverError(Error e);

    DbnParser parser_;
    bool paused_ = false;
};
```

### LiveClient

```cpp
class LiveClient : public DataSource {
public:
    LiveClient(Reactor* reactor, std::string api_key);

    void Connect(std::string_view gateway = "egress.databento.com",
                 int port = 13000);
    void Close();

    void Subscribe(std::string_view dataset,
                   std::string_view symbols,
                   Schema schema);

    void Start() override;
    void Stop() override;
    void Pause() override;
    void Resume() override;

    enum class State {
        Disconnected,
        Connecting,
        Authenticating,
        Ready,
        Streaming
    };
    State GetState() const;
};
```

### HistoricalClient

```cpp
class HistoricalClient : public DataSource {
public:
    HistoricalClient(Reactor* reactor, std::string api_key);

    void Request(std::string_view dataset,
                 std::string_view symbols,
                 Schema schema,
                 Timestamp start,
                 Timestamp end);

    void Start() override;
    void Stop() override;
    void Pause() override;
    void Resume() override;
};
```

### Error Types

```cpp
enum class ErrorCode {
    // Connection
    ConnectionFailed,
    ConnectionClosed,
    DnsResolutionFailed,

    // Auth
    AuthFailed,
    InvalidApiKey,

    // Protocol
    InvalidGreeting,
    InvalidChallenge,
    ParseError,

    // Subscription
    InvalidDataset,
    InvalidSymbol,
    InvalidSchema,

    // TLS (Historical)
    TlsHandshakeFailed,
    CertificateError,

    // HTTP (Historical)
    HttpError,

    // Decompression (Historical)
    DecompressionError,
};

struct Error {
    ErrorCode code;
    std::string message;
    int os_errno = 0;
};
```

## Dependencies

| Library | Purpose | Used By |
|---------|---------|---------|
| openssl | TLS, SHA256 | Both |
| llhttp | HTTP parsing | Historical |
| zstd | Decompression | Historical |

No libuv. No external event loop library.

## Implementation Phases

### Phase 1: Core
1. Reactor (epoll wrapper)
2. DbnParser (exists)
3. DataSource base class

### Phase 2: Live Client
4. TCP socket wrapper
5. CRAM auth (SHA256)
6. LiveClient

### Phase 3: Historical Client
7. TLS wrapper (OpenSSL)
8. HTTP client (llhttp)
9. Zstd decoder
10. HistoricalClient

### Phase 4: Polish
11. Async DNS resolution
12. Timeouts
13. Tests

## Constraints

- C++23 standard
- Linux only
- Single-threaded (all calls from reactor thread)
- Use databento-cpp types for records
