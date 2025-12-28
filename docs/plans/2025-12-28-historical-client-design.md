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
    // Option 1: Consume immediately (ideal case)
    ssize_t n = socket_write(data.data(), data.size());
    // data destructs, memory returns to caller's pool

    // Option 2: Copy into local pool if buffering needed (partial write/EAGAIN)
    pending_writes_.push_back(
        std::pmr::vector<std::byte>(data.begin(), data.end(), &write_pool_)
    );
}
```

Since Write() data originates from downstream's pool, upstream cannot hold references
beyond the call without copying. Intermediate components pass through without buffering.

### TcpSocket Write Buffering

TcpSocket must handle partial writes and EAGAIN:

```cpp
class TcpSocket {
public:
    void Write(std::pmr::vector<std::byte> data) {
        if (!pending_writes_.empty()) {
            // Already have queued data - must queue this too (preserve order)
            pending_writes_.push_back(CopyToLocalPool(std::move(data)));
            return;
        }

        // Try immediate write (with EINTR retry)
        ssize_t n;
        do {
            n = ::write(fd_, data.data(), data.size());
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full - queue entire buffer
                pending_writes_.push_back(CopyToLocalPool(std::move(data)));
                WatchWritable();  // register EPOLLOUT
                return;
            }
            // Real error
            downstream_->OnError({ErrorCode::ConnectionFailed, strerror(errno)});
            RequestClose();
            return;
        }

        if (static_cast<size_t>(n) < data.size()) {
            // Partial write - queue remainder
            auto remainder = CopyToLocalPool(data, n);  // copy from offset n
            pending_writes_.push_back(std::move(remainder));
            WatchWritable();
        }
        // Full write - data destructs, returns to caller's pool
    }

private:
    std::pmr::vector<std::byte> CopyToLocalPool(std::pmr::vector<std::byte>& src, size_t offset = 0) {
        std::pmr::vector<std::byte> copy{&write_pool_};
        copy.assign(src.begin() + offset, src.end());
        return copy;
    }

    std::deque<std::pmr::vector<std::byte>> pending_writes_;
    std::pmr::unsynchronized_pool_resource write_pool_;
};
```

**Key points:**
- Immediate write attempted first (zero-copy fast path)
- On EAGAIN/partial: copy remainder into socket's own pool
- Preserve write order with queue
- Register EPOLLOUT to drain queue when socket becomes writable

### Static Dispatch

Hot path uses templates for static dispatch:

```cpp
// Virtual interface for backpressure (cold path)
class Suspendable {
public:
    virtual ~Suspendable() = default;
    virtual void Suspend() = 0;
    virtual void Resume() = 0;
    virtual void Close() = 0;
};

// Example component using CRTP for reentrancy safety
template<typename Downstream>
class ExampleComponent
    : public PipelineComponent<ExampleComponent<Downstream>>
    , public Suspendable {

    using Base = PipelineComponent<ExampleComponent<Downstream>>;
    friend Base;

public:
    // Hot path - static dispatch, inlined, ALL GUARDED
    void Read(std::pmr::vector<std::byte> data) {
        if (this->IsClosed()) return;  // early exit if closing
        typename Base::ProcessingGuard guard(*this);
        std::pmr::vector<std::byte> processed{&output_pool_};
        // process...
        downstream_->Read(std::move(processed));
    }

    // Shutdown signals - propagate once (terminal guard)
    void OnError(const Error& e) {
        if (this->IsFinalized()) return;
        this->SetFinalized();
        typename Base::ProcessingGuard guard(*this);
        downstream_->OnError(e);
    }

    void OnDone() {
        if (this->IsFinalized()) return;
        this->SetFinalized();
        typename Base::ProcessingGuard guard(*this);
        downstream_->OnDone();
    }

    // Data path - reject after close
    void Write(std::pmr::vector<std::byte> data) {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        upstream_->Write(std::move(data));
    }

    // Cold path - reject after close
    void Suspend() override {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        upstream_->Suspend();
    }
    void Resume() override {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        upstream_->Resume();
    }
    void Close() override { this->RequestClose(); }

    // Required by PipelineComponent
    void DisableWatchers() { /* no-op for non-I/O components */ }

private:
    void DoClose() {
        downstream_.reset();
        upstream_->Close();
    }

    Suspendable* upstream_;
    std::shared_ptr<Downstream> downstream_;
    std::pmr::unsynchronized_pool_resource output_pool_;
};
```

### Teardown Ordering

On `Close()` or EOF, strict ordering to prevent races:

```cpp
// TcpSocket uses RequestClose like other components
void TcpSocket::Close() {
    this->RequestClose();  // deferred via CRTP base
}

