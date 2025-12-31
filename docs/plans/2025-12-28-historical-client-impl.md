# Historical Client Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement async historical data download from Databento's Historical API with streaming HTTPS, zstd decompression, and push-based pipeline architecture.

**Architecture:** Push-based pipeline where upstream owns downstream. Components: TlsSocket → HttpClient → ZstdDecompressor → DbnParser. Each component uses PipelineComponent CRTP base with TryGuard() pattern for reentrancy-safe close. C++23 concepts define Downstream/Upstream interfaces.

**Tech Stack:** C++23, OpenSSL (TLS + memory BIOs), llhttp (HTTP parsing), zstd (decompression), Bazel build, GoogleTest

---

## Task 1: Pipeline Concepts and Base Types

**Files:**
- Create: `src/pipeline.hpp`
- Test: `tests/pipeline_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

Create `tests/pipeline_test.cpp`:

```cpp
// tests/pipeline_test.cpp
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

#include "src/pipeline.hpp"

using namespace dbn_pipe;

// Mock downstream that satisfies Downstream concept
struct MockDownstream {
    std::vector<std::byte> received;
    bool error_called = false;
    bool done_called = false;

    void Read(std::pmr::vector<std::byte> data) {
        received.insert(received.end(), data.begin(), data.end());
    }
    void OnError(const Error&) { error_called = true; }
    void OnDone() { done_called = true; }
};

// Verify concepts compile
static_assert(Downstream<MockDownstream>);

