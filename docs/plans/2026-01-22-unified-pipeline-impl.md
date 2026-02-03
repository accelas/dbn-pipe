# Unified Pipeline Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement the unified `Pipeline<Protocol>` design from `2026-01-22-unified-pipeline-design.md`, replacing the current dual-pipeline architecture (streaming pipelines + ApiPipeline) with a single flexible abstraction.

**Architecture:** Protocol is a static factory defining sink type and construction. Pipeline is a thin runtime shell managing lifecycle. Sink is a concept with required lifecycle methods (`OnError`, `OnComplete`, `Invalidate`). Reusable components move to `lib/stream/`, domain-specific code stays in `src/`.

**Tech Stack:** C++23 concepts, `std::expected`, atomic operations, gtest

---

## Phase 1: Create Foundation (lib/stream/)

### Task 1: Create Sink Concepts and RecordSink

**Files:**
- Create: `lib/stream/sink.hpp`
- Test: `tests/sink_test.cpp`

**Step 1: Write the failing test for Sink concept**

```cpp
// tests/sink_test.cpp
#include <gtest/gtest.h>

#include "dbn_pipe/stream/sink.hpp"
#include "dbn_pipe/stream/error.hpp"

using namespace dbn_pipe;

// A minimal type that should satisfy Sink
struct MinimalSink {
    void OnError(const Error&) {}
    void OnComplete() {}
    void Invalidate() {}
};

// Verify concept satisfaction at compile time
static_assert(Sink<MinimalSink>, "MinimalSink must satisfy Sink concept");

TEST(SinkConceptTest, MinimalSinkSatisfiesConcept) {
    SUCCEED();  // Compile-time check above is the real test
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target sink_test && ./build/tests/sink_test`
Expected: FAIL with "No such file or directory" for sink.hpp

**Step 3: Write sink.hpp with Sink concept**

```cpp
// lib/stream/sink.hpp
#pragma once

#include <atomic>
#include <concepts>
#include <functional>

#include "dbn_pipe/stream/error.hpp"

namespace dbn_pipe {

// Base sink concept - lifecycle methods required for all sinks
template<typename S>
concept Sink = requires(S& s, const Error& e) {
    { s.OnError(e) } -> std::same_as<void>;
    { s.OnComplete() } -> std::same_as<void>;
    { s.Invalidate() } -> std::same_as<void>;
};

}  // namespace dbn_pipe
```

**Step 4: Run test to verify it passes**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target sink_test && ./build/tests/sink_test`
Expected: PASS

**Step 5: Commit**

```bash
git add lib/stream/sink.hpp tests/sink_test.cpp
git commit -m "feat(sink): add Sink concept for lifecycle methods"
```

---

### Task 2: Add StreamingSink Concept

**Files:**
- Modify: `lib/stream/sink.hpp`
- Modify: `tests/sink_test.cpp`

**Step 1: Write the failing test for StreamingSink**

Add to `tests/sink_test.cpp`:

```cpp
#include "dbn_pipe/record_batch.hpp"

// A type that should satisfy StreamingSink
struct MockStreamingSink {
    void OnData(RecordBatch&&) {}
    void OnError(const Error&) {}
    void OnComplete() {}
    void Invalidate() {}
};

static_assert(StreamingSink<MockStreamingSink>, "MockStreamingSink must satisfy StreamingSink");