void TcpSocket::DisableWatchers() {
    if (event_) {
        event_->Remove();  // deregister from reactor
    }
}

void TcpSocket::DoClose() {
    downstream_.reset();    // release downstream chain (triggers their DoClose)
    if (fd_ >= 0) {
        ::close(fd_);       // close fd last
        fd_ = -1;
    }
}
```

Single-threaded reactor guarantees no callback between steps.

### Error-Triggered Teardown

When a component detects an error, it propagates error downstream, then requests close:

```cpp
void ZstdDecompressor::Read(std::pmr::vector<std::byte> data) {
    ProcessingGuard guard(*this);

    size_t ret = ZSTD_decompressStream(...);
    if (ZSTD_isError(ret)) {
        downstream_->OnError({ErrorCode::DecompressionError, ...});
        RequestClose();  // deferred until guard destructs
        return;
    }
    // ...
}
```

**Error flow:**
1. Component detects error
2. Calls `downstream_->OnError()` - propagates to application
3. Calls `RequestClose()` - sets pending flag
4. Guard destructor checks: if count == 0 && pending, executes close

`RequestClose()` is idempotent - safe to call multiple times from different error paths.

### Reentrancy Guard (Processing Count)

**Problem:** Calling `Close()` mid-call can destroy the current component:
```cpp
// UNSAFE: ZstdDecompressor::Read calls Close
// which destroys `this` while method still executing
upstream_->Close();  // UB - `this` destroyed mid-call
```

**Solution:** CRTP base class with deferred DoClose():

```cpp
// CRTP base - provides reentrancy-safe close
template<typename Derived>
class PipelineComponent {
public:
    PipelineComponent(Reactor& reactor) : reactor_(reactor) {}

    // RAII guard for public API entry
    class ProcessingGuard {
    public:
        explicit ProcessingGuard(PipelineComponent& c) : comp_(c) {
            ++comp_.processing_count_;
        }
        ~ProcessingGuard() {
            if (--comp_.processing_count_ == 0 && comp_.close_pending_) {
                comp_.ScheduleClose();
            }
        }
    private:
        PipelineComponent& comp_;
    };

    void RequestClose() {
        if (closed_) return;
        closed_ = true;  // mark closed immediately (reject further calls)

        // Derived class must disable I/O watchers here
        static_cast<Derived*>(this)->DisableWatchers();

        if (processing_count_ > 0) {
            close_pending_ = true;  // defer until processing complete
            return;
        }
        ScheduleClose();  // schedule on reactor
    }

    bool IsClosed() const { return closed_; }

    // Per-message terminal guard - prevents double OnDone/OnError within one message
    bool IsFinalized() const { return finalized_; }
    void SetFinalized() { finalized_ = true; }
    void ResetFinalized() { finalized_ = false; }  // call in ResetMessageState()

protected:
    // Virtual so TcpSocket can override with raw this capture
    virtual void ScheduleClose() {
        if (close_scheduled_) return;
        close_scheduled_ = true;
        close_pending_ = false;

        // Default: shared_from_this for shared-owned components
        auto self = static_cast<Derived*>(this)->shared_from_this();
        reactor_.Defer([self]() {
            static_cast<Derived*>(self.get())->DoClose();
        });
    }

    Reactor& reactor_;
    bool close_scheduled_ = false;  // single-threaded: no atomics needed

private:
    int processing_count_ = 0;
    bool close_pending_ = false;
    bool closed_ = false;
    bool finalized_ = false;