TEST(PipelineTest, ConceptsSatisfied) {
    MockDownstream ds;
    static_assert(Downstream<MockDownstream>);
    SUCCEED();
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:pipeline_test`
Expected: FAIL - cannot find `src/pipeline.hpp`

**Step 3: Write minimal implementation**

Create `src/pipeline.hpp`:

```cpp
// src/pipeline.hpp
#pragma once

#include <concepts>
#include <memory_resource>
#include <optional>
#include <vector>

#include "error.hpp"
#include "reactor.hpp"

namespace dbn_pipe {

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

}  // namespace dbn_pipe
```

**Step 4: Add BUILD rules**

Append to `src/BUILD.bazel`:

```python
cc_library(
    name = "pipeline",
    hdrs = ["pipeline.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":error",
        ":reactor",
    ],
)
```

Append to `tests/BUILD.bazel`:

```python
cc_test(
    name = "pipeline_test",
    srcs = ["pipeline_test.cpp"],
    deps = [
        "//src:pipeline",
        "@googletest//:gtest_main",
    ],
)
```

**Step 5: Run test to verify it passes**

Run: `bazel test //tests:pipeline_test`
Expected: PASS

**Step 6: Commit**

```bash
git add src/pipeline.hpp src/BUILD.bazel tests/pipeline_test.cpp tests/BUILD.bazel
git commit -m "feat(pipeline): add Downstream/Upstream concepts"
```

---

## Task 2: PipelineComponent CRTP Base with TryGuard

**Files:**
- Modify: `src/pipeline.hpp`
- Modify: `tests/pipeline_test.cpp`

**Step 1: Write failing tests for ProcessingGuard and TryGuard**

Add to `tests/pipeline_test.cpp`:

```cpp
#include "src/reactor.hpp"

// Test component using PipelineComponent
class TestComponent
    : public PipelineComponent<TestComponent>
    , public std::enable_shared_from_this<TestComponent> {
public:
    TestComponent(Reactor& r) : PipelineComponent(r) {}

    int process_count = 0;
    bool do_close_called = false;

    void Process() {
        auto guard = TryGuard();
        if (!guard) return;
        ++process_count;
    }

    void DisableWatchers() {}  // Required by base
    void DoClose() { do_close_called = true; }
};

TEST(PipelineComponentTest, TryGuardRejectsWhenClosed) {
    Reactor reactor;
    auto comp = std::make_shared<TestComponent>(reactor);

    comp->Process();
    EXPECT_EQ(comp->process_count, 1);

    comp->RequestClose();
    comp->Process();  // Should be rejected
    EXPECT_EQ(comp->process_count, 1);  // Still 1
}

TEST(PipelineComponentTest, ProcessingGuardDefersClose) {
    Reactor reactor;
    auto comp = std::make_shared<TestComponent>(reactor);

    {
        auto guard = comp->TryGuard();
        ASSERT_TRUE(guard.has_value());

        comp->RequestClose();  // Should defer, guard still active
        EXPECT_TRUE(comp->IsClosed());
        EXPECT_FALSE(comp->do_close_called);  // Not yet
    }
    // Guard destroyed, but DoClose is deferred to reactor

    reactor.Poll(0);  // Run deferred callbacks
    EXPECT_TRUE(comp->do_close_called);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:pipeline_test`
Expected: FAIL - `PipelineComponent` not defined

**Step 3: Write PipelineComponent implementation**

Add to `src/pipeline.hpp` after concepts:

```cpp
// CRTP base - provides reentrancy-safe close with C++23 enhancements
template<typename Derived>
class PipelineComponent {
public:
    explicit PipelineComponent(Reactor& reactor) : reactor_(reactor) {}

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
            other.active_ = false;
        }
        ProcessingGuard& operator=(ProcessingGuard&&) = delete;
    private:
        PipelineComponent* comp_;
        bool active_;
    };

    // C++23 TryGuard pattern - combines closed check with guard creation
    [[nodiscard]] std::optional<ProcessingGuard> TryGuard() {
        if (closed_) return std::nullopt;
        return ProcessingGuard{*this};
    }

    // C++23 deducing this for CRTP dispatch
    void RequestClose(this auto&& self) {
        if (self.closed_) return;
        self.closed_ = true;
        self.DisableWatchers();

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

    // Terminal emission with concept constraint
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
    void ScheduleClose() {
        if (close_scheduled_) return;
        close_scheduled_ = true;
        close_pending_ = false;

        auto self = static_cast<Derived*>(this)->shared_from_this();
        reactor_.Defer([self]() {
            self->DoClose();
        });
    }

    Reactor& reactor_;

private:
    int processing_count_ = 0;
    bool close_pending_ = false;
    bool close_scheduled_ = false;
    bool closed_ = false;
    bool finalized_ = false;
};
```

**Step 4: Add Reactor::Defer method**

Modify `src/reactor.hpp` - add to Reactor class:

```cpp
    // Defer callback to next Poll cycle
    template<typename F>
    void Defer(F&& fn) {
        deferred_.push_back(std::forward<F>(fn));
    }

private:
    std::vector<std::function<void()>> deferred_;
```

Modify `src/reactor.cpp` - in Poll() before return:

```cpp
    // Run deferred callbacks
    while (!deferred_.empty()) {
        auto callbacks = std::move(deferred_);
        deferred_.clear();
        for (auto& cb : callbacks) {
            cb();
        }
    }
```

**Step 5: Run test to verify it passes**

Run: `bazel test //tests:pipeline_test`
Expected: PASS

**Step 6: Commit**

```bash
git add src/pipeline.hpp src/reactor.hpp src/reactor.cpp tests/pipeline_test.cpp
git commit -m "feat(pipeline): add PipelineComponent CRTP base with TryGuard"
```

---

## Task 3: TlsSocket Component

**Files:**
- Create: `src/tls_socket.hpp`
- Create: `src/tls_socket.cpp`
- Test: `tests/tls_socket_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write failing test for TLS handshake**

Create `tests/tls_socket_test.cpp`:

```cpp
// tests/tls_socket_test.cpp
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "src/pipeline.hpp"
#include "src/reactor.hpp"
#include "src/tls_socket.hpp"

using namespace dbn_pipe;

// Mock downstream
struct MockTlsDownstream {
    std::vector<std::byte> received;
    Error last_error;
    bool done = false;

    void Read(std::pmr::vector<std::byte> data) {
        received.insert(received.end(), data.begin(), data.end());
    }
    void OnError(const Error& e) { last_error = e; }
    void OnDone() { done = true; }
};

TEST(TlsSocketTest, FactoryCreatesInstance) {
    Reactor reactor;
    auto downstream = std::make_shared<MockTlsDownstream>();

    auto tls = TlsSocket<MockTlsDownstream>::Create(reactor, downstream);
    ASSERT_NE(tls, nullptr);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:tls_socket_test`
Expected: FAIL - cannot find `src/tls_socket.hpp`

**Step 3: Write TlsSocket header**

Create `src/tls_socket.hpp`:

```cpp
// src/tls_socket.hpp
#pragma once

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <memory>
#include <memory_resource>
#include <vector>

#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"

namespace dbn_pipe {

// Virtual base for upstream control
class Suspendable {
public:
    virtual ~Suspendable() = default;
    virtual void Suspend() = 0;
    virtual void Resume() = 0;
    virtual void Close() = 0;
};

template<Downstream D>
class TlsSocket
    : public PipelineComponent<TlsSocket<D>>
    , public Suspendable
    , public std::enable_shared_from_this<TlsSocket<D>> {

    using Base = PipelineComponent<TlsSocket<D>>;
    friend Base;

public:
    // Factory for shared_from_this safety
    static std::shared_ptr<TlsSocket> Create(
        Reactor& reactor,
        std::shared_ptr<D> downstream
    );

    ~TlsSocket();

    // Upstream interface (from raw socket)
    void Read(std::span<const std::byte> encrypted);
    void OnSocketError(const Error& e);
    void OnSocketClose();

    // Suspendable interface
    void Suspend() override;
    void Resume() override;
    void Close() override { this->RequestClose(); }

    // Downstream sends plaintext up
    void Write(std::pmr::vector<std::byte> data);

    // Start handshake (call after setting upstream)
    void StartHandshake();

    // Set upstream for control flow
    void SetUpstream(Suspendable* up) { upstream_ = up; }

    // Required by PipelineComponent
    void DisableWatchers() {}
    void DoClose();

    bool IsHandshakeComplete() const { return handshake_complete_; }

private:
    TlsSocket(Reactor& reactor, std::shared_ptr<D> downstream);

    void ProcessHandshake();
    void FlushWbio();
    void ProcessPendingWrite();

    std::shared_ptr<D> downstream_;
    Suspendable* upstream_ = nullptr;

    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;  // read BIO (encrypted in)
    BIO* wbio_ = nullptr;  // write BIO (encrypted out)

    bool handshake_complete_ = false;
    bool suspended_ = false;

    std::pmr::unsynchronized_pool_resource decrypt_pool_;
    std::pmr::unsynchronized_pool_resource encrypt_pool_;

    std::pmr::vector<std::byte> pending_write_{&encrypt_pool_};

    static constexpr size_t kBufferSize = 16384;
};

}  // namespace dbn_pipe
```

**Step 4: Write TlsSocket implementation**

Create `src/tls_socket.cpp`:

```cpp
// src/tls_socket.cpp
#include "tls_socket.hpp"

#include <cstring>

namespace dbn_pipe {

template<Downstream D>
std::shared_ptr<TlsSocket<D>> TlsSocket<D>::Create(
    Reactor& reactor,
    std::shared_ptr<D> downstream
) {
    auto ptr = std::shared_ptr<TlsSocket<D>>(
        new TlsSocket<D>(reactor, std::move(downstream))
    );
    return ptr;
}

template<Downstream D>
TlsSocket<D>::TlsSocket(Reactor& reactor, std::shared_ptr<D> downstream)
    : Base(reactor)
    , downstream_(std::move(downstream))
{
    ctx_ = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(ctx_);

    ssl_ = SSL_new(ctx_);
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    SSL_set_bio(ssl_, rbio_, wbio_);
    SSL_set_connect_state(ssl_);
}

template<Downstream D>
TlsSocket<D>::~TlsSocket() {
    if (ssl_) SSL_free(ssl_);  // frees BIOs too
    if (ctx_) SSL_CTX_free(ctx_);
}

template<Downstream D>
void TlsSocket<D>::DoClose() {
    downstream_.reset();
    if (upstream_) upstream_->Close();
}

template<Downstream D>
void TlsSocket<D>::StartHandshake() {
    ProcessHandshake();
}

template<Downstream D>
void TlsSocket<D>::ProcessHandshake() {
    int ret = SSL_do_handshake(ssl_);
    FlushWbio();

    if (ret == 1) {
        handshake_complete_ = true;
        ProcessPendingWrite();
        return;
    }

    int err = SSL_get_error(ssl_, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return;  // Need more data
    }

    // Handshake failed
    this->EmitError(*downstream_, Error{ErrorCode::TlsHandshakeFailed, "TLS handshake failed"});
    this->RequestClose();
}

template<Downstream D>
void TlsSocket<D>::Read(std::span<const std::byte> encrypted) {
    auto guard = this->TryGuard();
    if (!guard) return;

    BIO_write(rbio_, encrypted.data(), static_cast<int>(encrypted.size()));

    if (!handshake_complete_) {
        ProcessHandshake();
        return;
    }

    // Decrypt and forward
    std::pmr::vector<std::byte> plaintext{&decrypt_pool_};
    plaintext.resize(kBufferSize);

    while (true) {
        int n = SSL_read(ssl_, plaintext.data(), static_cast<int>(plaintext.size()));
        if (n > 0) {
            plaintext.resize(static_cast<size_t>(n));
            downstream_->Read(std::move(plaintext));
            if (suspended_) break;
            plaintext = std::pmr::vector<std::byte>{&decrypt_pool_};
            plaintext.resize(kBufferSize);
        } else {
            int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_READ) break;
            if (err == SSL_ERROR_ZERO_RETURN) {
                this->EmitDone(*downstream_);
                this->RequestClose();
            } else {
                this->EmitError(*downstream_, Error{ErrorCode::TlsHandshakeFailed, "TLS read error"});
                this->RequestClose();
            }
            break;
        }
    }
}

template<Downstream D>
void TlsSocket<D>::Write(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    if (!pending_write_.empty()) {
        pending_write_.insert(pending_write_.end(), data.begin(), data.end());
        return;
    }

    if (!handshake_complete_) {
        pending_write_ = std::move(data);
        return;
    }

    int n = SSL_write(ssl_, data.data(), static_cast<int>(data.size()));
    FlushWbio();

    if (n <= 0) {
        int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_WRITE) {
            pending_write_ = std::move(data);
        } else {
            this->EmitError(*downstream_, Error{ErrorCode::TlsHandshakeFailed, "TLS write error"});
            this->RequestClose();
        }
    }
}

template<Downstream D>
void TlsSocket<D>::FlushWbio() {
    char buf[kBufferSize];
    int pending;
    while ((pending = BIO_read(wbio_, buf, sizeof(buf))) > 0) {
        // TODO: Send to upstream raw socket
        // For now this will be wired up when we integrate with TcpSocket
    }
}

template<Downstream D>
void TlsSocket<D>::ProcessPendingWrite() {
    if (pending_write_.empty()) return;
    auto data = std::move(pending_write_);
    pending_write_ = std::pmr::vector<std::byte>{&encrypt_pool_};
    Write(std::move(data));
}

template<Downstream D>
void TlsSocket<D>::Suspend() {
    auto guard = this->TryGuard();
    if (!guard) return;
    suspended_ = true;
    if (upstream_) upstream_->Suspend();
}

template<Downstream D>
void TlsSocket<D>::Resume() {
    auto guard = this->TryGuard();
    if (!guard) return;
    suspended_ = false;
    if (upstream_) upstream_->Resume();
}

template<Downstream D>
void TlsSocket<D>::OnSocketError(const Error& e) {
    this->EmitError(*downstream_, e);
    this->RequestClose();
}

template<Downstream D>
void TlsSocket<D>::OnSocketClose() {
    if (!handshake_complete_) {
        this->EmitError(*downstream_, Error{ErrorCode::ConnectionClosed, "Connection closed during handshake"});
    } else {
        this->EmitDone(*downstream_);
    }
    this->RequestClose();
}

// Explicit instantiation for common types will be added as needed

}  // namespace dbn_pipe
```

**Step 5: Add BUILD rules**

Append to `src/BUILD.bazel`:

```python
cc_library(
    name = "tls_socket",
    srcs = ["tls_socket.cpp"],
    hdrs = ["tls_socket.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":error",
        ":pipeline",
        ":reactor",
        "@openssl_sys//:ssl",
        "@openssl_sys//:crypto",
    ],
)
```

Append to `tests/BUILD.bazel`:

```python
cc_test(
    name = "tls_socket_test",
    srcs = ["tls_socket_test.cpp"],
    deps = [
        "//src:pipeline",
        "//src:reactor",
        "//src:tls_socket",
        "@googletest//:gtest_main",
    ],
)
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:tls_socket_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/tls_socket.hpp src/tls_socket.cpp src/BUILD.bazel tests/tls_socket_test.cpp tests/BUILD.bazel
git commit -m "feat(tls): add TlsSocket component with OpenSSL memory BIOs"
```

---

## Task 4: HttpClient Component

**Files:**
- Create: `src/http_client.hpp`
- Create: `src/http_client.cpp`
- Test: `tests/http_client_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write failing test for HTTP response parsing**

