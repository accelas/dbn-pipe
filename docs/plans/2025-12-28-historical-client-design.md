# Historical Client Design

## Overview

Async historical data download from Databento's Historical API using streaming HTTPS with zstd decompression. Shares composable pipeline architecture with LiveClient.

## Architecture

**Push-based pipeline** - each component receives data via `Read()`, processes it, and pushes to the next component's `Read()`:

```
     Read() / OnError()          Suspend() / Resume()
     (downstream ▼)              (upstream ▲)
            │                          │
┌───────────┴──────────────────────────┴───────────┐
│                   TcpSocket                       │
└───────────────────────────────────────────────────┘
            │ ▼                        ▲ │
┌───────────┴──────────────────────────┴───────────┐
│                   TlsSocket                       │
└───────────────────────────────────────────────────┘
            │ ▼                        ▲ │
┌───────────┴──────────────────────────┴───────────┐
│                   HttpClient                      │
└───────────────────────────────────────────────────┘
            │ ▼                        ▲ │
┌───────────┴──────────────────────────┴───────────┐
│                 ZstdDecompressor                  │
└───────────────────────────────────────────────────┘
            │ ▼                        ▲ │
┌───────────┴──────────────────────────┴───────────┐
│                   DbnParser                       │
└───────────────────────────────────────────────────┘
            │ ▼                        ▲ │
┌───────────┴──────────────────────────┴───────────┐
│                  Application                      │
│         (handles error, initiates Close)          │
└───────────────────────────────────────────────────┘
```

**Four operations:**
- `Read()` - data flows downstream
- `OnError()` - errors flow downstream to application
- `Suspend()`/`Resume()` - backpressure flows upstream
- Teardown via shared_ptr release (RAII)

## Design Decisions

### Static Dispatch

Hot path uses templates for static dispatch, cold path uses virtual interface:

```cpp
class Suspendable {
public:
    virtual ~Suspendable() = default;
    virtual void Suspend() = 0;
    virtual void Resume() = 0;
};

template<typename Downstream>
class Component : public Suspendable {
public:
    // Hot path - static dispatch
    void Read(std::span<const std::byte> data) {
        // process...
        downstream_->Read(processed);  // inlined
    }

    void OnError(const Error& e) {
        downstream_->OnError(e);  // inlined
    }

    // Cold path - virtual dispatch
    void Suspend() override { upstream_->Suspend(); }
    void Resume() override { upstream_->Resume(); }

private:
    Suspendable* upstream_;                   // type-erased, cold path
    std::shared_ptr<Downstream> downstream_;  // concrete, hot path
};
```

### Ownership Model

Application owns the pipeline top-down (opposite of data flow):

```
Application
  └─ shared_ptr<DbnParser>
       └─ shared_ptr<ZstdDecompressor>
            └─ shared_ptr<HttpClient>
                 └─ shared_ptr<TlsSocket>
                      └─ shared_ptr<TcpSocket>
```

Teardown happens automatically when Application releases its reference.

## Components

### TlsSocket

Wraps TcpSocket, handles OpenSSL encryption/decryption:

```cpp
template<typename Downstream>
class TlsSocket : public Suspendable {
public:
    TlsSocket(std::shared_ptr<TcpSocket> tcp,
              std::shared_ptr<Downstream> downstream);
    ~TlsSocket();

    // Initiate TLS handshake (hostname for SNI)
    void Connect(const std::string& hostname);

    // From upstream (TcpSocket callbacks)
    void Read(std::span<const std::byte> ciphertext);
    void OnError(const Error& e);

    // From downstream (backpressure)
    void Suspend() override;
    void Resume() override;

    // Write encrypted data
    void Write(std::span<const std::byte> plaintext);

private:
    std::shared_ptr<TcpSocket> tcp_;
    std::shared_ptr<Downstream> downstream_;

    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;  // read bio (network -> SSL)
    BIO* wbio_ = nullptr;  // write bio (SSL -> network)

    bool handshake_complete_ = false;
};
```

Uses memory BIOs (not socket BIOs) for async integration with Reactor.

### HttpClient

Handles HTTP/1.1 request/response with llhttp:

```cpp
template<typename Downstream>
class HttpClient : public Suspendable {
public:
    HttpClient(std::shared_ptr<Downstream> downstream);
    ~HttpClient();

    void SetUpstream(Suspendable* upstream);

    // Send HTTP GET request
    void Request(std::string_view host,
                 std::string_view path,
                 std::string_view auth_header);

    // From upstream (TlsSocket)
    void Read(std::span<const std::byte> plaintext);
    void OnError(const Error& e);

    // From downstream
    void Suspend() override;
    void Resume() override;

    template<typename TlsSocket>
    void SetWriter(TlsSocket* tls);

private:
    Suspendable* upstream_ = nullptr;
    std::shared_ptr<Downstream> downstream_;
    std::function<void(std::span<const std::byte>)> write_;

    llhttp_t parser_;
    llhttp_settings_t settings_;

    int status_code_ = 0;
};
```