    // Note: All state is accessed from reactor thread only.
    // Single-threaded reactor model eliminates data races.
};
```

**Lifetime Invariant:**

Shared-owned components use `shared_from_this()` in `ScheduleClose()`:
```cpp
auto self = static_cast<Derived*>(this)->shared_from_this();
reactor_.Defer([self]() { self->DoClose(); });
```

This guarantees the component stays alive until `DoClose()` runs, even if upstream
calls `downstream_.reset()` before the deferred callback executes.

The ownership chain:
```
TcpSocket (stack/unique_ptr owned by app)
  └─ shared_ptr<TlsSocket>         ← upstream + deferred callback hold this
       └─ shared_ptr<HttpClient>   ← upstream + deferred callback hold this
            └─ ...
```

**TcpSocket (head) exception:**
Application must follow `Close()` → `Poll()` → destroy pattern. See below.

**Head TcpSocket destruction requirements:**

TcpSocket (head) is owned by the application, not via `shared_ptr`. To safely destroy it:

```cpp
// Option 1: Close and drain before destruction
tcp->Close();                    // initiates teardown, schedules Defer()
while (reactor.Poll(0) > 0) {}   // drain deferred queue
// Now safe to destroy tcp

// Option 2: Use unique_ptr and ensure reactor drains
{
    auto tcp = std::make_unique<TcpSocket>(reactor);
    // ... use tcp ...
    tcp->Close();
}
// tcp destroyed here - ONLY safe if reactor was drained

// Option 3: Shared ownership (if needed for complex scenarios)
auto tcp = std::make_shared<TcpSocket>(reactor);
// Now tcp survives until all deferred callbacks complete
```

**Rule**: Application must ensure all deferred callbacks complete before destroying head TcpSocket.

**Safe destruction patterns:**

```cpp
// Pattern 1: Single Poll() after Close() (simplest, usually sufficient)
tcp->Close();    // schedules DoClose() for this pipeline
reactor.Poll(0); // runs all pending deferred callbacks
// Safe: DoClose() has run, downstream chain released

// Pattern 2: Run until idle (if other activity possible)
tcp->Close();
reactor.Run();   // runs until Stop() called or no more events
// Safe after Run() returns

// Pattern 3: Let reactor outlive TcpSocket
{
    auto tcp = std::make_unique<TcpSocket>(reactor);
    tcp->Close();
}
// TcpSocket destroyed, but reactor still has DoClose() pending
reactor.Poll(0);  // DoClose() runs, accesses destroyed tcp - UAF!
// UNSAFE: Must Poll() before destroying tcp
```

**Why one Poll(0) is sufficient after Close():**
1. `Close()` → `DisableWatchers()` removes epoll registration (no more I/O events)
2. `Close()` → `ScheduleClose()` queues exactly one `DoClose()`
3. `DoClose()` releases downstream chain (which schedules their DoClose())
4. All DoClose() calls run in the same Poll() deferred drain loop
5. No strong `this` captures remain after (banned by rule)

**DisableWatchers() requirement:**

Each component must implement `DisableWatchers()` to stop receiving I/O events:

```cpp
// TcpSocket - has actual epoll registration
void DisableWatchers() {
    if (event_) {
        event_->Remove();  // deregister from epoll
    }
}

// TlsSocket, HttpClient, etc. - no direct I/O watchers
void DisableWatchers() {
    // No-op: these components don't own epoll registrations
    // They receive data via Read() from upstream
}
```

This ensures no I/O callbacks fire between `RequestClose()` and deferred `DoClose()`.

**Reactor callback cancellation rule:**

Any reactor callback that captures `this` must be one of:
1. **Epoll events** - cancelled by `Event::Remove()` in `DisableWatchers()`
2. **ScheduleClose() only** - the ONLY place that may capture strong `this` in `Reactor::Defer()`
3. **Timers** (if added) - must be cancelled in `DisableWatchers()`

**RULE: Strong `this` captures are BANNED outside `ScheduleClose()`.**

All other deferred work MUST use weak capture:

```cpp
// REQUIRED pattern for any deferred work (shared-owned components)
auto weak = weak_from_this();
reactor_.Defer([weak]() {
    if (auto self = weak.lock()) {
        self->DoWork();
    }
});