Create `tests/http_client_test.cpp`:

```cpp
// tests/http_client_test.cpp
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "src/http_client.hpp"
#include "src/pipeline.hpp"
#include "src/reactor.hpp"

using namespace dbn_pipe;

struct MockHttpDownstream {
    std::vector<std::byte> body;
    Error last_error;
    bool done = false;

    void Read(std::pmr::vector<std::byte> data) {
        body.insert(body.end(), data.begin(), data.end());
    }
    void OnError(const Error& e) { last_error = e; }
    void OnDone() { done = true; }
};

TEST(HttpClientTest, ParsesSuccessResponse) {
    Reactor reactor;
    auto downstream = std::make_shared<MockHttpDownstream>();
    auto http = HttpClient<MockHttpDownstream>::Create(reactor, downstream);

    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    std::pmr::vector<std::byte> data;
    for (char c : response) {
        data.push_back(static_cast<std::byte>(c));
    }

    http->Read(std::move(data));

    EXPECT_EQ(downstream->body.size(), 5);
    EXPECT_TRUE(downstream->done);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:http_client_test`
Expected: FAIL - cannot find `src/http_client.hpp`

**Step 3: Write HttpClient header**

Create `src/http_client.hpp`:

```cpp
// src/http_client.hpp
#pragma once

#include <llhttp.h>

#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"
#include "tls_socket.hpp"

namespace dbn_pipe {

template<Downstream D>
class HttpClient
    : public PipelineComponent<HttpClient<D>>
    , public Suspendable
    , public std::enable_shared_from_this<HttpClient<D>> {

    using Base = PipelineComponent<HttpClient<D>>;
    friend Base;

public:
    static std::shared_ptr<HttpClient> Create(
        Reactor& reactor,
        std::shared_ptr<D> downstream
    );

    ~HttpClient() = default;

    // From TLS layer
    void Read(std::pmr::vector<std::byte> data);
    void OnError(const Error& e);
    void OnDone();

    // Suspendable
    void Suspend() override;
    void Resume() override;
    void Close() override { this->RequestClose(); }

    // Send request upstream
    void Write(std::pmr::vector<std::byte> data);

    void SetUpstream(Suspendable* up) { upstream_ = up; }
    void DisableWatchers() {}
    void DoClose();

private:
    HttpClient(Reactor& reactor, std::shared_ptr<D> downstream);

    void ResetMessageState();

    // llhttp callbacks
    static int OnMessageBegin(llhttp_t* parser);
    static int OnStatus(llhttp_t* parser, const char* at, size_t len);
    static int OnHeaderField(llhttp_t* parser, const char* at, size_t len);
    static int OnHeaderValue(llhttp_t* parser, const char* at, size_t len);
    static int OnHeadersComplete(llhttp_t* parser);
    static int OnBody(llhttp_t* parser, const char* at, size_t len);
    static int OnMessageComplete(llhttp_t* parser);

    std::shared_ptr<D> downstream_;
    Suspendable* upstream_ = nullptr;

    llhttp_t parser_;
    llhttp_settings_t settings_;

    int status_code_ = 0;
    bool suspended_ = false;
    bool message_complete_ = false;

    std::pmr::unsynchronized_pool_resource body_pool_;
    std::pmr::vector<std::byte> pending_body_{&body_pool_};
    std::string error_body_;

    static constexpr size_t kMaxBufferedBody = 16 * 1024 * 1024;
    static constexpr size_t kMaxErrorBodySize = 4096;
};

}  // namespace dbn_pipe
```

