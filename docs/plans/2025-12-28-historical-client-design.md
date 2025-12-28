# Historical Client Design

## Overview

Async historical data download from Databento's Historical API using streaming HTTPS with zstd decompression. Shares composable pipeline architecture with LiveClient.

## Architecture

**Push-based pipeline** with ownership flowing same direction as data:

```
     Read() / OnError() / OnDone()      Suspend() / Resume() / Close()
     (downstream ▼)                      (upstream ▲)
            │                                   │
┌───────────┴───────────────────────────────────┴───────────┐
│                   TcpSocket (owns ▼)                       │
└───────────────────────────────────────────────────────────┘
            │ ▼                                 ▲ │
┌───────────┴───────────────────────────────────┴───────────┐
│                   TlsSocket (owns ▼)                       │
└───────────────────────────────────────────────────────────┘
            │ ▼                                 ▲ │
┌───────────┴───────────────────────────────────┴───────────┐
│                   HttpClient (owns ▼)                      │
└───────────────────────────────────────────────────────────┘
            │ ▼                                 ▲ │
┌───────────┴───────────────────────────────────┴───────────┐
│                 ZstdDecompressor (owns ▼)                  │
└───────────────────────────────────────────────────────────┘
            │ ▼                                 ▲ │
┌───────────┴───────────────────────────────────┴───────────┐
│                   DbnParser (owns ▼)                       │
└───────────────────────────────────────────────────────────┘
            │ ▼                                 ▲ │
┌───────────┴───────────────────────────────────┴───────────┐
│                  Application                               │
│           (raw ptr to TcpSocket head)                      │
└───────────────────────────────────────────────────────────┘
            │
            ▼ Write() flows upstream
```

**Five downstream operations:**
- `Read(vector)` - data chunk, ownership transferred
- `OnError(error)` - error, no more reads
- `OnDone()` - success/EOF, flush signal, no more reads for this message
- `Write(vector)` - send data upstream

**Three upstream operations:**
- `Suspend()` - backpressure, stop sending
- `Resume()` - resume sending
- `Close()` - teardown, propagates upstream

## Design Decisions

### Ownership Model

Upstream owns downstream (same direction as data flow):

```
TcpSocket
  └─ shared_ptr<TlsSocket>
       └─ shared_ptr<HttpClient>
            └─ shared_ptr<ZstdDecompressor>
                 └─ shared_ptr<DbnParser>
```

Application holds raw pointer to TcpSocket (head of chain). Teardown via `Close()`.

### Memory Management (PMR)

Each component owns module-local PMR pools:

```cpp
template<typename Downstream>
class TlsSocket : public Suspendable {
private:
    std::pmr::unsynchronized_pool_resource decrypt_pool_;
    std::pmr::unsynchronized_pool_resource encrypt_pool_;
};
```

Benefits:
- No external pool coordination
- Pools destroyed with component
- `unsynchronized_pool_resource` recycles buffers in steady state

### Read() Ownership Transfer

`Read()` transfers ownership - callee responsible for data:

```cpp
void Read(std::pmr::vector<std::byte> data);  // ownership transferred
```

Each component:
1. Processes what it can
2. Buffers remainder internally if suspended or partial data
3. Allocates output from local pool, moves to downstream

### Static Dispatch

Hot path uses templates for static dispatch:

```cpp
class Suspendable {
public:
    virtual ~Suspendable() = default;
    virtual void Suspend() = 0;
    virtual void Resume() = 0;
    virtual void Close() = 0;
};

template<typename Downstream>
class Component : public Suspendable {
public:
    // Hot path - static dispatch, inlined
    void Read(std::pmr::vector<std::byte> data) {
        std::pmr::vector<std::byte> processed{&output_pool_};
        // process...
        downstream_->Read(std::move(processed));
    }

    void OnError(const Error& e) {
        downstream_->OnError(e);
    }

    void OnDone() {
        // flush any buffered data
        downstream_->OnDone();
    }

    void Write(std::pmr::vector<std::byte> data) {
        upstream_->Write(std::move(data));
    }

    // Cold path - virtual dispatch
    void Suspend() override { upstream_->Suspend(); }
    void Resume() override { upstream_->Resume(); }
    void Close() override {
        downstream_.reset();  // release downstream
    }

private:
    Suspendable* upstream_;
    std::shared_ptr<Downstream> downstream_;
    std::pmr::unsynchronized_pool_resource output_pool_;
};
```