On HTTP 4xx/5xx, calls `downstream_->OnError()` with status and raw body (no JSON parsing).

### ZstdDecompressor

Streaming zstd decompression:

```cpp
template<typename Downstream>
class ZstdDecompressor : public Suspendable {
public:
    ZstdDecompressor(std::shared_ptr<Downstream> downstream);
    ~ZstdDecompressor();

    void SetUpstream(Suspendable* upstream);

    // From upstream (HttpClient body chunks)
    void Read(std::span<const std::byte> compressed);
    void OnError(const Error& e);

    // From downstream
    void Suspend() override;
    void Resume() override;

private:
    Suspendable* upstream_ = nullptr;
    std::shared_ptr<Downstream> downstream_;

    ZSTD_DStream* dstream_ = nullptr;
    std::vector<std::byte> output_buffer_;
};
```

Uses `ZSTD_decompressStream()` for streaming decompression.

### HistoricalClient

Ties the pipeline together:

```cpp
using HistoricalPipeline = TlsSocket<HttpClient<ZstdDecompressor<DbnParser>>>;

class HistoricalClient : public DataSource {
public:
    HistoricalClient(Reactor& reactor, std::string api_key);
    ~HistoricalClient() override;

    void Request(std::string_view dataset,
                 std::string_view symbols,
                 std::string_view schema,
                 databento::UnixNanos start,
                 databento::UnixNanos end);

    void Start() override;
    void Stop() override;
    void Pause() override;
    void Resume() override;

    enum class State {
        Idle,
        Connecting,
        Handshaking,
        Requesting,
        Streaming,
        Complete,
        Error
    };

    State GetState() const;

private:
    Reactor& reactor_;
    std::string api_key_;
    State state_ = State::Idle;

    std::shared_ptr<TcpSocket> tcp_;
    std::shared_ptr<HistoricalPipeline> pipeline_;
};
```

## LiveClient Refactor

Restructure LiveClient to use the same pattern:

```
TcpSocket → CramAuth → DbnParser → Application
```

### CramAuth

Two-phase component - auth then bypass:

```cpp
template<typename Downstream>
class CramAuth : public Suspendable {
public:
    CramAuth(std::string api_key, std::shared_ptr<Downstream> downstream);

    void Read(std::span<const std::byte> data) {
        if (authenticated_) {
            downstream_->Read(data);  // bypass: zero overhead
            return;
        }
        ProcessAuthData(data);  // auth phase
    }

    void OnError(const Error& e);
    void Suspend() override;
    void Resume() override;

    void Subscribe(std::string_view dataset,
                   std::string_view symbols,
                   std::string_view schema);
    void Start();

private:
    void ProcessAuthData(std::span<const std::byte> data);
    void EnterBypass();

    std::string api_key_;
    std::shared_ptr<Downstream> downstream_;
    Suspendable* upstream_ = nullptr;

    bool authenticated_ = false;
    std::string line_buffer_;
};
```

Once authenticated, CramAuth becomes a passthrough with single branch check.

### Pipeline Comparison

| Component | HistoricalClient | LiveClient |
|-----------|------------------|------------|
| Transport | TcpSocket | TcpSocket |
| Encryption | TlsSocket | - |
| Protocol | HttpClient | CramAuth |
| Compression | ZstdDecompressor | - |
| Parsing | DbnParser | DbnParser |

## Error Handling

```cpp
enum class ErrorCode {
    // Connection
    ConnectionFailed,
    ConnectionClosed,

    // Auth
    AuthFailed,

    // TLS
    TlsHandshakeFailed,
    TlsCertificateError,

    // HTTP
    HttpError,
    HttpParseError,

    // Decompression
    DecompressionError,

    // Parser
    ParseError,
};
```

Errors propagate downstream through `OnError()` until they reach the application.

## Dependencies

| Library | Purpose | Used By |
|---------|---------|---------|
| OpenSSL | TLS, SHA256 | TlsSocket, CramAuth |
| llhttp | HTTP parsing | HttpClient |
| zstd | Decompression | ZstdDecompressor |

## Implementation Order

1. Refactor existing code to composable pattern
   - Extract DbnParser as standalone component
   - Refactor CramAuth from LiveClient
   - Update LiveClient to use pipeline

2. New components for HistoricalClient
   - TlsSocket (OpenSSL wrapper)
   - HttpClient (llhttp wrapper)
   - ZstdDecompressor (zstd wrapper)
   - HistoricalClient (pipeline assembly)