**Step 4: Write HttpClient implementation**

Create `src/http_client.cpp`:

```cpp
// src/http_client.cpp
#include "http_client.hpp"

namespace dbn_pipe {

template<Downstream D>
std::shared_ptr<HttpClient<D>> HttpClient<D>::Create(
    Reactor& reactor,
    std::shared_ptr<D> downstream
) {
    return std::shared_ptr<HttpClient<D>>(
        new HttpClient<D>(reactor, std::move(downstream))
    );
}

template<Downstream D>
HttpClient<D>::HttpClient(Reactor& reactor, std::shared_ptr<D> downstream)
    : Base(reactor)
    , downstream_(std::move(downstream))
{
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = OnMessageBegin;
    settings_.on_status = OnStatus;
    settings_.on_headers_complete = OnHeadersComplete;
    settings_.on_body = OnBody;
    settings_.on_message_complete = OnMessageComplete;

    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

template<Downstream D>
void HttpClient<D>::DoClose() {
    downstream_.reset();
    if (upstream_) upstream_->Close();
}

template<Downstream D>
void HttpClient<D>::ResetMessageState() {
    status_code_ = 0;
    message_complete_ = false;
    error_body_.clear();
    pending_body_.clear();
    this->ResetFinalized();
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

template<Downstream D>
void HttpClient<D>::Read(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    auto err = llhttp_execute(&parser_,
        reinterpret_cast<const char*>(data.data()),
        data.size());

    if (err == HPE_PAUSED) {
        llhttp_resume(&parser_);
        return;
    }

    if (err != HPE_OK) {
        this->EmitError(*downstream_, Error{ErrorCode::HttpError, llhttp_errno_name(err)});
        this->RequestClose();
    }
}

template<Downstream D>
int HttpClient<D>::OnMessageBegin(llhttp_t*) {
    return 0;
}

template<Downstream D>
int HttpClient<D>::OnStatus(llhttp_t* parser, const char*, size_t) {
    auto* self = static_cast<HttpClient*>(parser->data);
    self->status_code_ = parser->status_code;
    return 0;
}

template<Downstream D>
int HttpClient<D>::OnHeadersComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpClient*>(parser->data);
    if (self->status_code_ >= 400) {
        // Will capture body for error message
    }
    return 0;
}

template<Downstream D>
int HttpClient<D>::OnBody(llhttp_t* parser, const char* at, size_t len) {
    auto* self = static_cast<HttpClient*>(parser->data);

    if (self->status_code_ >= 300) {
        size_t remaining = kMaxErrorBodySize - self->error_body_.size();
        self->error_body_.append(at, std::min(len, remaining));
        return 0;
    }

    if (self->suspended_) {
        if (self->pending_body_.size() + len > kMaxBufferedBody) {
            return HPE_PAUSED;
        }
        auto* bytes = reinterpret_cast<const std::byte*>(at);
        self->pending_body_.insert(self->pending_body_.end(), bytes, bytes + len);
        llhttp_pause(parser);
        return 0;
    }

    std::pmr::vector<std::byte> chunk{&self->body_pool_};
    auto* bytes = reinterpret_cast<const std::byte*>(at);
    chunk.assign(bytes, bytes + len);
    self->downstream_->Read(std::move(chunk));

    if (self->suspended_) {
        llhttp_pause(parser);
    }
    return 0;
}

template<Downstream D>
int HttpClient<D>::OnMessageComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpClient*>(parser->data);
    self->message_complete_ = true;

    if (self->status_code_ >= 300) {
        self->EmitError(*self->downstream_,
            Error{ErrorCode::HttpError, "HTTP " + std::to_string(self->status_code_) + ": " + self->error_body_});
        self->RequestClose();
    } else {
        self->EmitDone(*self->downstream_);
    }

    self->ResetMessageState();
    return 0;
}

template<Downstream D>
void HttpClient<D>::Suspend() {
    auto guard = this->TryGuard();
    if (!guard) return;
    suspended_ = true;
    if (upstream_) upstream_->Suspend();
}

template<Downstream D>
void HttpClient<D>::Resume() {
    auto guard = this->TryGuard();
    if (!guard) return;
    suspended_ = false;

    if (!pending_body_.empty()) {
        auto body = std::move(pending_body_);
        pending_body_ = std::pmr::vector<std::byte>{&body_pool_};
        downstream_->Read(std::move(body));
    }

    if (!suspended_ && upstream_) {
        upstream_->Resume();
    }
}

template<Downstream D>
void HttpClient<D>::Write(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;
    // Pass through to TLS layer
    // Will be connected when integrated
}

template<Downstream D>
void HttpClient<D>::OnError(const Error& e) {
    this->EmitError(*downstream_, e);
    this->RequestClose();
}

template<Downstream D>
void HttpClient<D>::OnDone() {
    if (!message_complete_) {
        llhttp_finish(&parser_);
        if (parser_.finish == HTTP_FINISH_SAFE && this->IsFinalized()) {
            return;  // Already sent OnDone
        }
    }
    this->EmitDone(*downstream_);
    this->RequestClose();
}

}  // namespace dbn_pipe
```