### Teardown Ordering

On `Close()` or EOF, strict ordering to prevent races:

```cpp
void TcpSocket::Close() {
    event_.Remove();        // 1. deregister from reactor
    downstream_.reset();    // 2. release downstream chain
    close(fd_);             // 3. close fd last
}
```

Single-threaded reactor guarantees no callback between steps.

### EOF Handling

When TcpSocket receives EOF:

```cpp
void TcpSocket::HandleRead() {
    ssize_t n = read(fd_, buf, size);
    if (n > 0) {
        downstream_->Read(std::move(buf));
    } else if (n == 0) {
        downstream_->OnDone();  // 1. flush downstream
        Close();                 // 2. teardown
    } else {
        downstream_->OnError(...);
        Close();
    }
}
```

Both terminal states (done/error) end with `Close()`.

### OnDone() as Flush Signal

`OnDone()` signals EOF and triggers flush:

```cpp
void ZstdDecompressor::OnDone() {
    // Flush remaining decompressed data
    if (!buffered_.empty()) {
        downstream_->Read(std::move(buffered_));
    }
    // Check for incomplete frame
    if (!zstd_frame_complete_) {
        downstream_->OnError({ErrorCode::DecompressionError, "truncated"});
        return;
    }
    downstream_->OnDone();
}
```

After `OnDone()`, next `Read()` starts new message.

## Components

### TcpSocket

Entry point, owns the pipeline:

```cpp
class TcpSocket {
public:
    TcpSocket(Reactor& reactor);

    void Connect(const sockaddr_storage& addr);
    void Write(std::pmr::vector<std::byte> data);
    void Close();

    template<typename Downstream>
    void SetDownstream(std::shared_ptr<Downstream> downstream);

private:
    void HandleRead();
    void HandleWrite();
    void HandleError();

    Reactor& reactor_;
    std::unique_ptr<Event> event_;
    std::shared_ptr<void> downstream_;  // type-erased

    std::pmr::unsynchronized_pool_resource read_pool_;
};
```

### TlsSocket

OpenSSL wrapper with async BIO:

```cpp
template<typename Downstream>
class TlsSocket : public Suspendable {
public:
    TlsSocket(std::shared_ptr<Downstream> downstream);
    ~TlsSocket();

    void SetUpstream(Suspendable* upstream) { upstream_ = upstream; }

    // Initiate TLS handshake
    void Connect(const std::string& hostname);

    // From upstream
    void Read(std::pmr::vector<std::byte> ciphertext);
    void OnError(const Error& e);
    void OnDone();

    // From downstream
    void Write(std::pmr::vector<std::byte> plaintext);
    void Suspend() override;
    void Resume() override;
    void Close() override;

private:
    void InitContext() {
        ctx_ = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(ctx_);      // system CA
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    }

    void SetupVerification(const std::string& hostname) {
        SSL_set_tlsext_host_name(ssl_, hostname.c_str());  // SNI
        SSL_set1_host(ssl_, hostname.c_str());              // hostname verify
        SSL_set_verify(ssl_, SSL_VERIFY_PEER, nullptr);
    }

    Suspendable* upstream_ = nullptr;
    std::shared_ptr<Downstream> downstream_;

    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;
    BIO* wbio_ = nullptr;

    std::pmr::unsynchronized_pool_resource decrypt_pool_;
    std::pmr::unsynchronized_pool_resource encrypt_pool_;
};
```

Uses memory BIOs for async integration with Reactor.