// BANNED - will cause UAF
reactor_.Defer([this]() {  // ❌ NEVER DO THIS
    this->DoWork();
});
```

**Rationale:** `ScheduleClose()` is the only deferred callback with guaranteed lifetime
(upstream holds `shared_ptr` until `DoClose()` runs). All other callbacks lack this guarantee.

**TcpSocket exception:**
TcpSocket (head) is not shared-owned, so cannot use `shared_from_this()` in `ScheduleClose()`.
TcpSocket overrides `ScheduleClose()` with raw `this` capture:

```cpp
class TcpSocket : public PipelineComponent<TcpSocket> {
protected:
    void ScheduleClose() override {
        if (close_scheduled_) return;
        close_scheduled_ = true;
        close_pending_ = false;
        reactor_.Defer([this]() { DoClose(); });  // raw capture OK for head
    }
};
```

This is safe because application follows destruction pattern: `Close()` → `Poll()` → destroy.
The Poll() drains all deferred callbacks including DoClose() before TcpSocket is destroyed.

**Shared-owned components requirement:**
All pipeline components except TcpSocket must inherit `std::enable_shared_from_this`:

```cpp
template<typename Downstream>
class TlsSocket
    : public PipelineComponent<TlsSocket<Downstream>>
    , public Suspendable
    , public std::enable_shared_from_this<TlsSocket<Downstream>> {  // required
    // ...
};
```

**Post-construction init pattern:**
`shared_from_this()` throws if called before the object is owned by `shared_ptr`.
Components must defer initialization that can fail until after construction:

```cpp
// WRONG - RequestClose() in constructor, shared_from_this not valid yet
TlsSocket(...) {
    if (!InitContext()) {
        RequestClose();  // THROWS: not owned by shared_ptr yet
    }
}

// CORRECT - two-phase init with factory
static std::shared_ptr<TlsSocket> Create(...) {
    auto self = std::make_shared<TlsSocket>(...);  // now owned by shared_ptr
    if (!self->Init()) {
        return nullptr;  // or throw
    }
    return self;
}

bool Init() {
    if (!InitContext()) {
        RequestClose();  // OK: shared_from_this works
        return false;
    }
    return true;
}
```

**Component inherits via CRTP:**

```cpp
template<typename Downstream>
class ZstdDecompressor
    : public PipelineComponent<ZstdDecompressor<Downstream>>
    , public Suspendable {

    using Base = PipelineComponent<ZstdDecompressor<Downstream>>;
    friend Base;  // allow CRTP access to DoClose

public:
    void Read(std::pmr::vector<std::byte> data) {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);

        size_t ret = ZSTD_decompressStream(...);
        if (ZSTD_isError(ret)) {
            downstream_->OnError({ErrorCode::DecompressionError, ...});
            this->RequestClose();
            return;
        }
        downstream_->Read(std::move(output));
    }

    // Shutdown signals - propagate once (terminal guard)
    void OnError(const Error& e) {
        if (this->IsFinalized()) return;
        this->SetFinalized();
        typename Base::ProcessingGuard guard(*this);
        downstream_->OnError(e);
    }

    void OnDone() {
        if (this->IsFinalized()) return;
        this->SetFinalized();
        typename Base::ProcessingGuard guard(*this);
        // flush remaining data, check frame complete
        downstream_->OnDone();
    }

    // Suspendable interface - reject after close
    void Suspend() override {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        upstream_->Suspend();
    }
    void Resume() override {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        upstream_->Resume();
    }
    void Close() override { this->RequestClose(); }

    void DisableWatchers() { /* no-op */ }

private:
    // Called by CRTP base when processing_count == 0
    void DoClose() {
        downstream_.reset();
        upstream_->Close();
    }

    Suspendable* upstream_ = nullptr;
    std::shared_ptr<Downstream> downstream_;
};
```

**Key properties:**
- Close only executes when processing_count_ == 0
- Nested calls (Read → OnError → app callback → Close) stay safe
- CRTP avoids virtual dispatch for DoClose()
- Works for both internal and external (app-triggered) Close calls
- Single-threaded model - no atomics needed

**DoClose() idempotency requirement:**

Each component's `DoClose()` must be safe to call multiple times or tolerate reentrancy:

```cpp
void DoClose() {
    if (downstream_) {
        downstream_.reset();  // safe: reset() is idempotent
    }
    if (upstream_) {
        upstream_->Close();   // safe: Close() checks closed_ flag
        upstream_ = nullptr;  // prevent double-call
    }
}
```

This handles edge cases where `upstream_->Close()` propagates back through the chain.

**IsClosed() early return pattern:**

Public methods check `IsClosed()`, but **OnDone/OnError always pass through** for protocol correctness:

```cpp
// Data methods - reject after close
void Read(std::pmr::vector<std::byte> data) {
    if (this->IsClosed()) return;  // reject new work
    typename Base::ProcessingGuard guard(*this);
    // ...
}