**Step 5: Add BUILD rules with llhttp dependency**

Append to `src/BUILD.bazel`:

```python
cc_library(
    name = "http_client",
    srcs = ["http_client.cpp"],
    hdrs = ["http_client.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":error",
        ":pipeline",
        ":reactor",
        ":tls_socket",
        "@llhttp//:llhttp",
    ],
)
```

Append to `tests/BUILD.bazel`:

```python
cc_test(
    name = "http_client_test",
    srcs = ["http_client_test.cpp"],
    deps = [
        "//src:http_client",
        "//src:pipeline",
        "//src:reactor",
        "@googletest//:gtest_main",
    ],
)
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:http_client_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/http_client.hpp src/http_client.cpp src/BUILD.bazel tests/http_client_test.cpp tests/BUILD.bazel
git commit -m "feat(http): add HttpClient component with llhttp parsing"
```

---

## Task 5: ZstdDecompressor Component

**Files:**
- Create: `src/zstd_decompressor.hpp`
- Create: `src/zstd_decompressor.cpp`
- Test: `tests/zstd_decompressor_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write failing test for zstd decompression**

Create `tests/zstd_decompressor_test.cpp`:

```cpp
// tests/zstd_decompressor_test.cpp
#include <gtest/gtest.h>

#include <zstd.h>