### HttpClient

HTTP/1.1 with llhttp:

```cpp
template<typename Downstream>
class HttpClient : public Suspendable {
public:
    HttpClient(std::shared_ptr<Downstream> downstream);
    ~HttpClient();

    void SetUpstream(Suspendable* upstream) { upstream_ = upstream; }

    // From upstream
    void Read(std::pmr::vector<std::byte> plaintext);
    void OnError(const Error& e);
    void OnDone();

    // From downstream
    void Write(std::pmr::vector<std::byte> data);  // pass-through
    void Suspend() override;
    void Resume() override;
    void Close() override;

    // Send HTTP GET request
    void Request(std::string_view host,
                 std::string_view path,
                 std::string_view auth_header);

private:
    static int OnBody(llhttp_t* parser, const char* data, size_t len);
    static int OnMessageComplete(llhttp_t* parser);

    Suspendable* upstream_ = nullptr;
    std::shared_ptr<Downstream> downstream_;

    llhttp_t parser_;
    llhttp_settings_t settings_;
    int status_code_ = 0;

    std::pmr::unsynchronized_pool_resource write_pool_;
};
```

On HTTP 4xx/5xx, calls `OnError()` with status and raw body.
On `on_message_complete`, calls `OnDone()`.

### ZstdDecompressor

Streaming zstd:

```cpp
template<typename Downstream>
class ZstdDecompressor : public Suspendable {
public:
    ZstdDecompressor(std::shared_ptr<Downstream> downstream);
    ~ZstdDecompressor();

    void SetUpstream(Suspendable* upstream) { upstream_ = upstream; }

    // From upstream
    void Read(std::pmr::vector<std::byte> compressed);
    void OnError(const Error& e);
    void OnDone();

    // From downstream
    void Suspend() override;
    void Resume() override;
    void Close() override;

private:
    Suspendable* upstream_ = nullptr;
    std::shared_ptr<Downstream> downstream_;

    ZSTD_DStream* dstream_ = nullptr;
    std::pmr::unsynchronized_pool_resource decompress_pool_;
};
```

### HistoricalClient

Pipeline assembly:

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

    TcpSocket* tcp_ = nullptr;  // raw ptr to head
    std::shared_ptr<HistoricalPipeline> pipeline_;
};
```

## LiveClient Refactor

Same pipeline pattern:

```
TcpSocket → CramAuth → DbnParser → Application
```

### CramAuth

Two-phase: auth then bypass:

```cpp
template<typename Downstream>
class CramAuth : public Suspendable {
public:
    CramAuth(std::string api_key, std::shared_ptr<Downstream> downstream);

    void SetUpstream(Suspendable* upstream) { upstream_ = upstream; }

    void Read(std::pmr::vector<std::byte> data) {
        if (authenticated_) {
            downstream_->Read(std::move(data));  // bypass
            return;
        }
        ProcessAuthData(std::move(data));
    }

    void OnError(const Error& e);
    void OnDone();

    // From downstream
    void Write(std::pmr::vector<std::byte> data);
    void Suspend() override;
    void Resume() override;
    void Close() override;

    void Subscribe(...);
    void Start();

private:
    void ProcessAuthData(std::pmr::vector<std::byte> data);

    void EnterBypass(std::pmr::vector<std::byte> residual) {
        authenticated_ = true;
        line_buffer_.clear();
        line_buffer_.shrink_to_fit();  // free auth buffers

        if (!residual.empty()) {
            downstream_->Read(std::move(residual));  // forward residual
        }
    }

    std::string api_key_;
    std::shared_ptr<Downstream> downstream_;
    Suspendable* upstream_ = nullptr;

    bool authenticated_ = false;
    std::string line_buffer_;

    std::pmr::unsynchronized_pool_resource write_pool_;
};
```

Key: forward residual bytes after auth, free auth buffers.

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

Errors propagate downstream via `OnError()`, then `Close()` for cleanup.

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