void Write(std::pmr::vector<std::byte> data) {
    if (this->IsClosed()) return;
    typename Base::ProcessingGuard guard(*this);
    // ...
}

// Shutdown signals - ALWAYS propagate, but only once (terminal guard)
void OnDone() {
    if (this->IsFinalized()) return;  // prevent double finalization
    this->SetFinalized();
    typename Base::ProcessingGuard guard(*this);
    // flush buffered data, finalize decompression, etc.
    downstream_->OnDone();
}

void OnError(const Error& e) {
    if (this->IsFinalized()) return;  // prevent double finalization
    this->SetFinalized();
    typename Base::ProcessingGuard guard(*this);
    downstream_->OnError(e);
}
```

This ensures:
1. No new data processing after `RequestClose()` is called
2. Shutdown signals (OnDone/OnError) reach application exactly once per message
3. Protocol finalization (zstd frame complete, HTTP trailers) happens correctly
4. ProcessingGuard still prevents close during propagation
5. Terminal guard prevents double-finalization within one message
6. `ResetFinalized()` in `ResetMessageState()` enables keep-alive multi-message flows

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
    if (this->IsFinalized()) return;  // terminal guard
    this->SetFinalized();
    typename Base::ProcessingGuard guard(*this);

    // Flush remaining decompressed data
    if (!buffered_.empty()) {
        downstream_->Read(std::move(buffered_));
    }
    // Check for incomplete frame
    if (!zstd_frame_complete_) {
        downstream_->OnError({ErrorCode::DecompressionError, "truncated"});
        this->RequestClose();  // error triggers close
        return;
    }
    downstream_->OnDone();

    // Reset for next message (keep-alive scenarios)
    ResetMessageState();  // includes ResetFinalized()
}
```

After `OnDone()`, next `Read()` starts new message.

### Reactor::Defer()

Reactor provides deferred callback queue for safe close:

```cpp
class Reactor {
public:
    void Defer(std::function<void()> fn) {
        deferred_.push_back(std::move(fn));
    }

    int Poll(int timeout_ms) {
        int n = epoll_wait(epoll_fd_, events_.data(), kMaxEvents, timeout_ms);
        // ... handle epoll events ...

        // Drain deferred queue after event processing
        while (!deferred_.empty()) {
            auto fn = std::move(deferred_.front());
            deferred_.pop_front();
            fn();
        }
        return n;
    }

private:
    std::deque<std::function<void()>> deferred_;
};
```

**Key properties:**
- **Never inline at call site**: `Defer()` only queues; callbacks run later in same `Poll()`
- Deferred callbacks run after all epoll events in current iteration
- Stack of the original caller is unwound before callback executes
- Single-threaded - no races

**Execution timing:**
```cpp
int Poll() {
    // 1. Process epoll events (may enqueue deferred work)
    for (auto& event : events) {
        callbacks_[event.fd](event.events);  // may call RequestClose()
    }
    // 2. Only AFTER all events: drain deferred queue
    while (!deferred_.empty()) {
        auto fn = std::move(deferred_.front());
        deferred_.pop_front();
        fn();  // DoClose() runs here, not during step 1
    }
}
```

**Key guarantee:** `DoClose()` never runs mid-call-stack. The caller of `RequestClose()` returns
before any deferred callback executes.

### Message Boundary Reset

Each component resets per-message state after `OnDone()`:

| Component | State to Reset |
|-----------|----------------|
| HttpClient | `ResetFinalized()`, `llhttp_init()`, status_code, headers_done, error_body_, message_state_=Idle |
| ZstdDecompressor | `ResetFinalized()`, `ZSTD_initDStream()`, buffered data |
| DbnParser | `ResetFinalized()`, partial record buffer |
| TlsSocket | No reset - passthrough only, doesn't know message boundaries |
| TcpSocket | No reset - only sends OnDone() on connection close |
| CramAuth | No reset needed (bypass mode is stateless) |