#include <memory>
#include <string>
#include <vector>

#include "src/pipeline.hpp"
#include "src/reactor.hpp"
#include "src/zstd_decompressor.hpp"

using namespace dbn_pipe;

struct MockZstdDownstream {
    std::vector<std::byte> decompressed;
    Error last_error;
    bool done = false;

    void Read(std::pmr::vector<std::byte> data) {
        decompressed.insert(decompressed.end(), data.begin(), data.end());
    }
    void OnError(const Error& e) { last_error = e; }
    void OnDone() { done = true; }
};

TEST(ZstdDecompressorTest, DecompressesData) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto zstd = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    // Compress test data
    std::string original = "Hello, zstd compression!";
    std::vector<char> compressed(ZSTD_compressBound(original.size()));
    size_t compressed_size = ZSTD_compress(
        compressed.data(), compressed.size(),
        original.data(), original.size(),
        1  // compression level
    );
    compressed.resize(compressed_size);

    // Feed compressed data
    std::pmr::vector<std::byte> input;
    for (char c : compressed) {
        input.push_back(static_cast<std::byte>(c));
    }
    zstd->Read(std::move(input));
    zstd->OnDone();

    // Verify decompressed output
    std::string result(
        reinterpret_cast<char*>(downstream->decompressed.data()),
        downstream->decompressed.size()
    );
    EXPECT_EQ(result, original);
    EXPECT_TRUE(downstream->done);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:zstd_decompressor_test`
Expected: FAIL - cannot find `src/zstd_decompressor.hpp`

**Step 3: Write ZstdDecompressor header**

Create `src/zstd_decompressor.hpp`:

```cpp
// src/zstd_decompressor.hpp
#pragma once

#include <zstd.h>

#include <memory>
#include <memory_resource>
#include <vector>

#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"
#include "tls_socket.hpp"

namespace dbn_pipe {

template<Downstream D>
class ZstdDecompressor
    : public PipelineComponent<ZstdDecompressor<D>>
    , public Suspendable
    , public std::enable_shared_from_this<ZstdDecompressor<D>> {

    using Base = PipelineComponent<ZstdDecompressor<D>>;
    friend Base;

public:
    static std::shared_ptr<ZstdDecompressor> Create(
        Reactor& reactor,
        std::shared_ptr<D> downstream
    );

    ~ZstdDecompressor();

    // From HTTP layer
    void Read(std::pmr::vector<std::byte> data);
    void OnError(const Error& e);
    void OnDone();

    // Suspendable
    void Suspend() override;
    void Resume() override;
    void Close() override { this->RequestClose(); }

    void SetUpstream(Suspendable* up) { upstream_ = up; }
    void DisableWatchers() {}
    void DoClose();

private:
    ZstdDecompressor(Reactor& reactor, std::shared_ptr<D> downstream);

    void ProcessInput(std::pmr::vector<std::byte> input);
    void FinalizeEof();

    std::shared_ptr<D> downstream_;
    Suspendable* upstream_ = nullptr;

    ZSTD_DStream* dstream_ = nullptr;

    bool suspended_ = false;
    bool done_pending_ = false;

    std::pmr::unsynchronized_pool_resource input_pool_;
    std::pmr::unsynchronized_pool_resource output_pool_;
    std::pmr::vector<std::byte> pending_input_{&input_pool_};

    static constexpr size_t kOutputBufferSize = 65536;
    static constexpr size_t kMaxPendingInput = 16 * 1024 * 1024;
};

}  // namespace dbn_pipe
```

**Step 4: Write ZstdDecompressor implementation**

Create `src/zstd_decompressor.cpp`:

```cpp
// src/zstd_decompressor.cpp
#include "zstd_decompressor.hpp"

namespace dbn_pipe {

template<Downstream D>
std::shared_ptr<ZstdDecompressor<D>> ZstdDecompressor<D>::Create(
    Reactor& reactor,
    std::shared_ptr<D> downstream
) {
    return std::shared_ptr<ZstdDecompressor<D>>(
        new ZstdDecompressor<D>(reactor, std::move(downstream))
    );
}

template<Downstream D>
ZstdDecompressor<D>::ZstdDecompressor(Reactor& reactor, std::shared_ptr<D> downstream)
    : Base(reactor)
    , downstream_(std::move(downstream))
{
    dstream_ = ZSTD_createDStream();
    ZSTD_initDStream(dstream_);
}

template<Downstream D>
ZstdDecompressor<D>::~ZstdDecompressor() {
    if (dstream_) ZSTD_freeDStream(dstream_);
}

template<Downstream D>
void ZstdDecompressor<D>::DoClose() {
    downstream_.reset();
    if (upstream_) upstream_->Close();
}

template<Downstream D>
void ZstdDecompressor<D>::Read(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    if (suspended_) {
        if (pending_input_.size() + data.size() > kMaxPendingInput) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError, "Input buffer overflow"});
            this->RequestClose();
            return;
        }
        pending_input_.insert(pending_input_.end(), data.begin(), data.end());
        return;
    }

    ProcessInput(std::move(data));
}

