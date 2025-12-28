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

### Write() Allocator Lifetime

`Write()` flows upstream. **Critical rule**: receiver must consume or copy immediately.

```cpp
void Write(std::pmr::vector<std::byte> data) {
    // Option 1: Consume immediately (TcpSocket does this)
    socket_write(data.data(), data.size());
    // data destructs, memory returns to caller's pool

    // Option 2: Copy into local pool if buffering needed
    pending_writes_.push_back(
        std::pmr::vector<std::byte>(data.begin(), data.end(), &write_pool_)
    );
}
```

Since Write() data originates from downstream's pool, upstream cannot hold references
beyond the call without copying. TcpSocket (the sink) writes to socket immediately,
so no buffering issue. Intermediate components pass through without buffering.

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
        if (closed_) return;  // idempotent
        closed_ = true;
        downstream_.reset();  // release downstream
        upstream_->Close();   // propagate upstream to stop socket
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
    if (closed_) return;    // idempotent
    closed_ = true;
    event_.Remove();        // 1. deregister from reactor
    downstream_.reset();    // 2. release downstream chain
    close(fd_);             // 3. close fd last
}
```

Single-threaded reactor guarantees no callback between steps.

### Error-Triggered Teardown

When a component detects an error, it propagates error downstream, then closes upstream:

```cpp
void ZstdDecompressor::Read(std::pmr::vector<std::byte> data) {
    size_t ret = ZSTD_decompressStream(...);
    if (ZSTD_isError(ret)) {
        downstream_->OnError({ErrorCode::DecompressionError, ...});
        upstream_->Close();  // stop the socket
        return;
    }
    // ...
}
```

**Error flow:**
1. Component detects error
2. Calls `downstream_->OnError()` - propagates to application
3. Calls `upstream_->Close()` - stops socket, releases chain

`Close()` is idempotent - safe to call multiple times from different error paths.

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

    // Reset for next message (keep-alive scenarios)
    ResetMessageState();
}
```

After `OnDone()`, next `Read()` starts new message.

### Message Boundary Reset

Each component resets per-message state after `OnDone()`:

| Component | State to Reset |
|-----------|----------------|
| HttpClient | `llhttp_init()`, clear status_code, headers_done |
| ZstdDecompressor | `ZSTD_initDStream()`, clear buffered data |
| DbnParser | Clear partial record buffer |
| CramAuth | No reset needed (bypass mode is stateless) |

```cpp
void HttpClient::ResetMessageState() {
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    status_code_ = 0;
    headers_done_ = false;
}
```

This enables HTTP keep-alive and multiple responses on same connection.

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
    bool InitContext() {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) {
            downstream_->OnError({ErrorCode::TlsHandshakeFailed, "SSL_CTX_new failed"});
            return false;
        }
        if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
            downstream_->OnError({ErrorCode::TlsCertificateError, "Failed to load system CA"});
            return false;
        }
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        return true;
    }

    bool SetupVerification(const std::string& hostname) {
        if (SSL_set_tlsext_host_name(ssl_, hostname.c_str()) != 1) {
            downstream_->OnError({ErrorCode::TlsHandshakeFailed, "SNI setup failed"});
            return false;
        }
        if (SSL_set1_host(ssl_, hostname.c_str()) != 1) {
            downstream_->OnError({ErrorCode::TlsHandshakeFailed, "Hostname verification setup failed"});
            return false;
        }
        SSL_set_verify(ssl_, SSL_VERIFY_PEER, nullptr);
        return true;
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

On HTTP 4xx/5xx, calls `OnError()` with status and raw body (capped at 4KB to prevent
unbounded memory growth on large error responses).
On `on_message_complete`, calls `OnDone()`.

```cpp
static constexpr size_t kMaxErrorBodySize = 4096;

void HttpClient::OnBody(const char* data, size_t len) {
    if (status_code_ >= 400) {
        // Cap error body size
        size_t remaining = kMaxErrorBodySize - error_body_.size();
        error_body_.append(data, std::min(len, remaining));
    } else {
        // Normal body - forward to downstream
        // ...
    }
}
```

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