**Note:** Only components that originate or process message boundaries call `ResetMessageState()`.
TlsSocket and TcpSocket are stream-level; they pass through OnDone() but don't reset because:
- TlsSocket: TLS is a stream protocol, no message boundaries
- TcpSocket: OnDone() means connection close, no "next message"

```cpp
void HttpClient::ResetMessageState() {
    this->ResetFinalized();  // CRTP base - re-enable OnDone/OnError
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    status_code_ = 0;
    headers_done_ = false;
    error_body_.clear();
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
            RequestClose();
            return false;
        }
        if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
            downstream_->OnError({ErrorCode::TlsCertificateError, "Failed to load system CA"});
            RequestClose();
            return false;
        }
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        return true;
    }

    bool SetupVerification(const std::string& hostname) {
        if (SSL_set_tlsext_host_name(ssl_, hostname.c_str()) != 1) {
            downstream_->OnError({ErrorCode::TlsHandshakeFailed, "SNI setup failed"});
            RequestClose();
            return false;
        }
        if (SSL_set1_host(ssl_, hostname.c_str()) != 1) {
            downstream_->OnError({ErrorCode::TlsHandshakeFailed, "Hostname verification setup failed"});
            RequestClose();
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

**HTTP error handling (4xx/5xx):**
- On 4xx/5xx status, calls `OnError()` with status and raw body (capped at 4KB)
- HTTP errors are **terminal** - triggers `RequestClose()` after `OnError()`
- No keep-alive for error responses (simplifies error handling)

**HTTP success handling (2xx/3xx):**
- On `on_message_complete`, calls `downstream_->OnDone()`
- Followed by `ResetMessageState()` for keep-alive on next request

**HTTP message state machine:**
```cpp
enum class MessageState { Idle, InProgress, Complete };
MessageState message_state_ = MessageState::Idle;

// llhttp callbacks
int OnMessageBegin() { message_state_ = MessageState::InProgress; return 0; }
int OnMessageComplete() {
    message_state_ = MessageState::Complete;
    if (status_code_ >= 400) {
        downstream_->OnError({ErrorCode::HttpError, status_code_, error_body_});
        this->RequestClose();
    } else {
        downstream_->OnDone();
        ResetMessageState();  // sets message_state_ = Idle
    }
    return 0;
}

void OnDone() {  // from upstream (EOF)
    if (this->IsFinalized()) return;
    this->SetFinalized();
    typename Base::ProcessingGuard guard(*this);

    switch (message_state_) {
        case MessageState::Idle:
            // Normal keep-alive close, no error
            downstream_->OnDone();
            break;
        case MessageState::InProgress:
            // Truncated - EOF mid-message
            downstream_->OnError({ErrorCode::HttpParseError, "truncated"});
            this->RequestClose();
            break;
        case MessageState::Complete:
            // Already handled in OnMessageComplete
            break;
    }
}
```

This distinguishes:
- **Idle EOF**: Normal connection close after response complete (no error)
- **InProgress EOF**: Truncation error (message started but not finished)
- **Complete EOF**: Already handled by llhttp callback

**Double OnDone prevention:**
The `finalized_` terminal guard prevents double-done semantics:
- `OnMessageComplete` calls `downstream_->OnDone()` then `ResetMessageState()` (resets finalized_)
- If EOF arrives later, `OnDone()` checks `IsFinalized()` - won't fire again for same message
- For keep-alive, `ResetFinalized()` enables next message's OnDone

**Parser error handling:**
If llhttp encounters a parse error (malformed HTTP), it invokes `on_error` callback:
```cpp
int OnError(llhttp_t* parser, const char* reason) {
    downstream_->OnError({ErrorCode::HttpParseError, reason});
    RequestClose();
    return HPE_USER;  // stop parsing
}
```
This fires regardless of `message_state_` - parse errors are always terminal.

**HTTP close-on-error rationale (4xx/5xx):**
This design closes connection on HTTP errors because:
1. Historical API makes one request/response per connection
2. Simplifies error handling (no partial state after error)
3. Server may send `Connection: close` anyway on errors
For general HTTP/1.1 keep-alive clients, errors could allow continuation.

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