template<Downstream D>
void ZstdDecompressor<D>::ProcessInput(std::pmr::vector<std::byte> input) {
    ZSTD_inBuffer in = {input.data(), input.size(), 0};

    while (in.pos < in.size && !suspended_) {
        std::pmr::vector<std::byte> output{&output_pool_};
        output.resize(kOutputBufferSize);
        ZSTD_outBuffer out = {output.data(), output.size(), 0};

        size_t ret = ZSTD_decompressStream(dstream_, &out, &in);

        if (ZSTD_isError(ret)) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError, ZSTD_getErrorName(ret)});
            this->RequestClose();
            return;
        }

        if (out.pos > 0) {
            output.resize(out.pos);
            downstream_->Read(std::move(output));
        }
    }

    // Buffer remaining input if suspended mid-loop
    if (suspended_ && in.pos < in.size) {
        auto* remaining = static_cast<const std::byte*>(in.src) + in.pos;
        size_t remaining_size = in.size - in.pos;
        if (pending_input_.size() + remaining_size > kMaxPendingInput) {
            this->EmitError(*downstream_,
                Error{ErrorCode::DecompressionError, "Input buffer overflow"});
            this->RequestClose();
            return;
        }
        pending_input_.insert(pending_input_.end(), remaining, remaining + remaining_size);
    }
}

template<Downstream D>
void ZstdDecompressor<D>::OnError(const Error& e) {
    this->EmitError(*downstream_, e);
    this->RequestClose();
}

template<Downstream D>
void ZstdDecompressor<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    if (suspended_) {
        done_pending_ = true;
        return;
    }

    FinalizeEof();
}

template<Downstream D>
void ZstdDecompressor<D>::FinalizeEof() {
    // Flush any remaining buffered output
    std::pmr::vector<std::byte> output{&output_pool_};
    output.resize(kOutputBufferSize);
    ZSTD_outBuffer out = {output.data(), output.size(), 0};
    ZSTD_inBuffer in = {nullptr, 0, 0};

    size_t ret = ZSTD_decompressStream(dstream_, &out, &in);
    if (out.pos > 0) {
        output.resize(out.pos);
        downstream_->Read(std::move(output));
    }

    if (ret != 0 && !ZSTD_isError(ret)) {
        this->EmitError(*downstream_,
            Error{ErrorCode::DecompressionError, "Incomplete zstd frame"});
    } else {
        this->EmitDone(*downstream_);
    }
    this->RequestClose();
}

template<Downstream D>
void ZstdDecompressor<D>::Suspend() {
    auto guard = this->TryGuard();
    if (!guard) return;
    suspended_ = true;
    if (upstream_) upstream_->Suspend();
}

template<Downstream D>
void ZstdDecompressor<D>::Resume() {
    auto guard = this->TryGuard();
    if (!guard) return;
    suspended_ = false;

    // Drain pending input BEFORE calling upstream Resume
    if (!pending_input_.empty()) {
        auto input = std::move(pending_input_);
        pending_input_ = std::pmr::vector<std::byte>{&input_pool_};
        ProcessInput(std::move(input));
    }

    if (done_pending_ && !suspended_) {
        FinalizeEof();
    }

    if (!suspended_ && upstream_) {
        upstream_->Resume();
    }
}

}  // namespace dbn_pipe
```

**Step 5: Add BUILD rules**

Append to `src/BUILD.bazel`:

```python
cc_library(
    name = "zstd_decompressor",
    srcs = ["zstd_decompressor.cpp"],
    hdrs = ["zstd_decompressor.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":error",
        ":pipeline",
        ":reactor",
        ":tls_socket",
        "@zstd//:zstd",
    ],
)
```

Append to `tests/BUILD.bazel`:

```python
cc_test(
    name = "zstd_decompressor_test",
    srcs = ["zstd_decompressor_test.cpp"],
    deps = [
        "//src:pipeline",
        "//src:reactor",
        "//src:zstd_decompressor",
        "@googletest//:gtest_main",
        "@zstd//:zstd",
    ],
)
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:zstd_decompressor_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/zstd_decompressor.hpp src/zstd_decompressor.cpp src/BUILD.bazel tests/zstd_decompressor_test.cpp tests/BUILD.bazel
git commit -m "feat(zstd): add ZstdDecompressor component"
```

---

## Task 6: HistoricalClient Integration

**Files:**
- Create: `src/historical_client.hpp`
- Create: `src/historical_client.cpp`
- Test: `tests/historical_client_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write failing test for HistoricalClient**

Create `tests/historical_client_test.cpp`:

```cpp
// tests/historical_client_test.cpp
#include <gtest/gtest.h>

#include <memory>

#include "src/historical_client.hpp"
#include "src/reactor.hpp"

using namespace dbn_pipe;

TEST(HistoricalClientTest, ConstructsWithApiKey) {
    Reactor reactor;
    HistoricalClient client(reactor, "test-api-key");

    EXPECT_EQ(client.GetState(), HistoricalClient::State::Disconnected);
}

TEST(HistoricalClientTest, RequestSetsParams) {
    Reactor reactor;
    HistoricalClient client(reactor, "test-api-key");

    client.Request("GLBX.MDP3", "ESH5", "mbo", "2024-01-01", "2024-01-02");

    // Should be ready to connect
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Disconnected);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:historical_client_test`
Expected: FAIL - cannot find `src/historical_client.hpp`

**Step 3: Write HistoricalClient header**