TEST(SinkConceptTest, StreamingSinkSatisfiesConcept) {
    SUCCEED();
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target sink_test && ./build/tests/sink_test`
Expected: FAIL with "StreamingSink is not defined"

**Step 3: Add StreamingSink concept to sink.hpp**

Add after `Sink` concept:

```cpp
// Forward declaration
class RecordBatch;

// Streaming sink - receives batches of records
template<typename S>
concept StreamingSink = Sink<S> && requires(S& s, RecordBatch&& batch) {
    { s.OnData(std::move(batch)) } -> std::same_as<void>;
};
```

**Step 4: Run test to verify it passes**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target sink_test && ./build/tests/sink_test`
Expected: PASS

**Step 5: Commit**

```bash
git add lib/stream/sink.hpp tests/sink_test.cpp
git commit -m "feat(sink): add StreamingSink concept for batch delivery"
```

---

### Task 3: Implement RecordSink Class

**Files:**
- Modify: `lib/stream/sink.hpp`
- Modify: `tests/sink_test.cpp`

**Step 1: Write the failing test for RecordSink**

Add to `tests/sink_test.cpp`:

```cpp
TEST(RecordSinkTest, DeliversDataToCallback) {
    bool data_called = false;
    RecordBatch captured_batch;

    RecordSink sink(
        [&](RecordBatch&& batch) { data_called = true; captured_batch = std::move(batch); },
        [](const Error&) {},
        []() {}
    );

    RecordBatch batch;
    sink.OnData(std::move(batch));

    EXPECT_TRUE(data_called);
}

TEST(RecordSinkTest, InvalidateStopsCallbacks) {
    bool data_called = false;

    RecordSink sink(
        [&](RecordBatch&&) { data_called = true; },
        [](const Error&) {},
        []() {}
    );

    sink.Invalidate();

    RecordBatch batch;
    sink.OnData(std::move(batch));

    EXPECT_FALSE(data_called);
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target sink_test && ./build/tests/sink_test`
Expected: FAIL with "RecordSink is not defined"

**Step 3: Implement RecordSink class**

Add to `lib/stream/sink.hpp`:

```cpp
#include "dbn_pipe/record_batch.hpp"

// RecordSink - concrete streaming sink implementation
class RecordSink {
public:
    RecordSink(
        std::function<void(RecordBatch&&)> on_data,
        std::function<void(const Error&)> on_error,
        std::function<void()> on_complete
    ) : on_data_(std::move(on_data)),
        on_error_(std::move(on_error)),
        on_complete_(std::move(on_complete)) {}

    void OnData(RecordBatch&& batch) {
        if (valid_.load(std::memory_order_acquire)) on_data_(std::move(batch));
    }

    void OnError(const Error& e) {
        if (valid_.load(std::memory_order_acquire)) on_error_(e);
    }

    void OnComplete() {
        if (valid_.load(std::memory_order_acquire)) on_complete_();
    }

    void Invalidate() { valid_.store(false, std::memory_order_release); }

private:
    std::function<void(RecordBatch&&)> on_data_;
    std::function<void(const Error&)> on_error_;
    std::function<void()> on_complete_;
    std::atomic<bool> valid_{true};
};

static_assert(StreamingSink<RecordSink>, "RecordSink must satisfy StreamingSink");
```

**Step 4: Run test to verify it passes**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target sink_test && ./build/tests/sink_test`
Expected: PASS

**Step 5: Commit**

```bash
git add lib/stream/sink.hpp tests/sink_test.cpp
git commit -m "feat(sink): implement RecordSink for streaming protocols"
```

---

### Task 4: Add SingleResultSink Concept and ResultSink Class

**Files:**
- Modify: `lib/stream/sink.hpp`
- Modify: `tests/sink_test.cpp`

**Step 1: Write the failing test for ResultSink**

Add to `tests/sink_test.cpp`:

```cpp
#include <expected>
#include <string>

TEST(ResultSinkTest, DeliversResultToCallback) {
    std::expected<std::string, Error> captured;

    ResultSink<std::string> sink([&](std::expected<std::string, Error> result) {
        captured = std::move(result);
    });

    sink.OnResult("success");

    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(*captured, "success");
}

TEST(ResultSinkTest, DeliversErrorViaOnError) {
    std::expected<std::string, Error> captured;

    ResultSink<std::string> sink([&](std::expected<std::string, Error> result) {
        captured = std::move(result);
    });

    sink.OnError(Error{ErrorCode::ConnectionFailed, "test error"});

    ASSERT_FALSE(captured.has_value());
    EXPECT_EQ(captured.error().code, ErrorCode::ConnectionFailed);
}

TEST(ResultSinkTest, ExactlyOnceDelivery) {
    int call_count = 0;

    ResultSink<int> sink([&](std::expected<int, Error>) {
        ++call_count;
    });

    sink.OnResult(1);
    sink.OnResult(2);  // Should be ignored
    sink.OnError(Error{ErrorCode::Unknown, "late"});  // Should be ignored

    EXPECT_EQ(call_count, 1);
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target sink_test && ./build/tests/sink_test`
Expected: FAIL with "ResultSink is not defined"

**Step 3: Add SingleResultSink concept and ResultSink class**

Add to `lib/stream/sink.hpp`:

```cpp
#include <expected>

// Single-result sink - receives one result (success or error via expected)
template<typename S>
concept SingleResultSink = Sink<S> && requires(S& s) {
    typename S::ResultType;
    { s.OnResult(std::declval<typename S::ResultType>()) } -> std::same_as<void>;
};

// ResultSink - concrete single-result sink implementation
template<typename Result>
class ResultSink {
public:
    using ResultType = Result;

    explicit ResultSink(std::function<void(std::expected<Result, Error>)> on_result)
        : on_result_(std::move(on_result)) {}

    void OnResult(Result&& result) {
        if (!valid_.load(std::memory_order_acquire) || delivered_) return;
        delivered_ = true;
        on_result_(std::move(result));
    }

    void OnError(const Error& e) {
        if (!valid_.load(std::memory_order_acquire) || delivered_) return;
        delivered_ = true;
        on_result_(std::unexpected(e));
    }

    void OnComplete() {
        // No-op for single-result - result already delivered
    }

    void Invalidate() { valid_.store(false, std::memory_order_release); }

private:
    std::function<void(std::expected<Result, Error>)> on_result_;
    std::atomic<bool> valid_{true};
    bool delivered_ = false;
};

template<typename R>
static_assert(SingleResultSink<ResultSink<R>>, "ResultSink<R> must satisfy SingleResultSink");
```

**Step 4: Run test to verify it passes**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target sink_test && ./build/tests/sink_test`
Expected: PASS

**Step 5: Commit**

```bash
git add lib/stream/sink.hpp tests/sink_test.cpp
git commit -m "feat(sink): add SingleResultSink concept and ResultSink class"
```

---

### Task 5: Create Pipeline Forward Declaration

**Files:**
- Create: `lib/stream/pipeline_fwd.hpp`

**Step 1: Write the forward declaration header**

```cpp
// lib/stream/pipeline_fwd.hpp
#pragma once

namespace dbn_pipe {

// Forward declaration of Pipeline template
// Used by Protocol headers to declare return type of Create()
template<typename P>
class Pipeline;

}  // namespace dbn_pipe
```

**Step 2: Commit**

```bash
git add lib/stream/pipeline_fwd.hpp
git commit -m "feat(pipeline): add forward declaration header"
```

---

### Task 6: Create Protocol Concept

**Files:**
- Create: `lib/stream/protocol.hpp`
- Create: `tests/protocol_concept_test.cpp`

**Step 1: Write the failing test for Protocol concept**

```cpp
// tests/protocol_concept_test.cpp
#include <gtest/gtest.h>

#include "dbn_pipe/stream/protocol.hpp"
#include "dbn_pipe/stream/sink.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/event_loop.hpp"

using namespace dbn_pipe;

// Mock types for testing
struct MockRequest {};
struct MockChain {
    void Connect(const sockaddr_storage&) {}
    void Close() {}
    void SetReadyCallback(std::function<void()>) {}
    void Suspend() {}
    void Resume() {}
};

struct MockSink {
    void OnError(const Error&) {}
    void OnComplete() {}
    void Invalidate() {}
};

// A minimal protocol that should satisfy the concept
struct MockProtocol {
    using Request = MockRequest;
    using SinkType = MockSink;
    using ChainType = MockChain;

    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop&, SinkType&, const std::string&) {
        return std::make_shared<ChainType>();
    }

    static void SendRequest(std::shared_ptr<ChainType>&, const Request&) {}
    static void Teardown(std::shared_ptr<ChainType>&) {}
    static std::string GetHostname(const Request&) { return "test.com"; }
    static uint16_t GetPort(const Request&) { return 443; }
};

static_assert(Protocol<MockProtocol>, "MockProtocol must satisfy Protocol concept");

TEST(ProtocolConceptTest, MockProtocolSatisfiesConcept) {
    SUCCEED();
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target protocol_concept_test && ./build/tests/protocol_concept_test`
Expected: FAIL with "No such file or directory" for protocol.hpp

**Step 3: Write protocol.hpp with Protocol concept**

```cpp
// lib/stream/protocol.hpp
#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>

#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/sink.hpp"

namespace dbn_pipe {

// Protocol concept - defines the interface for protocol implementations
//
// Protocol is a static factory that defines types and construction.
// The Pipeline template uses these static methods to handle protocol-specific behavior.
template<typename P>
concept Protocol = requires {
    // Required type aliases
    typename P::Request;
    typename P::SinkType;
    typename P::ChainType;

    // SinkType must satisfy Sink concept
    requires Sink<typename P::SinkType>;

    // ChainType must support network lifecycle and backpressure
    requires requires(
        typename P::ChainType& chain,
        const sockaddr_storage& addr,
        std::function<void()> cb
    ) {
        { chain.Connect(addr) } -> std::same_as<void>;
        { chain.Close() } -> std::same_as<void>;
        { chain.SetReadyCallback(cb) } -> std::same_as<void>;
        { chain.Suspend() } -> std::same_as<void>;
        { chain.Resume() } -> std::same_as<void>;
    };

    // Chain building (sink passed by reference - Pipeline owns sink)
    requires requires(
        IEventLoop& loop,
        typename P::SinkType& sink,
        const std::string& api_key
    ) {
        { P::BuildChain(loop, sink, api_key) }
            -> std::same_as<std::shared_ptr<typename P::ChainType>>;
    };

    // Chain operations
    requires requires(
        std::shared_ptr<typename P::ChainType>& chain,
        const typename P::Request& request
    ) {
        { P::SendRequest(chain, request) } -> std::same_as<void>;
        { P::Teardown(chain) } -> std::same_as<void>;
        { P::GetHostname(request) } -> std::convertible_to<std::string>;
        { P::GetPort(request) } -> std::convertible_to<uint16_t>;
    };
};

}  // namespace dbn_pipe
```

**Step 4: Run test to verify it passes**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target protocol_concept_test && ./build/tests/protocol_concept_test`
Expected: PASS

**Step 5: Commit**

```bash
git add lib/stream/protocol.hpp tests/protocol_concept_test.cpp
git commit -m "feat(protocol): add Protocol concept for static factory interface"
```

---

### Task 7: Create Generic Pipeline Template

**Files:**
- Create: `lib/stream/pipeline.hpp`
- Create: `tests/generic_pipeline_test.cpp`

**Step 1: Write the failing test for Pipeline**

```cpp
// tests/generic_pipeline_test.cpp
#include <gtest/gtest.h>

#include "dbn_pipe/stream/pipeline.hpp"
#include "dbn_pipe/stream/epoll_event_loop.hpp"

using namespace dbn_pipe;

// Mock types from protocol_concept_test
struct MockRequest {};

struct MockChain {
    void Connect(const sockaddr_storage&) { connect_called = true; }
    void Close() { close_called = true; }
    void SetReadyCallback(std::function<void()> cb) { ready_cb = std::move(cb); }
    void Suspend() { suspended = true; }
    void Resume() { suspended = false; }
    bool IsSuspended() const { return suspended; }

    bool connect_called = false;
    bool close_called = false;
    bool suspended = false;
    std::function<void()> ready_cb;
};

struct MockSink {
    void OnError(const Error&) {}
    void OnComplete() {}
    void Invalidate() { invalidated = true; }
    bool invalidated = false;
};

struct MockProtocol {
    using Request = MockRequest;
    using SinkType = MockSink;
    using ChainType = MockChain;

    static inline std::shared_ptr<MockChain> last_chain;
    static inline MockSink* last_sink;

    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop&, SinkType& sink, const std::string&) {
        last_chain = std::make_shared<ChainType>();
        last_sink = &sink;
        return last_chain;
    }

    static void SendRequest(std::shared_ptr<ChainType>&, const Request&) {}
    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) chain->Close();
    }
    static std::string GetHostname(const Request&) { return "test.com"; }
    static uint16_t GetPort(const Request&) { return 443; }
};

