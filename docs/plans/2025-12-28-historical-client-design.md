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

**Four downstream operations** (data flows toward application):
- `Read(vector)` - data chunk, ownership transferred
- `OnError(error)` - error, no more reads
- `OnDone()` - success/EOF, flush signal, no more reads for this message

**Four upstream operations** (control flows toward socket):
- `Write(vector)` - send data to socket (request bodies, auth)
- `Suspend()` - backpressure, stop sending data
- `Resume()` - resume sending data
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
            // Real error - use terminal guard to prevent duplicate signals
            if (!error_signaled_) {
                error_signaled_ = true;
                downstream_->OnError({ErrorCode::ConnectionFailed, strerror(errno)});
            }
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
    bool error_signaled_ = false;  // terminal guard for TcpSocket errors
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
```

### C++23 Interface Concepts

Formalize pipeline interfaces with concepts for compile-time contract checking:

```cpp
// Downstream interface - receives data flowing toward application
template<typename D>
concept Downstream = requires(D& d, std::pmr::vector<std::byte> data, const Error& e) {
    { d.Read(std::move(data)) } -> std::same_as<void>;
    { d.OnError(e) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

// Upstream interface - control flowing toward socket
template<typename U>
concept Upstream = requires(U& u, std::pmr::vector<std::byte> data) {
    { u.Write(std::move(data)) } -> std::same_as<void>;
    { u.Suspend() } -> std::same_as<void>;
    { u.Resume() } -> std::same_as<void>;
    { u.Close() } -> std::same_as<void>;
};

// Component that can be closed with deferred cleanup
template<typename C>
concept Closeable = requires(C& c) {
    { c.IsClosed() } -> std::same_as<bool>;
    { c.DisableWatchers() } -> std::same_as<void>;
    { c.DoClose() } -> std::same_as<void>;
};
```

Usage in component templates:

```cpp
template<Downstream D>
class HttpClient : public PipelineComponent<HttpClient<D>>, public Suspendable {
    std::shared_ptr<D> downstream_;  // D guaranteed to satisfy Downstream concept
};
```

Benefits:
- Compile-time documentation of interface contracts
- Clear error messages when interface not satisfied
- Enables constrained template parameters

```cpp
// Example component using C++23 patterns
template<Downstream D>  // concept-constrained template parameter
class ExampleComponent
    : public PipelineComponent<ExampleComponent<D>>
    , public Suspendable {

    using Base = PipelineComponent<ExampleComponent<D>>;
    friend Base;

public:
    // Hot path - TryGuard() combines closed check + guard creation
    void Read(std::pmr::vector<std::byte> data) {
        auto guard = this->TryGuard();
        if (!guard) return;  // closed, reject

        std::pmr::vector<std::byte> processed{&output_pool_};
        // process...
        downstream_->Read(std::move(processed));
    }

    // Terminal signals - use EmitError/EmitDone helpers
    void OnError(const Error& e) {
        this->EmitError(*downstream_, e);  // handles finalized check + guard
    }

    void OnDone() {
        this->EmitDone(*downstream_);  // handles finalized check + guard
    }

    // Data path - TryGuard pattern
    void Write(std::pmr::vector<std::byte> data) {
        auto guard = this->TryGuard();
        if (!guard) return;
        upstream_->Write(std::move(data));
    }

    // Cold path - TryGuard pattern
    void Suspend() override {
        auto guard = this->TryGuard();
        if (!guard) return;
        upstream_->Suspend();
    }
    void Resume() override {
        auto guard = this->TryGuard();
        if (!guard) return;
        upstream_->Resume();
    }
    void Close() override { this->RequestClose(); }

    // Required by Closeable concept
    void DisableWatchers() { /* no-op for non-I/O components */ }

private:
    void DoClose() {
        downstream_.reset();
        upstream_->Close();
    }

    Suspendable* upstream_;       // Upstream interface (virtual for cold path)
    std::shared_ptr<D> downstream_;  // D satisfies Downstream concept
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
// CRTP base - provides reentrancy-safe close with C++23 enhancements
template<typename Derived>
class PipelineComponent {
    // Compile-time check: Derived must satisfy Closeable concept
    static_assert(
        Closeable<Derived>,
        "Derived must implement IsClosed(), DisableWatchers(), DoClose()"
    );
    static_assert(
        std::is_base_of_v<std::enable_shared_from_this<Derived>, Derived> ||
        requires { &Derived::ScheduleClose; },
        "Derived must inherit enable_shared_from_this or override ScheduleClose()"
    );

public:
    PipelineComponent(Reactor& reactor) : reactor_(reactor) {}

    // RAII guard for reentrancy-safe processing
    // Move-safe via active flag to prevent double-decrement
    class ProcessingGuard {
    public:
        explicit ProcessingGuard(PipelineComponent& c) : comp_(&c), active_(true) {
            ++comp_->processing_count_;
        }
        ~ProcessingGuard() {
            if (active_) {
                if (--comp_->processing_count_ == 0 && comp_->close_pending_) {
                    comp_->ScheduleClose();
                }
            }
        }
        ProcessingGuard(const ProcessingGuard&) = delete;
        ProcessingGuard& operator=(const ProcessingGuard&) = delete;
        ProcessingGuard(ProcessingGuard&& other) noexcept
            : comp_(other.comp_), active_(other.active_) {
            other.active_ = false;  // disable source guard
        }
        ProcessingGuard& operator=(ProcessingGuard&&) = delete;
    private:
        PipelineComponent* comp_;
        bool active_;
    };

    // C++23 TryGuard pattern - combines closed check with guard creation
    // Returns nullopt if closed, otherwise returns guard that must be held
    [[nodiscard]] std::optional<ProcessingGuard> TryGuard() {
        if (closed_) return std::nullopt;
        return ProcessingGuard{*this};
    }

    // C++23 deducing this - eliminates static_cast in derived class calls
    void RequestClose(this auto&& self) {
        if (self.closed_) return;
        self.closed_ = true;

        self.DisableWatchers();  // direct call via deducing this

        if (self.processing_count_ > 0) {
            self.close_pending_ = true;
            return;
        }
        self.ScheduleClose();
    }

    bool IsClosed() const { return closed_; }

    // Per-message terminal guard
    bool IsFinalized() const { return finalized_; }
    void SetFinalized() { finalized_ = true; }
    void ResetFinalized() { finalized_ = false; }

    // Terminal emission with concept constraint (C++23)
    template<Downstream D>
    void EmitError(D& downstream, const Error& e) {
        if (finalized_) return;
        finalized_ = true;
        ProcessingGuard guard(*this);
        downstream.OnError(e);
    }

    template<Downstream D>
    void EmitDone(D& downstream) {
        if (finalized_) return;
        finalized_ = true;
        ProcessingGuard guard(*this);
        downstream.OnDone();
    }

protected:
    // Virtual for TcpSocket override - cannot use deducing this with virtual
    // Note: deducing this makes function a template, incompatible with virtual
    virtual void ScheduleClose() {
        if (close_scheduled_) return;
        close_scheduled_ = true;
        close_pending_ = false;

        // CRTP static_cast required for virtual function
        auto self = static_cast<Derived*>(this)->shared_from_this();
        reactor_.Defer([self]() {
            static_cast<Derived*>(self.get())->DoClose();
        });
    }

    Reactor& reactor_;
    bool close_scheduled_ = false;

private:
    int processing_count_ = 0;
    bool close_pending_ = false;
    bool closed_ = false;
    bool finalized_ = false;
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

**DoClose() idempotency and ordering:**

Each component's `DoClose()` must be safe to call multiple times:

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

**Why upstream_ is still valid when DoClose() runs:**
1. Upstream's `DoClose()` calls `downstream_.reset()` first
2. But downstream's `ScheduleClose()` captured `shared_from_this()`
3. So downstream survives until its own `DoClose()` callback runs
4. When downstream's `DoClose()` runs, it calls `upstream_->Close()`
5. Upstream is still alive (TcpSocket on stack, or its own shared_ptr not yet released)

The `shared_from_this()` capture ensures each component's `DoClose()` runs while upstream is alive.

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
| HttpClient | `ResetFinalized()`, `llhttp_init()`, status_code, headers_done, error_body_, message_state_=Idle, request_pending_=false |
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
    message_state_ = MessageState::Idle;  // ready for next message
    request_pending_ = false;              // no request in flight
}
```

This enables HTTP keep-alive and multiple responses on same connection. The `message_state_` and `request_pending_` resets are critical - without them, the pipelining guard would reject subsequent requests.

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

    // Backpressure control - modify epoll registration
    void Suspend();
    void Resume();

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
    bool suspended_ = false;  // tracks EPOLLIN state
};
```

**Suspend/Resume implementation:**

TcpSocket implements backpressure by modifying epoll registration:

```cpp
void TcpSocket::Suspend() {
    if (suspended_ || fd_ < 0) return;
    suspended_ = true;
    // Remove EPOLLIN - stop receiving read events
    uint32_t events = EPOLLOUT;  // keep EPOLLOUT if write pending
    if (!pending_writes_.empty()) {
        reactor_.Modify(fd_, events);
    } else {
        reactor_.Modify(fd_, 0);  // no events
    }
}

void TcpSocket::Resume() {
    if (!suspended_ || fd_ < 0) return;
    suspended_ = false;
    // Re-add EPOLLIN
    uint32_t events = EPOLLIN;
    if (!pending_writes_.empty()) {
        events |= EPOLLOUT;
    }
    reactor_.Modify(fd_, events);
}
```

**Backpressure flow:**
1. Downstream calls `Suspend()` when overwhelmed
2. TcpSocket removes EPOLLIN from epoll
3. Kernel buffers incoming data (up to socket receive buffer)
4. TCP flow control eventually slows sender
5. Downstream calls `Resume()` when ready
6. TcpSocket re-adds EPOLLIN, receives buffered data

### TlsSocket

OpenSSL wrapper with async BIO:

```cpp
template<typename Downstream>
class TlsSocket
    : public PipelineComponent<TlsSocket<Downstream>>
    , public Suspendable
    , public std::enable_shared_from_this<TlsSocket<Downstream>> {

    using Base = PipelineComponent<TlsSocket<Downstream>>;
    friend Base;

public:
    // Factory - use instead of constructor for proper init
    static std::shared_ptr<TlsSocket> Create(
            Reactor& reactor,
            std::shared_ptr<Downstream> downstream,
            const std::string& hostname) {
        auto self = std::make_shared<TlsSocket>(reactor, std::move(downstream));
        if (!self->Init(hostname)) {
            return nullptr;  // caller should handle null (init failed)
        }
        return self;
    }

    // Constructor: basic member init only, no failures possible
    TlsSocket(Reactor& reactor, std::shared_ptr<Downstream> downstream)
        : Base(reactor)
        , downstream_(std::move(downstream)) {}

    ~TlsSocket();

    void SetUpstream(Suspendable* upstream) { upstream_ = upstream; }

    // From upstream
    void Read(std::pmr::vector<std::byte> ciphertext);
    void OnError(const Error& e);
    void OnDone();

    // From downstream
    void Write(std::pmr::vector<std::byte> plaintext);
    void Suspend() override;
    void Resume() override;
    void Close() override;

    void DisableWatchers() { /* no-op: no direct epoll registration */ }

private:
    // Called by factory after shared_ptr exists
    bool Init(const std::string& hostname) {
        if (!InitContext()) return false;
        if (!InitSsl()) return false;
        if (!SetupVerification(hostname)) return false;
        return true;
    }

    bool InitContext() {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) {
            downstream_->OnError({ErrorCode::TlsHandshakeFailed, "SSL_CTX_new failed"});
            this->RequestClose();  // OK: shared_ptr exists from factory
            return false;
        }
        if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
            downstream_->OnError({ErrorCode::TlsCertificateError, "Failed to load system CA"});
            this->RequestClose();
            return false;
        }
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        return true;
    }

    bool InitSsl() {
        ssl_ = SSL_new(ctx_);
        if (!ssl_) {
            downstream_->OnError({ErrorCode::TlsHandshakeFailed, "SSL_new failed"});
            this->RequestClose();
            return false;
        }
        rbio_ = BIO_new(BIO_s_mem());
        wbio_ = BIO_new(BIO_s_mem());
        if (!rbio_ || !wbio_) {
            downstream_->OnError({ErrorCode::TlsHandshakeFailed, "BIO_new failed"});
            this->RequestClose();
            return false;
        }
        SSL_set_bio(ssl_, rbio_, wbio_);  // transfers ownership
        SSL_set_connect_state(ssl_);
        return true;
    }

    bool SetupVerification(const std::string& hostname) {
        if (SSL_set_tlsext_host_name(ssl_, hostname.c_str()) != 1) {
            downstream_->OnError({ErrorCode::TlsHandshakeFailed, "SNI setup failed"});
            this->RequestClose();
            return false;
        }
        if (SSL_set1_host(ssl_, hostname.c_str()) != 1) {
            downstream_->OnError({ErrorCode::TlsHandshakeFailed, "Hostname verification setup failed"});
            this->RequestClose();
            return false;
        }
        SSL_set_verify(ssl_, SSL_VERIFY_PEER, nullptr);
        return true;
    }

    void DoClose() {
        downstream_.reset();
        if (upstream_) {
            upstream_->Close();
            upstream_ = nullptr;
        }
    }

    Suspendable* upstream_ = nullptr;
    std::shared_ptr<Downstream> downstream_;

    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;  // owned by ssl_ after SSL_set_bio
    BIO* wbio_ = nullptr;  // owned by ssl_ after SSL_set_bio

    std::pmr::unsynchronized_pool_resource decrypt_pool_;
    std::pmr::unsynchronized_pool_resource encrypt_pool_;
};
```

**Factory pattern rationale:**
- Constructor does basic member init only (cannot fail)
- `Init()` called by factory after `make_shared` (shared_ptr exists)
- `RequestClose()` in `Init()` is safe: `shared_from_this()` works
- Factory returns `nullptr` on init failure (caller handles)

Uses memory BIOs for async integration with Reactor.

**Backpressure handling (TlsSocket):**

TlsSocket must buffer plaintext when downstream suspends:

```cpp
template<typename Downstream>
class TlsSocket : ... {
private:
    bool suspended_ = false;
    std::pmr::vector<std::byte> pending_plaintext_;  // decrypted data when paused
    std::pmr::unsynchronized_pool_resource plaintext_pool_;

    static constexpr size_t kMaxBufferedPlaintext = 16 * 1024 * 1024;  // 16 MB

public:
    void Suspend() override {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        suspended_ = true;
        upstream_->Suspend();
    }

    void Resume() override {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        suspended_ = false;
        upstream_->Resume();

        // Deliver buffered plaintext
        if (!pending_plaintext_.empty()) {
            auto data = std::move(pending_plaintext_);
            pending_plaintext_ = std::pmr::vector<std::byte>{&plaintext_pool_};
            downstream_->Read(std::move(data));
        }
    }

    void Read(std::pmr::vector<std::byte> ciphertext) {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);

        // Feed ciphertext to OpenSSL
        int bio_ret = BIO_write(rbio_, ciphertext.data(), ciphertext.size());
        if (bio_ret <= 0) {
            this->EmitError(*downstream_, {ErrorCode::TlsHandshakeFailed, "BIO_write failed"});
            this->RequestClose();
            return;
        }

        // Drive handshake if not complete
        if (!SSL_is_init_finished(ssl_)) {
            int hs_ret = SSL_do_handshake(ssl_);
            FlushWbio();  // send handshake messages
            if (hs_ret <= 0) {
                int err = SSL_get_error(ssl_, hs_ret);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    return;  // need more data
                } else {
                    this->EmitError(*downstream_, {ErrorCode::TlsHandshakeFailed, "Handshake failed"});
                    this->RequestClose();
                    return;
                }
            }
            // Handshake complete - fall through to read any application data
        }

        // Decrypt available data
        while (true) {
            std::pmr::vector<std::byte> plaintext{4096, &decrypt_pool_};
            int n = SSL_read(ssl_, plaintext.data(), plaintext.size());

            if (n > 0) {
                plaintext.resize(n);
                if (suspended_) {
                    // Buffer when suspended
                    if (pending_plaintext_.size() + n > kMaxBufferedPlaintext) {
                        this->EmitError(*downstream_, {ErrorCode::BufferOverflow, "TLS buffer exceeded"});
                        this->RequestClose();
                        return;
                    }
                    pending_plaintext_.insert(pending_plaintext_.end(),
                        plaintext.begin(), plaintext.end());
                } else {
                    downstream_->Read(std::move(plaintext));
                    // Check if downstream suspended us during Read()
                    if (suspended_) continue;  // buffer remaining in next iteration
                }
            } else {
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_READ) {
                    break;  // need more ciphertext
                } else if (err == SSL_ERROR_WANT_WRITE) {
                    // TLS needs to send data (renegotiation, key update, alert)
                    FlushWbio();
                    break;  // will retry SSL_read on next Read() call
                } else if (err == SSL_ERROR_ZERO_RETURN) {
                    // TLS close_notify - propagate to downstream
                    if (suspended_) {
                        // Defer until Resume() - don't drop buffered data
                        done_pending_ = true;
                        return;
                    }
                    FinalizeCloseNotify();
                    return;
                } else {
                    this->EmitError(*downstream_, {ErrorCode::TlsHandshakeFailed, "SSL_read failed"});
                    this->RequestClose();
                    return;
                }
            }
        }

        // Always flush any pending outbound TLS data (alerts, key updates)
        FlushWbio();
    }
};
```

**Key points:**
- `suspended_` flag checked during SSL_read loop
- Plaintext buffered in `pending_plaintext_` when suspended
- 16 MB cap prevents unbounded growth
- On Resume(), buffered plaintext delivered before new ciphertext

**Close notify with backpressure:**

```cpp
bool done_pending_ = false;  // TLS close_notify received while suspended

void FinalizeCloseNotify() {
    // Deliver any buffered plaintext
    if (!pending_plaintext_.empty()) {
        auto data = std::move(pending_plaintext_);
        pending_plaintext_ = std::pmr::vector<std::byte>{&plaintext_pool_};
        downstream_->Read(std::move(data));
        if (suspended_) {
            done_pending_ = true;  // got suspended during delivery
            return;
        }
    }
    done_pending_ = false;
    this->EmitDone(*downstream_);
    this->RequestClose();
}

void Resume() override {
    if (this->IsClosed()) return;
    typename Base::ProcessingGuard guard(*this);
    suspended_ = false;
    upstream_->Resume();

    // Deliver buffered plaintext
    if (!pending_plaintext_.empty()) {
        auto data = std::move(pending_plaintext_);
        pending_plaintext_ = std::pmr::vector<std::byte>{&plaintext_pool_};
        downstream_->Read(std::move(data));
    }

    // Handle deferred close_notify
    if (done_pending_ && !suspended_) {
        FinalizeCloseNotify();
    }
}
```

**TLS output flushing (FlushWbio):**

After any SSL operation, flush pending ciphertext to upstream:

```cpp
void FlushWbio() {
    char buf[4096];
    int pending;
    while ((pending = BIO_pending(wbio_)) > 0) {
        int n = BIO_read(wbio_, buf, std::min(pending, (int)sizeof(buf)));
        if (n > 0) {
            std::pmr::vector<std::byte> ciphertext{&encrypt_pool_};
            ciphertext.assign(reinterpret_cast<std::byte*>(buf),
                              reinterpret_cast<std::byte*>(buf + n));
            upstream_->Write(std::move(ciphertext));
        }
    }
}
```

**TLS shutdown (Close):**

Send `close_notify` before closing transport:

```cpp
void TlsSocket::DoClose() {
    // Send close_notify if connected
    if (ssl_ && SSL_is_init_finished(ssl_)) {
        int ret = SSL_shutdown(ssl_);
        FlushWbio();  // send close_notify
        // Note: we don't wait for peer's close_notify (half-close)
        // Full bidirectional shutdown would require keeping socket open
    }

    // Clean up OpenSSL resources
    if (ssl_) {
        SSL_free(ssl_);  // also frees rbio_/wbio_
        ssl_ = nullptr;
    }
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }

    downstream_.reset();
    if (upstream_) {
        upstream_->Close();
        upstream_ = nullptr;
    }
}
```

**Handshake initiation:**

After socket connects, initiate TLS handshake:

```cpp
void TlsSocket::StartHandshake() {
    int ret = SSL_do_handshake(ssl_);
    FlushWbio();  // send ClientHello
    if (ret <= 0) {
        int err = SSL_get_error(ssl_, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            this->EmitError(*downstream_, {ErrorCode::TlsHandshakeFailed, "Initial handshake failed"});
            this->RequestClose();
        }
        // WANT_READ/WANT_WRITE: wait for Read() with server response
    }
}
```

**TLS Write path:**

Downstream calls Write() to send plaintext (HTTP requests):

```cpp
void TlsSocket::Write(std::pmr::vector<std::byte> plaintext) {
    if (this->IsClosed()) return;
    typename Base::ProcessingGuard guard(*this);

    // Preserve write order - if pending_write_ has data, append to it
    if (!pending_write_.empty() || !SSL_is_init_finished(ssl_)) {
        // Buffer for later - handshake in progress or previous write incomplete
        if (pending_write_.size() + plaintext.size() > kMaxBufferedPlaintext) {
            this->EmitError(*downstream_, {ErrorCode::BufferOverflow, "TLS write buffer exceeded"});
            this->RequestClose();
            return;
        }
        pending_write_.insert(pending_write_.end(), plaintext.begin(), plaintext.end());
        return;
    }

    WriteInternal(plaintext);
}

void TlsSocket::WriteInternal(std::span<const std::byte> plaintext) {
    int n = SSL_write(ssl_, plaintext.data(), plaintext.size());
    if (n <= 0) {
        int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_READ) {
            // Need to read before writing (renegotiation)
            // Buffer remaining plaintext
            pending_write_.assign(plaintext.begin(), plaintext.end());
            // Will retry after next Read() call completes
        } else if (err == SSL_ERROR_WANT_WRITE) {
            // BIO is full - flush and retry
            FlushWbio();
            // Buffer remaining for retry
            pending_write_.assign(plaintext.begin(), plaintext.end());
        } else {
            this->EmitError(*downstream_, {ErrorCode::TlsHandshakeFailed, "SSL_write failed"});
            this->RequestClose();
        }
        return;
    }

    FlushWbio();  // send encrypted data to socket

    if (static_cast<size_t>(n) < plaintext.size()) {
        // Partial write - buffer remainder
        pending_write_.assign(plaintext.begin() + n, plaintext.end());
    }
}

// In Read(), after successful handshake or data processing, drain pending writes:
if (!pending_write_.empty() && SSL_is_init_finished(ssl_)) {
    auto data = std::move(pending_write_);
    pending_write_ = std::pmr::vector<std::byte>{&plaintext_pool_};
    WriteInternal(data);
}

private:
    std::pmr::vector<std::byte> pending_write_;  // buffered plaintext awaiting send
```

**Write path key points:**
- Writes gated on `SSL_is_init_finished()` - buffered during handshake
- `SSL_ERROR_WANT_READ` buffers data for retry after Read()
- `SSL_ERROR_WANT_WRITE` flushes wbio and buffers for retry
- Pending writes drained after handshake completes or after each Read()

### HttpClient

HTTP/1.1 with llhttp:

```cpp
template<typename Downstream>
class HttpClient
    : public PipelineComponent<HttpClient<Downstream>>
    , public Suspendable
    , public std::enable_shared_from_this<HttpClient<Downstream>> {

    using Base = PipelineComponent<HttpClient<Downstream>>;
    friend Base;

public:
    HttpClient(Reactor& reactor, std::shared_ptr<Downstream> downstream);
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

    void DisableWatchers() { /* no-op */ }

    // Send HTTP GET request
    void Request(std::string_view host,
                 std::string_view path,
                 std::string_view auth_header);

private:
    void DoClose() {
        downstream_.reset();
        if (upstream_) {
            upstream_->Close();
            upstream_ = nullptr;
        }
    }

    static int OnBody(llhttp_t* parser, const char* data, size_t len);
    static int OnMessageComplete(llhttp_t* parser);

    Suspendable* upstream_ = nullptr;
    std::shared_ptr<Downstream> downstream_;

    llhttp_t parser_;
    llhttp_settings_t settings_;
    int status_code_ = 0;

    std::pmr::unsynchronized_pool_resource write_pool_;
    std::pmr::unsynchronized_pool_resource input_pool_;
};
```

**HTTP error handling (4xx/5xx):**
- On 4xx/5xx status, calls `OnError()` with status and raw body (capped at 4KB)
- HTTP errors are **terminal** - triggers `RequestClose()` after `OnError()`
- No keep-alive for error responses (simplifies error handling)

**HTTP success handling (2xx only):**
- On `on_message_complete` with 2xx status, calls `downstream_->OnDone()`
- Followed by `ResetMessageState()` for keep-alive on next request
- **3xx treated as error**: Redirects not supported; client expects payload at requested URL

**HTTP message state machine:**
```cpp
enum class MessageState { Idle, InProgress, Complete };
MessageState message_state_ = MessageState::Idle;
bool request_pending_ = false;  // true between Request() and on_message_begin

void Request(...) {
    // Guard against pipelining - reject if request already in flight
    if (request_pending_ || message_state_ != MessageState::Idle) {
        this->EmitError(*downstream_, {ErrorCode::InvalidState, "Request already in progress"});
        this->RequestClose();
        return;
    }

    // ... format and send HTTP request ...
    upstream_->Write(std::move(request_bytes));
    request_pending_ = true;  // waiting for response
}

// llhttp callbacks
int OnMessageBegin() {
    // Validate that a request is in flight
    if (!request_pending_ || message_state_ != MessageState::Idle) {
        // Unsolicited response - protocol error
        unsolicited_response_ = true;  // set flag for ProcessInput to handle
        llhttp_pause(&parser_);
        return 0;
    }
    message_state_ = MessageState::InProgress;
    request_pending_ = false;  // response started
    return 0;
}

private:
    bool unsolicited_response_ = false;  // set in OnMessageBegin callback

int OnMessageComplete() {
    message_state_ = MessageState::Complete;
    if (status_code_ >= 300) {
        // 3xx redirects and 4xx/5xx errors all treated as HTTP errors
        // Redirects not supported - client expects payload at requested URL
        this->EmitError(*downstream_, {ErrorCode::HttpError, status_code_, error_body_});
        this->RequestClose();
    } else {
        // 2xx success
        this->EmitDone(*downstream_);
        ResetMessageState();  // sets message_state_ = Idle, ResetFinalized()
    }
    return 0;
}

void OnDone() {  // from upstream (EOF)
    if (this->IsFinalized()) return;
    // Note: Do NOT set finalized_ here - llhttp_finish may trigger OnMessageComplete
    typename Base::ProcessingGuard guard(*this);

    switch (message_state_) {
        case MessageState::Idle:
            if (request_pending_) {
                // Request sent, but no response received - connection closed unexpectedly
                this->EmitError(*downstream_, {ErrorCode::ConnectionClosed, "Connection closed before response"});
                this->RequestClose();
            } else {
                // Connection closed between messages (keep-alive close)
                // No downstream OnDone - message already completed via OnMessageComplete
                this->SetFinalized();  // finalize after we know no more signals needed
            }
            break;
        case MessageState::InProgress: {
            // EOF while message in progress - try to finalize
            // llhttp_finish() handles close-delimited responses (HTTP/1.0, Connection: close)
            llhttp_errno_t err = llhttp_finish(&parser_);
            if (err == HPE_OK) {
                // Parser accepted EOF as valid end-of-message
                // Check if OnMessageComplete callback was triggered
                if (!this->IsFinalized()) {
                    // llhttp_finish() returned OK but didn't trigger OnMessageComplete
                    // This can happen if there was no pending body data
                    // Manually finalize for close-delimited responses
                    this->EmitDone(*downstream_);
                    ResetMessageState();
                }
                // Otherwise finalized_ was set by OnMessageComplete via EmitDone
            } else {
                // Truncated - parser rejected EOF
                this->EmitError(*downstream_, {ErrorCode::HttpParseError, llhttp_errno_name(err)});
                this->RequestClose();
            }
            break;
        }
        case MessageState::Complete:
            // Already handled in OnMessageComplete
            this->SetFinalized();  // ensure finalized for any further EOF
            break;
    }
}
```

This distinguishes:
- **Idle EOF with request_pending_**: Connection closed before response - `ConnectionClosed` error
- **Idle EOF without request_pending_**: Keep-alive close after response complete - silent close
- **InProgress EOF**: Calls `llhttp_finish()` to handle close-delimited responses; truncation error only if parser rejects EOF
- **Complete EOF**: Already handled by OnMessageComplete

**Close-delimited response handling:**
HTTP/1.0 and HTTP/1.1 with `Connection: close` may use EOF as message terminator.
`llhttp_finish()` tells the parser EOF arrived.

llhttp behavior on close-delimited responses:
- Response has no `Content-Length` or `Transfer-Encoding: chunked`
- Parser is in "consume until EOF" mode
- `llhttp_finish()` triggers `on_message_complete` callback
- Our `OnMessageComplete` callback fires, calling `downstream_->OnDone()`

If llhttp_finish() returns `HPE_OK` without calling `on_message_complete` (no pending data),
the response was empty or already complete - no downstream signal needed.

**Double OnDone prevention:**
Each message gets exactly one terminal signal:
- `OnMessageComplete` calls `downstream_->OnDone()` for success, or `OnError()` for 4xx/5xx
- Later EOF sees `Idle` state and does nothing (message already complete)
- `finalized_` prevents duplicate signals within same message boundary
- `ResetFinalized()` in `ResetMessageState()` enables next message (keep-alive only)

**Parser error handling:**
If llhttp encounters a parse error (malformed HTTP), it invokes `on_error` callback:
```cpp
int OnParserError(llhttp_t* parser, const char* reason) {
    this->EmitError(*downstream_, {ErrorCode::HttpParseError, reason});
    RequestClose();
    return HPE_USER;  // stop parsing
}
```
Uses `EmitError` which sets `finalized_` to ensure single error signal even if EOF follows.

**HTTP close-on-error rationale (4xx/5xx):**
This design closes connection on HTTP errors because:
1. Historical API makes one request/response per connection
2. Simplifies error handling (no partial state after error)
3. Server may send `Connection: close` anyway on errors
For general HTTP/1.1 keep-alive clients, errors could allow continuation.

**No HTTP pipelining:**
This client does not support HTTP pipelining (multiple requests before responses).
- One request sent, one response received, then optionally another request
- `ResetMessageState()` is safe because it's called after response complete
- `llhttp` reset happens before next request, not during parsing
- Historical API uses single request/response anyway

**Backpressure handling (HttpClient):**

When downstream calls `Suspend()`, HttpClient must stop delivering data. llhttp callbacks fire synchronously during `llhttp_execute()`, so we use llhttp's pause mechanism:

```cpp
template<typename Downstream>
class HttpClient : ... {
private:
    bool suspended_ = false;
    std::pmr::vector<std::byte> pending_input_;  // unconsumed input when paused

public:
    void Suspend() override {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        suspended_ = true;
        upstream_->Suspend();
    }

    void Resume() override {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        suspended_ = false;
        upstream_->Resume();

        // Process buffered input
        if (!pending_input_.empty()) {
            auto input = std::move(pending_input_);
            pending_input_ = std::pmr::vector<std::byte>{&input_pool_};
            ProcessInput(std::move(input));
        }
    }

    void Read(std::pmr::vector<std::byte> data) {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        ProcessInput(std::move(data));
    }

private:
    void ProcessInput(std::pmr::vector<std::byte> data) {
        const char* input = reinterpret_cast<const char*>(data.data());
        size_t input_len = data.size();

        // Track bytes consumed via parser's error_pos (reliable for HPE_PAUSED)
        llhttp_errno_t err = llhttp_execute(&parser_, input, input_len);

        if (err == HPE_PAUSED) {
            // Downstream called Suspend() during callback
            // Compute consumed bytes from error_pos (points to pause location)
            const char* pause_pos = llhttp_get_error_pos(&parser_);
            size_t consumed = 0;
            if (pause_pos && pause_pos >= input && pause_pos <= input + input_len) {
                consumed = pause_pos - input;
            } else {
                // Fallback: buffer entire input to avoid data loss
                // This shouldn't happen with llhttp's pause, but be safe
                consumed = 0;
            }

            // Append unconsumed portion (may already have buffered data)
            if (consumed < input_len) {
                size_t remaining = input_len - consumed;
                // Check buffer limit
                if (pending_input_.size() + remaining > kMaxBufferedInput) {
                    this->EmitError(*downstream_, {ErrorCode::BufferOverflow, "HTTP input buffer exceeded"});
                    this->RequestClose();
                    return;
                }
                pending_input_.insert(pending_input_.end(),
                    data.begin() + consumed, data.end());
            }
            llhttp_resume(&parser_);  // clear pause state for next Resume()
        } else if (err != HPE_OK) {
            this->EmitError(*downstream_, {ErrorCode::HttpParseError, llhttp_errno_name(err)});
            this->RequestClose();
        }
    }

    static constexpr size_t kMaxBufferedInput = 16 * 1024 * 1024;  // 16 MB

    // In on_body callback:
    static int OnBody(llhttp_t* parser, const char* data, size_t len) {
        auto* self = static_cast<HttpClient*>(parser->data);

        // Error responses (3xx/4xx/5xx) - capture body for error message, don't stream
        if (self->status_code_ >= 300) {
            size_t remaining = kMaxErrorBodySize - self->error_body_.size();
            self->error_body_.append(data, std::min(len, remaining));
            return 0;  // don't stream error bodies downstream
        }

        // Normal response (2xx) - stream to downstream
        if (self->suspended_) {
            // Enforce buffer limit before appending
            if (self->pending_body_.size() + len > kMaxBufferedBody) {
                // Can't emit error from callback - set flag and pause
                self->buffer_overflow_ = true;
                llhttp_pause(parser);
                return 0;  // return 0 to avoid parse error; check flag after execute
            }
            // Buffer body bytes BEFORE pausing - llhttp already consumed them
            self->pending_body_.insert(self->pending_body_.end(),
                reinterpret_cast<const std::byte*>(data),
                reinterpret_cast<const std::byte*>(data + len));
            llhttp_pause(parser);
            return 0;  // will resume parsing later
        }

        // Forward body chunk to downstream
        std::pmr::vector<std::byte> chunk{&self->body_pool_};
        chunk.assign(reinterpret_cast<const std::byte*>(data),
                     reinterpret_cast<const std::byte*>(data + len));
        self->downstream_->Read(std::move(chunk));

        // Check if downstream suspended us during Read()
        if (self->suspended_) {
            llhttp_pause(parser);  // stop parsing until Resume()
        }
        return 0;
    }

private:
    static constexpr size_t kMaxBufferedBody = 16 * 1024 * 1024;  // 16 MB
    std::pmr::vector<std::byte> pending_body_;  // buffered body when paused (2xx only)
    std::pmr::unsynchronized_pool_resource body_pool_;
    bool buffer_overflow_ = false;  // set in callback, handled after execute
};

// In ProcessInput, check flags in order BEFORE pause handling:
void ProcessInput(...) {
    llhttp_errno_t err = llhttp_execute(&parser_, input, input_len);

    // 1. Check buffer overflow first (set in OnBody callback)
    if (buffer_overflow_) {
        buffer_overflow_ = false;
        this->EmitError(*downstream_, {ErrorCode::BufferOverflow, "HTTP body buffer exceeded"});
        this->RequestClose();
        return;
    }

    // 2. Check unsolicited response (set in OnMessageBegin callback)
    if (unsolicited_response_) {
        unsolicited_response_ = false;
        this->EmitError(*downstream_, {ErrorCode::InvalidState, "Unsolicited HTTP response"});
        this->RequestClose();
        return;
    }

    // 3. Handle pause (normal backpressure case)
    if (err == HPE_PAUSED) {
        // ... existing pause handling ...
    } else if (err != HPE_OK) {
        this->EmitError(*downstream_, {ErrorCode::HttpParseError, llhttp_errno_name(err)});
        this->RequestClose();
    }
}

// In Resume(), drain buffers BEFORE resuming upstream to preserve order:
void Resume() {
    if (this->IsClosed()) return;
    typename Base::ProcessingGuard guard(*this);
    suspended_ = false;

    // Deliver buffered body chunks first
    if (!pending_body_.empty()) {
        auto body = std::move(pending_body_);
        pending_body_ = std::pmr::vector<std::byte>{&body_pool_};
        downstream_->Read(std::move(body));
    }

    // Then resume parsing remaining input
    if (!pending_input_.empty()) {
        auto input = std::move(pending_input_);
        pending_input_ = std::pmr::vector<std::byte>{&input_pool_};
        ProcessInput(std::move(input));
    }

    // Only resume upstream AFTER draining buffers to preserve byte order
    // (upstream Resume may deliver new data synchronously)
    if (!suspended_) {
        upstream_->Resume();
    }
}
```

**Key points:**
- `OnBody()` buffers body bytes in `pending_body_` BEFORE calling `llhttp_pause()`
- This is critical because llhttp has already consumed the bytes from the input buffer
- `llhttp_execute()` returns `HPE_PAUSED` immediately after callback returns
- On `Resume()`, drain buffers first, THEN resume upstream to preserve byte order
- `llhttp_resume()` clears pause state for next parse

**Buffer size limits:**

Backpressure buffers are capped to prevent unbounded memory growth:

```cpp
static constexpr size_t kMaxBufferedInput = 16 * 1024 * 1024;  // 16 MB
static constexpr size_t kMaxBufferedBody = 16 * 1024 * 1024;   // 16 MB

// In OnBody(), check before buffering:
if (pending_body_.size() + len > kMaxBufferedBody) {
    this->EmitError(*downstream_, {ErrorCode::BufferOverflow, "Body buffer exceeded"});
    this->RequestClose();
    return HPE_USER;  // stop parsing
}

// In ProcessInput(), check before buffering:
size_t remaining = data.size() - consumed;
if (pending_input_.size() + remaining > kMaxBufferedInput) {
    this->EmitError(*downstream_, {ErrorCode::BufferOverflow, "Input buffer exceeded"});
    this->RequestClose();
    return;  // don't buffer
}
```

Same pattern applies to ZstdDecompressor. 16 MB is generous for typical DBN records but
prevents runaway memory on pathological inputs or stuck downstream.

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
class ZstdDecompressor
    : public PipelineComponent<ZstdDecompressor<Downstream>>
    , public Suspendable
    , public std::enable_shared_from_this<ZstdDecompressor<Downstream>> {

    using Base = PipelineComponent<ZstdDecompressor<Downstream>>;
    friend Base;

public:
    ZstdDecompressor(Reactor& reactor, std::shared_ptr<Downstream> downstream);
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

    void DisableWatchers() { /* no-op */ }

private:
    void DoClose() {
        downstream_.reset();
        if (upstream_) {
            upstream_->Close();
            upstream_ = nullptr;
        }
    }

    Suspendable* upstream_ = nullptr;
    std::shared_ptr<Downstream> downstream_;

    ZSTD_DStream* dstream_ = nullptr;
    std::pmr::unsynchronized_pool_resource decompress_pool_;
    std::pmr::unsynchronized_pool_resource input_pool_;

    bool suspended_ = false;
    std::pmr::vector<std::byte> pending_input_;  // unconsumed when suspended
};
```

**Backpressure handling (ZstdDecompressor):**

Unlike llhttp, zstd doesn't have pause/resume APIs. ZstdDecompressor handles backpressure by buffering input:

```cpp
void ZstdDecompressor::Suspend() {
    if (this->IsClosed()) return;
    typename Base::ProcessingGuard guard(*this);
    suspended_ = true;
    upstream_->Suspend();
}

void ZstdDecompressor::Resume() {
    if (this->IsClosed()) return;
    typename Base::ProcessingGuard guard(*this);
    suspended_ = false;
    upstream_->Resume();

    // Process buffered input
    if (!pending_input_.empty()) {
        auto input = std::move(pending_input_);
        pending_input_ = std::pmr::vector<std::byte>{&input_pool_};
        ProcessInput(std::move(input));
    }
}

static constexpr size_t kMaxBufferedInput = 16 * 1024 * 1024;  // 16 MB

void ZstdDecompressor::Read(std::pmr::vector<std::byte> data) {
    if (this->IsClosed()) return;
    typename Base::ProcessingGuard guard(*this);

    if (suspended_) {
        // Check buffer limit before appending
        if (pending_input_.size() + data.size() > kMaxBufferedInput) {
            this->EmitError(*downstream_, {ErrorCode::BufferOverflow, "Zstd input buffer exceeded"});
            this->RequestClose();
            return;
        }
        // Buffer input for later processing
        pending_input_.insert(pending_input_.end(), data.begin(), data.end());
        return;
    }
    ProcessInput(std::move(data));
}

void ZstdDecompressor::ProcessInput(std::pmr::vector<std::byte> data) {
    ZSTD_inBuffer in{data.data(), data.size(), 0};

    while (in.pos < in.size) {
        std::pmr::vector<std::byte> out{ZSTD_DStreamOutSize(), &decompress_pool_};
        ZSTD_outBuffer out_buf{out.data(), out.size(), 0};

        size_t ret = ZSTD_decompressStream(dstream_, &out_buf, &in);
        if (ZSTD_isError(ret)) {
            this->EmitError(*downstream_, {ErrorCode::DecompressionError, ZSTD_getErrorName(ret)});
            this->RequestClose();
            return;
        }

        if (out_buf.pos > 0) {
            out.resize(out_buf.pos);
            downstream_->Read(std::move(out));

            // Check if downstream suspended us during Read()
            if (suspended_) {
                // Append remaining input (may already have buffered data)
                if (in.pos < in.size) {
                    size_t remaining = in.size - in.pos;
                    // Enforce buffer limit
                    if (pending_input_.size() + remaining > kMaxBufferedInput) {
                        this->EmitError(*downstream_, {ErrorCode::BufferOverflow, "Zstd input buffer exceeded"});
                        this->RequestClose();
                        return;
                    }
                    pending_input_.insert(pending_input_.end(),
                        static_cast<const std::byte*>(in.src) + in.pos,
                        static_cast<const std::byte*>(in.src) + in.size);
                }
                return;
            }
        }
    }
}
```

**Key points:**
- When suspended, input is buffered in `pending_input_`
- On Resume(), buffered input is processed
- During decompression loop, check `suspended_` after each downstream Read()
- If suspended mid-loop, buffer remaining input and return

**EOF validation (ZstdDecompressor::OnDone):**

On EOF, validate that the zstd frame ended cleanly:

```cpp
// Member variable to track pending EOF
bool done_pending_ = false;

void ZstdDecompressor::OnDone() {
    if (this->IsFinalized()) return;
    typename Base::ProcessingGuard guard(*this);

    if (suspended_) {
        // Defer EOF handling until Resume()
        done_pending_ = true;
        return;
    }

    FinalizeEof();
}

void ZstdDecompressor::FinalizeEof() {
    // Process any buffered input first
    if (!pending_input_.empty()) {
        auto input = std::move(pending_input_);
        pending_input_ = std::pmr::vector<std::byte>{&input_pool_};
        ProcessInput(std::move(input));
        if (this->IsFinalized()) return;  // error during processing
        if (suspended_) {
            // Got suspended during processing - defer rest
            done_pending_ = true;
            return;
        }
    }

    // Validate zstd frame is complete by trying to decompress empty input
    ZSTD_inBuffer in{nullptr, 0, 0};
    std::pmr::vector<std::byte> out{ZSTD_DStreamOutSize(), &decompress_pool_};
    ZSTD_outBuffer out_buf{out.data(), out.size(), 0};

    size_t ret = ZSTD_decompressStream(dstream_, &out_buf, &in);
    if (ZSTD_isError(ret)) {
        this->EmitError(*downstream_, {ErrorCode::DecompressionError, ZSTD_getErrorName(ret)});
        this->RequestClose();
        return;
    }

    // ret == 0 means frame is complete; ret > 0 means more input needed (truncated)
    if (ret != 0) {
        this->EmitError(*downstream_, {ErrorCode::DecompressionError, "Truncated zstd frame"});
        this->RequestClose();
        return;
    }

    // Deliver any final output
    if (out_buf.pos > 0) {
        out.resize(out_buf.pos);
        downstream_->Read(std::move(out));
        if (suspended_) {
            done_pending_ = true;  // defer terminal signal
            return;
        }
    }

    done_pending_ = false;
    this->EmitDone(*downstream_);

    // Reset for next message (keep-alive scenarios)
    ResetMessageState();
}

// Updated Resume() - drain buffers BEFORE resuming upstream to preserve order
void ZstdDecompressor::Resume() {
    if (this->IsClosed()) return;
    typename Base::ProcessingGuard guard(*this);
    suspended_ = false;

    // Process buffered input FIRST (before upstream delivers new data)
    if (!pending_input_.empty()) {
        auto input = std::move(pending_input_);
        pending_input_ = std::pmr::vector<std::byte>{&input_pool_};
        ProcessInput(std::move(input));
    }

    // Handle deferred EOF
    if (done_pending_ && !suspended_) {
        FinalizeEof();
    }

    // Only resume upstream AFTER draining buffers to preserve byte order
    if (!suspended_) {
        upstream_->Resume();
    }
}

void ZstdDecompressor::ResetMessageState() {
    this->ResetFinalized();
    ZSTD_initDStream(dstream_);  // reset decoder for next frame
    pending_input_.clear();       // clear any residual buffered data
}
```

**Validation semantics:**
- `ZSTD_decompressStream` returns 0 when frame is complete
- Returns > 0 when more input bytes needed
- Truncated stream triggers `DecompressionError`
- `ResetMessageState()` re-initializes decoder for next response

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
        Idle,         // not connected
        Connecting,   // TCP connecting
        Handshaking,  // TLS handshake in progress
        Ready,        // connected, handshake complete, can send requests
        Requesting,   // HTTP request sent, waiting for response
        Streaming,    // receiving response body
        Complete,     // response received successfully
        Error         // error occurred
    };

    State GetState() const;

private:
    Reactor& reactor_;
    std::string api_key_;
    State state_ = State::Idle;

    std::unique_ptr<TcpSocket> tcp_;  // owns the pipeline head
    // Note: No shared_ptr to pipeline - TcpSocket owns it via downstream_
    // Use raw pointers for access (valid while tcp_ exists)
};
```

**Ownership clarification:**

HistoricalClient owns TcpSocket via `unique_ptr`. TcpSocket owns the rest of the pipeline via `shared_ptr` chain. HistoricalClient does NOT hold a second `shared_ptr` to the pipeline - that would violate the "upstream owns downstream" invariant and keep components alive after `Close()`.

To access pipeline components for setup:
```cpp
bool HistoricalClient::Setup(const std::string& hostname) {
    auto zstd = std::make_shared<ZstdDecompressor<DbnParser>>(...);
    auto http = std::make_shared<HttpClient<...>>(reactor_, zstd);
    auto tls = TlsSocket<...>::Create(reactor_, http, hostname);

    // Check TLS init failure
    if (!tls) {
        state_ = State::Error;
        // Error already emitted by TlsSocket::Create via downstream_->OnError()
        return false;
    }

    // Wire up upstream pointers
    zstd->SetUpstream(http.get());
    http->SetUpstream(tls.get());
    tls->SetUpstream(tcp_.get());

    // Keep weak reference for HTTP requests
    http_weak_ = http;  // weak_ptr to HttpClient

    // Transfer ownership to TcpSocket
    tcp_->SetDownstream(std::move(tls));

    state_ = State::Handshaking;
    tls->StartHandshake();
    return true;
}

void HistoricalClient::Request(...) {
    // Guard on state and weak_ptr validity
    if (state_ != State::Ready) {
        // Not connected - report error
        return;
    }

    auto http = http_weak_.lock();
    if (!http) {
        // Pipeline was closed/destroyed
        return;
    }

    http->Request(host, path, auth_header);
    state_ = State::Requesting;
}

void HistoricalClient::Stop() {
    if (tcp_) {
        tcp_->Close();
    }
    http_weak_.reset();  // clear weak reference
    state_ = State::Idle;
}

private:
    std::weak_ptr<HttpClient<...>> http_weak_;  // safe access to HTTP layer
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
class CramAuth
    : public PipelineComponent<CramAuth<Downstream>>
    , public Suspendable
    , public std::enable_shared_from_this<CramAuth<Downstream>> {

    using Base = PipelineComponent<CramAuth<Downstream>>;
    friend Base;

public:
    CramAuth(Reactor& reactor, std::string api_key, std::shared_ptr<Downstream> downstream);

    void SetUpstream(Suspendable* upstream) { upstream_ = upstream; }

    void Read(std::pmr::vector<std::byte> data) {
        if (this->IsClosed()) return;
        typename Base::ProcessingGuard guard(*this);
        if (authenticated_) {
            downstream_->Read(std::move(data));  // bypass
            return;
        }
        ProcessAuthData(std::move(data));
    }

    void OnError(const Error& e);
    void OnDone();

    void DisableWatchers() { /* no-op */ }

    // From downstream
    void Write(std::pmr::vector<std::byte> data);
    void Suspend() override;
    void Resume() override;
    void Close() override;

    void Subscribe(...);
    void Start();

private:
    void DoClose() {
        downstream_.reset();
        if (upstream_) {
            upstream_->Close();
            upstream_ = nullptr;
        }
    }

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

    // Resource limits
    BufferOverflow,   // backpressure buffer exceeded

    // State
    InvalidState,     // e.g., Request() when request already in flight
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