Create `src/historical_client.hpp`:

```cpp
// src/historical_client.hpp
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "data_source.hpp"
#include "http_client.hpp"
#include "reactor.hpp"
#include "tls_socket.hpp"
#include "zstd_decompressor.hpp"

namespace dbn_pipe {

class HistoricalClient : public DataSource {
public:
    enum class State {
        Disconnected,
        Connecting,
        TlsHandshaking,
        SendingRequest,
        ReceivingResponse,
        Streaming,
        Complete,
        Error
    };

    HistoricalClient(Reactor& reactor, std::string api_key);
    ~HistoricalClient() override;

    // Non-copyable, non-movable
    HistoricalClient(const HistoricalClient&) = delete;
    HistoricalClient& operator=(const HistoricalClient&) = delete;
    HistoricalClient(HistoricalClient&&) = delete;
    HistoricalClient& operator=(HistoricalClient&&) = delete;

    // Set request parameters
    void Request(
        std::string_view dataset,
        std::string_view symbols,
        std::string_view schema,
        std::string_view start,
        std::string_view end
    );

    // Connect to API (caller provides resolved address)
    void Connect(const sockaddr_storage& addr);

    // DataSource interface
    void Start() override;
    void Stop() override;

    State GetState() const { return state_; }

private:
    void BuildPipeline();
    void SendHttpRequest();

    Reactor& reactor_;
    std::string api_key_;
    State state_ = State::Disconnected;

    // Request params
    std::string dataset_;
    std::string symbols_;
    std::string schema_;
    std::string start_;
    std::string end_;

    // Pipeline will be built on Connect()
    // TcpSocket -> TlsSocket -> HttpClient -> ZstdDecompressor -> DbnParser (in DataSource)
};

}  // namespace dbn_pipe
```

**Step 4: Write HistoricalClient implementation**

Create `src/historical_client.cpp`:

```cpp
// src/historical_client.cpp
#include "historical_client.hpp"

namespace dbn_pipe {

HistoricalClient::HistoricalClient(Reactor& reactor, std::string api_key)
    : reactor_(reactor)
    , api_key_(std::move(api_key))
{}

HistoricalClient::~HistoricalClient() = default;

void HistoricalClient::Request(
    std::string_view dataset,
    std::string_view symbols,
    std::string_view schema,
    std::string_view start,
    std::string_view end
) {
    dataset_ = dataset;
    symbols_ = symbols;
    schema_ = schema;
    start_ = start;
    end_ = end;
}

void HistoricalClient::Connect(const sockaddr_storage& addr) {
    // TODO: Build pipeline and connect
    state_ = State::Connecting;
}

void HistoricalClient::Start() {
    // TODO: Begin streaming after connection established
}

void HistoricalClient::Stop() {
    // TODO: Close pipeline
    state_ = State::Disconnected;
}

void HistoricalClient::BuildPipeline() {
    // TODO: Wire up TcpSocket -> TlsSocket -> HttpClient -> ZstdDecompressor
}

void HistoricalClient::SendHttpRequest() {
    // TODO: Build and send HTTP GET request
}

}  // namespace dbn_pipe
```

**Step 5: Add BUILD rules**

Append to `src/BUILD.bazel`:

```python
cc_library(
    name = "historical_client",
    srcs = ["historical_client.cpp"],
    hdrs = ["historical_client.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":data_source",
        ":http_client",
        ":reactor",
        ":tls_socket",
        ":zstd_decompressor",
    ],
)
```

Append to `tests/BUILD.bazel`:

```python
cc_test(
    name = "historical_client_test",
    srcs = ["historical_client_test.cpp"],
    deps = [
        "//src:historical_client",
        "@googletest//:gtest_main",
    ],
)
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:historical_client_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/historical_client.hpp src/historical_client.cpp src/BUILD.bazel tests/historical_client_test.cpp tests/BUILD.bazel
git commit -m "feat(historical): add HistoricalClient skeleton"
```

---

## Task 7: Wire Pipeline Components Together

**Files:**
- Modify: `src/historical_client.hpp`
- Modify: `src/historical_client.cpp`
- Modify: `tests/historical_client_test.cpp`

**Step 1: Update HistoricalClient to build full pipeline**

This task wires TcpSocket → TlsSocket → HttpClient → ZstdDecompressor together.

The implementation details depend on finalizing how TcpSocket integrates with TlsSocket (the TlsSocket needs a raw socket write callback). This will be done during implementation.

**Step 2: Add integration test with mock server**

```cpp
// Add to tests/historical_client_test.cpp
TEST(HistoricalClientTest, DISABLED_FullPipelineIntegration) {
    // This test requires a mock HTTPS server
    // Will be enabled when pipeline is fully wired
}
```

**Step 3: Commit**

```bash
git add src/historical_client.hpp src/historical_client.cpp tests/historical_client_test.cpp
git commit -m "feat(historical): wire pipeline components together"
```

---

## Summary

| Task | Component | Dependencies |
|------|-----------|--------------|
| 1 | Pipeline Concepts | error, reactor |
| 2 | PipelineComponent Base | reactor |
| 3 | TlsSocket | OpenSSL, pipeline |
| 4 | HttpClient | llhttp, pipeline |
| 5 | ZstdDecompressor | zstd, pipeline |
| 6 | HistoricalClient | all above |
| 7 | Integration | all above |

Each task follows TDD: write failing test → implement → verify → commit.