TEST(GenericPipelineTest, CreateReturnsPipeline) {
    EpollEventLoop loop;
    loop.Poll(0);

    auto sink = std::make_shared<MockSink>();
    auto chain = std::make_shared<MockChain>();

    auto pipeline = std::make_shared<Pipeline<MockProtocol>>(
        Pipeline<MockProtocol>::PrivateTag{},
        loop, chain, sink, MockRequest{});

    EXPECT_NE(pipeline, nullptr);
}

TEST(GenericPipelineTest, StopInvalidatesSink) {
    EpollEventLoop loop;
    loop.Poll(0);

    auto sink = std::make_shared<MockSink>();
    auto chain = std::make_shared<MockChain>();

    auto pipeline = std::make_shared<Pipeline<MockProtocol>>(
        Pipeline<MockProtocol>::PrivateTag{},
        loop, chain, sink, MockRequest{});

    pipeline->Stop();

    EXPECT_TRUE(sink->invalidated);
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target generic_pipeline_test && ./build/tests/generic_pipeline_test`
Expected: FAIL with "No such file or directory" for pipeline.hpp

**Step 3: Write pipeline.hpp**

```cpp
// lib/stream/pipeline.hpp
#pragma once

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/protocol.hpp"
#include "dbn_pipe/dns_resolver.hpp"

namespace dbn_pipe {

// Pipeline<P> - Runtime shell for any Protocol.
//
// Pipeline is a thin runtime wrapper - just lifecycle management.
// The Protocol provides construction logic via static methods.
//
// Thread safety:
// - All public methods must be called from event loop thread (enforced via RequireLoopThread)
// - IsSuspended() is thread-safe, can be called from any thread
//
// Ownership:
// - Pipeline is the root object, owns both chain and sink
// - Destructor controls teardown order: invalidate sink → teardown chain
template<typename P>
    requires Protocol<P>
class Pipeline : public Suspendable {
public:
    struct PrivateTag {};  // Force use of Protocol::Create()

    Pipeline(
        PrivateTag,
        IEventLoop& loop,
        std::shared_ptr<typename P::ChainType> chain,
        std::shared_ptr<typename P::SinkType> sink,
        typename P::Request request
    ) : loop_(loop),
        chain_(std::move(chain)),
        sink_(std::move(sink)),
        request_(std::move(request)) {}

    ~Pipeline() {
        DoTeardown();
    }

    // Connect using protocol-derived hostname/port
    void Connect() {
        RequireLoopThread(__func__);
        if (torn_down_) return;

        std::string hostname = P::GetHostname(request_);
        uint16_t port = P::GetPort(request_);

        auto addr = ResolveHostname(hostname, port);
        if (!addr) {
            if (sink_) sink_->OnError(Error{ErrorCode::DnsResolutionFailed,
                "Failed to resolve hostname: " + hostname});
            DoTeardown();
            return;
        }

        Connect(*addr);
    }

    // Connect to specific address
    void Connect(const sockaddr_storage& addr) {
        RequireLoopThread(__func__);
        if (torn_down_) return;
        if (chain_) chain_->Connect(addr);
    }

    // Start streaming (sends protocol request)
    void Start() {
        RequireLoopThread(__func__);
        if (torn_down_) return;
        P::SendRequest(chain_, request_);
    }

    // Stop and teardown
    void Stop() {
        RequireLoopThread(__func__);
        DoTeardown();
    }

    // Suspendable interface
    void Suspend() override {
        RequireLoopThread(__func__);
        if (!torn_down_ && chain_) chain_->Suspend();
    }

    void Resume() override {
        RequireLoopThread(__func__);
        if (!torn_down_ && chain_) chain_->Resume();
    }

    void Close() override {
        RequireLoopThread(__func__);
        DoTeardown();
    }

    bool IsSuspended() const override {
        // Thread-safe - can be called from any thread
        return chain_ && chain_->IsSuspended();
    }

private:
    // Fail-fast thread check - works in release builds
    void RequireLoopThread(const char* func) const {
        if (loop_.IsInEventLoopThread()) return;
        std::fprintf(stderr, "Pipeline::%s called off event loop thread\n", func);
        std::terminate();
    }

    // Idempotent teardown - safe to call multiple times
    void DoTeardown() {
        if (torn_down_) return;
        torn_down_ = true;

        // Order matters: invalidate sink first, then teardown chain
        if (sink_) sink_->Invalidate();
        if (chain_) P::Teardown(chain_);
    }

    IEventLoop& loop_;
    std::shared_ptr<typename P::ChainType> chain_;
    std::shared_ptr<typename P::SinkType> sink_;
    typename P::Request request_;
    bool torn_down_ = false;
};

}  // namespace dbn_pipe
```

**Step 4: Run test to verify it passes**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target generic_pipeline_test && ./build/tests/generic_pipeline_test`
Expected: PASS

**Step 5: Commit**

```bash
git add lib/stream/pipeline.hpp tests/generic_pipeline_test.cpp
git commit -m "feat(pipeline): implement generic Pipeline<Protocol> template"
```

---

## Phase 2: Move Reusable Components to lib/stream/

### Task 8: Move url_encode.hpp

**Files:**
- Move: `src/api/url_encode.hpp` → `lib/stream/url_encode.hpp`
- Modify: All files that include the old path

**Step 1: Copy file to new location**

```bash
cp src/api/url_encode.hpp lib/stream/url_encode.hpp
```

**Step 2: Update include path in file header comment**

Edit `lib/stream/url_encode.hpp` line 1:
```cpp
// lib/stream/url_encode.hpp
```

**Step 3: Find and update all includes**

Run: `grep -r 'api/url_encode.hpp' --include='*.hpp' --include='*.cpp'`

Update each file to use `lib/stream/url_encode.hpp`

**Step 4: Run tests**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build && ctest --output-on-failure`
Expected: All tests pass

**Step 5: Remove old file and commit**

```bash
git rm src/api/url_encode.hpp
git add lib/stream/url_encode.hpp
git add -u  # Stage modified files
git commit -m "refactor: move url_encode.hpp to lib/stream/"
```

---

### Task 9: Move tls_transport.hpp

**Files:**
- Move: `src/tls_transport.hpp` → `lib/stream/tls_transport.hpp`
- Modify: All files that include the old path

**Step 1: Copy file to new location**

```bash
cp src/tls_transport.hpp lib/stream/tls_transport.hpp
```

**Step 2: Update include path in file header comment**

Edit `lib/stream/tls_transport.hpp` line 1:
```cpp
// lib/stream/tls_transport.hpp
```

**Step 3: Find and update all includes**

Run: `grep -r 'src/tls_transport.hpp' --include='*.hpp' --include='*.cpp'`

Update each file to use `lib/stream/tls_transport.hpp`

**Step 4: Run tests**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build && ctest --output-on-failure`
Expected: All tests pass

**Step 5: Remove old file and commit**

```bash
git rm src/tls_transport.hpp
git add lib/stream/tls_transport.hpp
git add -u
git commit -m "refactor: move tls_transport.hpp to lib/stream/"
```

---

### Task 10: Move http_client.hpp

**Files:**
- Move: `src/http_client.hpp` → `lib/stream/http_client.hpp`
- Modify: All files that include the old path

**Step 1: Copy file to new location**

```bash
cp src/http_client.hpp lib/stream/http_client.hpp
```

**Step 2: Update include path in file header comment**

**Step 3: Find and update all includes**

Run: `grep -r 'src/http_client.hpp' --include='*.hpp' --include='*.cpp'`

**Step 4: Run tests**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build && ctest --output-on-failure`
Expected: All tests pass

**Step 5: Remove old file and commit**

```bash
git rm src/http_client.hpp
git add lib/stream/http_client.hpp
git add -u
git commit -m "refactor: move http_client.hpp to lib/stream/"
```

---

### Task 11: Move zstd_decompressor.hpp

**Files:**
- Move: `src/zstd_decompressor.hpp` → `lib/stream/zstd_decompressor.hpp`
- Modify: All files that include the old path

**Step 1: Copy file to new location**

```bash
cp src/zstd_decompressor.hpp lib/stream/zstd_decompressor.hpp
```

**Step 2: Update include path in file header comment**

**Step 3: Find and update all includes**

Run: `grep -r 'src/zstd_decompressor.hpp' --include='*.hpp' --include='*.cpp'`

**Step 4: Run tests**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build && ctest --output-on-failure`
Expected: All tests pass

**Step 5: Remove old file and commit**

```bash
git rm src/zstd_decompressor.hpp
git add lib/stream/zstd_decompressor.hpp
git add -u
git commit -m "refactor: move zstd_decompressor.hpp to lib/stream/"
```

---

### Task 12: Move json_parser.hpp

**Files:**
- Move: `src/api/json_parser.hpp` → `lib/stream/json_parser.hpp`
- Modify: All files that include the old path

**Step 1: Copy file to new location**

```bash
cp src/api/json_parser.hpp lib/stream/json_parser.hpp
```

**Step 2: Update include path in file header comment**

**Step 3: Find and update all includes**

Run: `grep -r 'api/json_parser.hpp' --include='*.hpp' --include='*.cpp'`

**Step 4: Run tests**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build && ctest --output-on-failure`
Expected: All tests pass

**Step 5: Remove old file and commit**

```bash
git rm src/api/json_parser.hpp
git add lib/stream/json_parser.hpp
git add -u
git commit -m "refactor: move json_parser.hpp to lib/stream/"
```

---

## Phase 3: Update Protocols to New Design

### Task 13: Update HistoricalProtocol with Create() Factory

**Files:**
- Modify: `src/historical_protocol.hpp`
- Create: `tests/historical_protocol_create_test.cpp`

**Step 1: Write the failing test**

```cpp
// tests/historical_protocol_create_test.cpp
#include <gtest/gtest.h>

#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/historical_protocol.hpp"

using namespace dbn_pipe;

TEST(HistoricalProtocolCreateTest, CreateReturnsPipeline) {
    EpollEventLoop loop;
    loop.Poll(0);

    bool data_called = false;
    bool error_called = false;
    bool complete_called = false;

    auto pipeline = HistoricalProtocol::Create(
        loop,
        "test_api_key",
        HistoricalRequest{
            .dataset = "GLBX.MDP3",
            .symbols = "ESZ4",
            .schema = "mbp-1",
            .start = 0,
            .end = 1000000000
        },
        [&](RecordBatch&&) { data_called = true; },
        [&](const Error&) { error_called = true; },
        [&]() { complete_called = true; }
    );

    EXPECT_NE(pipeline, nullptr);
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target historical_protocol_create_test && ./build/tests/historical_protocol_create_test`
Expected: FAIL with "Create is not a member of HistoricalProtocol"

**Step 3: Add Create() factory to HistoricalProtocol**

Add to `src/historical_protocol.hpp` after existing `ChainImpl`:

```cpp
#include "dbn_pipe/stream/pipeline.hpp"
#include "dbn_pipe/stream/sink.hpp"

// Add SinkType alias
using SinkType = RecordSink;

// Add Create() factory method
static auto Create(
    IEventLoop& loop,
    std::string api_key,
    Request request,
    std::function<void(RecordBatch&&)> on_data,
    std::function<void(const Error&)> on_error,
    std::function<void()> on_complete
) -> std::shared_ptr<Pipeline<HistoricalProtocol>>
{
    auto sink = std::make_shared<RecordSink>(
        std::move(on_data), std::move(on_error), std::move(on_complete));
    auto chain = BuildChain(loop, *sink, api_key, request.dataset);
    return std::make_shared<Pipeline<HistoricalProtocol>>(
        Pipeline<HistoricalProtocol>::PrivateTag{},
        loop, std::move(chain), std::move(sink), std::move(request));
}

// Update BuildChain to work with RecordSink
static std::shared_ptr<ChainType> BuildChain(
    IEventLoop& loop,
    RecordSink& sink,
    const std::string& api_key,
    const std::string& /*dataset*/ = {}
) {
    return std::make_shared<ChainImpl<RecordRef>>(loop, /* adapt sink */, api_key);
}
```

Note: This requires adapting the existing `Sink<Record>` usage to `RecordSink`. The `ChainImpl` template needs updating.

**Step 4: Run test to verify it passes**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target historical_protocol_create_test && ./build/tests/historical_protocol_create_test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/historical_protocol.hpp tests/historical_protocol_create_test.cpp
git commit -m "feat(historical): add Create() factory for new Pipeline design"
```

---

### Task 14: Update LiveProtocol with Create() Factory

**Files:**
- Modify: `src/live_protocol.hpp`
- Create: `tests/live_protocol_create_test.cpp`

**Step 1: Write the failing test**

```cpp
// tests/live_protocol_create_test.cpp
#include <gtest/gtest.h>

#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/live_protocol.hpp"

using namespace dbn_pipe;

TEST(LiveProtocolCreateTest, CreateReturnsPipeline) {
    EpollEventLoop loop;
    loop.Poll(0);

    auto pipeline = LiveProtocol::Create(
        loop,
        "test_api_key",
        LiveRequest{
            .dataset = "GLBX.MDP3",
            .symbols = "ESZ4",
            .schema = "mbp-1"
        },
        [](RecordBatch&&) {},
        [](const Error&) {},
        []() {}
    );

    EXPECT_NE(pipeline, nullptr);
}
```

**Step 2: Run test to verify it fails**

**Step 3: Add Create() factory to LiveProtocol**

Similar pattern to HistoricalProtocol.

**Step 4: Run test to verify it passes**

**Step 5: Commit**

```bash
git add src/live_protocol.hpp tests/live_protocol_create_test.cpp
git commit -m "feat(live): add Create() factory for new Pipeline design"
```

---

### Task 15: Create ApiProtocol Template

**Files:**
- Create: `src/api_protocol.hpp`
- Create: `tests/api_protocol_test.cpp`

**Step 1: Write the failing test**

```cpp
// tests/api_protocol_test.cpp
#include <gtest/gtest.h>

#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/api_protocol.hpp"
#include "dbn_pipe/api/metadata_client.hpp"  // For a real builder

using namespace dbn_pipe;

// Simple test builder
struct TestBuilder {
    using Result = std::string;

    void OnObjectStart() {}
    void OnObjectEnd() {}
    void OnArrayStart() {}
    void OnArrayEnd() {}
    void OnKey(std::string_view) {}
    void OnString(std::string_view s) { result = std::string(s); }
    void OnNumber(double) {}
    void OnBool(bool) {}
    void OnNull() {}

    Result Build() { return result; }

    std::string result;
};

TEST(ApiProtocolTest, CreateReturnsPipeline) {
    EpollEventLoop loop;
    loop.Poll(0);

    TestBuilder builder;

    auto pipeline = ApiProtocol<TestBuilder>::Create(
        loop,
        "test_api_key",
        ApiRequest{
            .method = "GET",
            .path = "/v0/test"
        },
        builder,
        [](std::expected<std::string, Error>) {}
    );

    EXPECT_NE(pipeline, nullptr);
}
```

**Step 2: Run test to verify it fails**

**Step 3: Implement ApiProtocol template**

```cpp
// src/api_protocol.hpp
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>

#include "dbn_pipe/stream/http_client.hpp"
#include "dbn_pipe/stream/json_parser.hpp"
#include "dbn_pipe/stream/pipeline.hpp"
#include "dbn_pipe/stream/pipeline_fwd.hpp"
#include "dbn_pipe/stream/sink.hpp"
#include "dbn_pipe/stream/tcp_socket.hpp"
#include "dbn_pipe/stream/tls_transport.hpp"
#include "dbn_pipe/api/api_pipeline.hpp"  // For ApiRequest

namespace dbn_pipe {

// ApiProtocol<Builder> - Protocol for JSON API requests
//
// Satisfies the Protocol concept. Uses TLS -> HTTP -> JSON parser chain.
// Builder must satisfy the JsonBuilder concept.
template<typename Builder>
struct ApiProtocol {
    using Request = ApiRequest;
    using Result = typename Builder::Result;
    using SinkType = ResultSink<Result>;

    // Chain wraps TcpSocket -> TlsTransport -> HttpClient -> JsonParser
    struct ChainType {
        // ... similar to HistoricalProtocol::ChainType
    };

    static auto Create(
        IEventLoop& loop,
        std::string api_key,
        Request request,
        Builder& builder,
        std::function<void(std::expected<Result, Error>)> on_result
    ) -> std::shared_ptr<Pipeline<ApiProtocol<Builder>>>;

    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop& loop, SinkType& sink, const std::string& api_key);

    static void SendRequest(std::shared_ptr<ChainType>& chain, const Request& req);
    static void Teardown(std::shared_ptr<ChainType>& chain);
    static std::string GetHostname(const Request&) { return "hist.databento.com"; }
    static uint16_t GetPort(const Request&) { return 443; }
};

}  // namespace dbn_pipe
```

**Step 4: Run test to verify it passes**

**Step 5: Commit**

```bash
git add src/api_protocol.hpp tests/api_protocol_test.cpp
git commit -m "feat(api): add ApiProtocol template for JSON API requests"
```

---

## Phase 4: Update API Clients

### Task 16: Update MetadataClient to Use ApiProtocol

**Files:**
- Modify: `src/api/metadata_client.hpp`
- Modify: `tests/metadata_client_test.cpp`

**Step 1: Run existing tests to establish baseline**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target metadata_client_test && ./build/tests/metadata_client_test`
Expected: All tests pass

**Step 2: Update MetadataClient to use ApiProtocol**

Replace `ApiPipeline<Builder>` usage with `ApiProtocol<Builder>::Create()`.

**Step 3: Run tests**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target metadata_client_test && ./build/tests/metadata_client_test`
Expected: All tests pass

**Step 4: Commit**

```bash
git add src/api/metadata_client.hpp
git commit -m "refactor(metadata): update to use ApiProtocol"
```

---

### Task 17: Update SymbologyClient to Use ApiProtocol

**Files:**
- Modify: `src/api/symbology_client.hpp`
- Modify: `tests/symbology_client_test.cpp`

**Step 1: Run existing tests to establish baseline**

**Step 2: Update SymbologyClient to use ApiProtocol**

**Step 3: Run tests**

**Step 4: Commit**

```bash
git add src/api/symbology_client.hpp
git commit -m "refactor(symbology): update to use ApiProtocol"
```

---

## Phase 5: Cleanup

### Task 18: Remove Old Pipeline Files

**Files:**
- Remove: `src/pipeline.hpp`
- Remove: `src/pipeline_sink.hpp`
- Remove: `src/protocol_driver.hpp`
- Remove: `src/api/api_pipeline.hpp`

**Step 1: Find remaining usages**

Run: `grep -r 'src/pipeline.hpp\|src/pipeline_sink.hpp\|src/protocol_driver.hpp\|api/api_pipeline.hpp' --include='*.hpp' --include='*.cpp'`

**Step 2: Update any remaining includes**

**Step 3: Remove old files**

```bash
git rm src/pipeline.hpp src/pipeline_sink.hpp src/protocol_driver.hpp src/api/api_pipeline.hpp
```

**Step 4: Run all tests**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build && ctest --output-on-failure`
Expected: All tests pass

**Step 5: Commit**

```bash
git commit -m "refactor: remove legacy pipeline files replaced by unified design"
```

---

### Task 19: Update Test Files

**Files:**
- Modify: `tests/pipeline_test.cpp` - update to new API
- Modify: `tests/pipeline_sink_test.cpp` - update to new sink types
- Remove: `tests/api_pipeline_test.cpp` - if functionality covered by new tests

**Step 1: Run all tests**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build && ctest --output-on-failure`

**Step 2: Fix any failing tests**

**Step 3: Commit**

```bash
git add -u tests/
git commit -m "test: update tests for unified pipeline design"
```

---

### Task 20: Final Verification

**Step 1: Run full test suite**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build && ctest --output-on-failure`
Expected: All tests pass

**Step 2: Run linter/formatter if available**

Run: `cd /home/kai/work/dbn-pipe && cmake --build build --target format` (if exists)

**Step 3: Verify directory structure**

```bash
ls -la lib/stream/
ls -la src/
ls -la src/api/
```

Expected structure per design doc.

**Step 4: Final commit if needed**

```bash
git status
# If clean, no commit needed
```

---

## Summary

| Phase | Tasks | Purpose |
|-------|-------|---------|
| 1 | 1-7 | Create foundation: Sink concepts, Protocol concept, Pipeline template |
| 2 | 8-12 | Move reusable components to lib/stream/ |
| 3 | 13-15 | Update protocols to new design with Create() factories |
| 4 | 16-17 | Update API clients to use ApiProtocol |
| 5 | 18-20 | Cleanup old files and verify |

**Total Tasks:** 20

**Key Files Created:**
- `lib/stream/sink.hpp` - Sink concepts and implementations
- `lib/stream/protocol.hpp` - Protocol concept
- `lib/stream/pipeline.hpp` - Generic Pipeline template
- `lib/stream/pipeline_fwd.hpp` - Forward declaration
- `src/api_protocol.hpp` - ApiProtocol for JSON APIs

**Key Files Moved:**
- `url_encode.hpp` → `lib/stream/`
- `tls_transport.hpp` → `lib/stream/`
- `http_client.hpp` → `lib/stream/`
- `zstd_decompressor.hpp` → `lib/stream/`
- `json_parser.hpp` → `lib/stream/`

**Key Files Removed:**
- `src/pipeline.hpp` (replaced by `lib/stream/pipeline.hpp`)
- `src/pipeline_sink.hpp` (replaced by `lib/stream/sink.hpp`)
- `src/protocol_driver.hpp` (replaced by `lib/stream/protocol.hpp`)
- `src/api/api_pipeline.hpp` (replaced by `src/api_protocol.hpp`)
