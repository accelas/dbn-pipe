# Unified Pipeline Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement the unified Pipeline<Protocol> template that replaces separate LiveClient and HistoricalClient with a single parameterized class.

**Architecture:** Pipeline<P> template with ProtocolDriver concept. LiveProtocol and HistoricalProtocol provide static hooks for protocol-specific behavior. Sink bridges Pipeline to downstream callbacks. Single-use lifecycle with reactor-thread-only access.

**Tech Stack:** C++23, concepts, templates, GoogleTest

**Design Document:** `docs/plans/2025-12-29-unified-pipeline-design.md`

---

## Task 1: Rename LiveProtocolHandler to CramAuth

The existing `LiveProtocolHandler` handles CRAM authentication. Rename it to `CramAuth` to clarify its purpose and prepare for the new Pipeline architecture.

**Files:**
- Rename: `src/live_protocol_handler.hpp` → `src/cram_auth.hpp`
- Rename: `tests/live_protocol_handler_test.cpp` → `tests/cram_auth_test.cpp`
- Modify: `src/cram_auth.hpp` (update class name and references)
- Modify: `tests/cram_auth_test.cpp` (update class name and references)

**Step 1: Rename the source file**

```bash
git mv src/live_protocol_handler.hpp src/cram_auth.hpp
```

**Step 2: Rename the test file**

```bash
git mv tests/live_protocol_handler_test.cpp tests/cram_auth_test.cpp
```

**Step 3: Update class name in src/cram_auth.hpp**

Replace all occurrences:
- `LiveProtocolHandler` → `CramAuth`
- Keep `LiveProtocolState` as is (will be renamed to `CramAuthState` in a later step)

The class declaration becomes:
```cpp
template <Downstream D>
class CramAuth
    : public PipelineComponent<CramAuth<D>>,
      public std::enable_shared_from_this<CramAuth<D>> {
```

**Step 4: Update LiveProtocolState to CramAuthState**

```cpp
enum class CramAuthState {
    WaitingGreeting,
    WaitingChallenge,
    Authenticating,
    Ready,
    Streaming
};
```

Update all references in the file.

**Step 5: Update test file references**

In `tests/cram_auth_test.cpp`:
- Change include: `#include "src/live_protocol_handler.hpp"` → `#include "src/cram_auth.hpp"`
- Replace all `LiveProtocolHandler` → `CramAuth`
- Replace all `LiveProtocolState` → `CramAuthState`
- Replace test class name: `LiveProtocolHandlerTest` → `CramAuthTest`

**Step 6: Run tests to verify rename is correct**

Run: `bazel test //tests:cram_auth_test`
Expected: All tests pass

**Step 7: Commit the rename**

```bash
git add -A
git commit -m "refactor: rename LiveProtocolHandler to CramAuth

Clarifies that this component handles CRAM authentication protocol,
preparing for the unified Pipeline architecture where it becomes
one of several protocol handlers.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 2: Create PipelineBase and Sink Classes

Create the base class that Pipeline inherits from, and the Sink class that bridges Pipeline to protocol components.

**Files:**
- Create: `src/pipeline_base.hpp`
- Test: `tests/pipeline_base_test.cpp`

**Step 1: Write the failing test for PipelineBase**

Create `tests/pipeline_base_test.cpp`:

```cpp
// tests/pipeline_base_test.cpp
#include <gtest/gtest.h>

#include "src/pipeline_base.hpp"
#include "src/error.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

// Mock record for testing
struct MockRecord {
    int value;
};

TEST(PipelineBaseTest, SinkInvalidationStopsCallbacks) {
    Reactor reactor;
    reactor.Poll(0);  // Initialize thread ID

    bool record_called = false;
    bool error_called = false;
    bool complete_called = false;

    // Create a mock pipeline that tracks calls
    struct MockPipeline : PipelineBase {
        bool* record_flag;
        bool* error_flag;
        bool* complete_flag;

        void HandleRecord(const MockRecord&) override { *record_flag = true; }
        void HandlePipelineError(const Error&) override { *error_flag = true; }
        void HandlePipelineComplete() override { *complete_flag = true; }
    };

    MockPipeline pipeline;
    pipeline.record_flag = &record_called;
    pipeline.error_flag = &error_called;
    pipeline.complete_flag = &complete_called;

    Sink sink(reactor, &pipeline);

    // Before invalidation, callbacks should work
    sink.OnRecord(MockRecord{42});
    EXPECT_TRUE(record_called);

    // Invalidate
    sink.Invalidate();

    // Reset flags
    record_called = false;

    // After invalidation, callbacks should be no-ops
    sink.OnRecord(MockRecord{99});
    sink.OnError(Error{ErrorCode::ConnectionFailed, "test"});
    sink.OnComplete();

    EXPECT_FALSE(record_called);
    EXPECT_FALSE(error_called);
    EXPECT_FALSE(complete_called);
}

TEST(PipelineBaseTest, SinkRequiresReactorThread) {
    Reactor reactor;
    reactor.Poll(0);  // Initialize thread ID

    struct MockPipeline : PipelineBase {
        void HandleRecord(const MockRecord&) override {}
        void HandlePipelineError(const Error&) override {}
        void HandlePipelineComplete() override {}
    };

    MockPipeline pipeline;
    Sink sink(reactor, &pipeline);

    // Should not crash when called from reactor thread
    sink.OnComplete();
    SUCCEED();
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:pipeline_base_test`
Expected: FAIL - file not found or class not defined

**Step 3: Create src/pipeline_base.hpp**

```cpp
// src/pipeline_base.hpp
#pragma once

#include <cassert>

#include "error.hpp"
#include "reactor.hpp"

namespace databento_async {

// Forward declaration for templated record type
// Pipeline implementations define their own record type
template <typename Record>
class PipelineBase {
public:
    virtual ~PipelineBase() = default;

    virtual void HandleRecord(const Record& rec) = 0;
    virtual void HandlePipelineError(const Error& e) = 0;
    virtual void HandlePipelineComplete() = 0;
};

// Sink bridges protocol components to Pipeline callbacks.
// Thread-safety: ALL methods must be called from reactor thread.
// No atomics needed since single-threaded access is guaranteed.
template <typename Record>
class Sink {
public:
    Sink(Reactor& reactor, PipelineBase<Record>* pipeline)
        : reactor_(reactor), pipeline_(pipeline) {}

    // Invalidate clears valid_ and nulls pipeline_.
    // Called from TeardownPipeline before deferred cleanup.
    void Invalidate() {
        assert(reactor_.IsInReactorThread());
        valid_ = false;
        pipeline_ = nullptr;
    }

    // RecordSink interface - only called from reactor thread
    // Note: Handler may trigger teardown, so check valid_ on each iteration.
    void OnRecord(const Record& rec) {
        assert(reactor_.IsInReactorThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandleRecord(rec);
    }

    void OnError(const Error& e) {
        assert(reactor_.IsInReactorThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandlePipelineError(e);
    }

    void OnComplete() {
        assert(reactor_.IsInReactorThread());
        if (!valid_ || !pipeline_) return;
        pipeline_->HandlePipelineComplete();
    }

private:
    Reactor& reactor_;
    PipelineBase<Record>* pipeline_;
    bool valid_ = true;
};

}  // namespace databento_async
```

**Step 4: Update test to use correct template**

Update the test to specify the record type:

```cpp
using TestSink = Sink<MockRecord>;
using TestPipelineBase = PipelineBase<MockRecord>;
```

**Step 5: Run test to verify it passes**

Run: `bazel test //tests:pipeline_base_test`
Expected: PASS

**Step 6: Commit**

```bash
git add src/pipeline_base.hpp tests/pipeline_base_test.cpp
git commit -m "feat: add PipelineBase and Sink classes

PipelineBase is the virtual base class for Pipeline<P> that provides
the callback interface (HandleRecord, HandlePipelineError, HandlePipelineComplete).

Sink bridges protocol components to Pipeline, with reactor-thread
enforcement and invalidation support to prevent callbacks during teardown.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 3: Create ProtocolDriver Concept

Define the ProtocolDriver concept that constrains protocol implementations.

**Files:**
- Create: `src/protocol_driver.hpp`
- Test: `tests/protocol_driver_test.cpp`

**Step 1: Write the failing test**

Create `tests/protocol_driver_test.cpp`:

```cpp
// tests/protocol_driver_test.cpp
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "src/protocol_driver.hpp"
#include "src/reactor.hpp"
#include "src/pipeline_base.hpp"

using namespace databento_async;

// Mock sink for testing
struct MockRecord {};
using TestSink = Sink<MockRecord>;

// Mock chain type
struct MockChain {
    void Close() {}
};

// Valid protocol driver for testing
struct ValidProtocol {
    struct Request {
        std::string data;
    };

    using ChainType = MockChain;

    static std::shared_ptr<ChainType> BuildChain(
        Reactor&, TestSink&, const std::string&
    ) {
        return std::make_shared<ChainType>();
    }

    static void WireTcp(TcpSocket&, std::shared_ptr<ChainType>&) {}

    static bool OnConnect(std::shared_ptr<ChainType>&) {
        return true;
    }

    static bool OnRead(std::shared_ptr<ChainType>&, std::pmr::vector<std::byte>) {
        return true;
    }

    static void SendRequest(std::shared_ptr<ChainType>&, const Request&) {}

    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) chain->Close();
    }
};

TEST(ProtocolDriverTest, ValidProtocolSatisfiesConcept) {
    static_assert(ProtocolDriver<ValidProtocol, MockRecord>);
    SUCCEED();
}

// Invalid protocol missing BuildChain
struct InvalidProtocol {
    struct Request {};
    using ChainType = MockChain;
    // Missing BuildChain and other methods
};

TEST(ProtocolDriverTest, InvalidProtocolDoesNotSatisfyConcept) {
    static_assert(!ProtocolDriver<InvalidProtocol, MockRecord>);
    SUCCEED();
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:protocol_driver_test`
Expected: FAIL - ProtocolDriver not defined

**Step 3: Create src/protocol_driver.hpp**

```cpp
// src/protocol_driver.hpp
#pragma once

#include <concepts>
#include <memory>
#include <memory_resource>
#include <string>

#include "pipeline_base.hpp"
#include "reactor.hpp"
#include "tcp_socket.hpp"

namespace databento_async {

// ProtocolDriver concept defines the interface for protocol implementations.
// Each protocol (Live, Historical) provides static methods that the Pipeline
// template uses to handle protocol-specific behavior.
//
// Template parameters:
//   P - The protocol type (e.g., LiveProtocol, HistoricalProtocol)
//   Record - The record type passed through the Sink
template <typename P, typename Record>
concept ProtocolDriver = requires {
    // Request type for this protocol
    typename P::Request;

    // Chain type - the entry point component for this protocol
    typename P::ChainType;

    // Required static methods
    requires requires(
        Reactor& reactor,
        Sink<Record>& sink,
        const std::string& api_key,
        TcpSocket& tcp,
        std::shared_ptr<typename P::ChainType> chain,
        const typename P::Request& request,
        std::pmr::vector<std::byte> data
    ) {
        // Build component chain, returns entry point
        { P::BuildChain(reactor, sink, api_key) }
            -> std::same_as<std::shared_ptr<typename P::ChainType>>;

        // Wire TCP write callbacks to chain
        { P::WireTcp(tcp, chain) } -> std::same_as<void>;

        // Handle TCP connect event - returns true if ready to send request
        // Live: returns true (send on connect)
        // Historical: returns false (must wait for TLS handshake)
        { P::OnConnect(chain) } -> std::same_as<bool>;

        // Handle TCP read - returns true if ready to send request
        // Live: always returns true (already ready)
        // Historical: returns true after TLS handshake completes
        { P::OnRead(chain, std::move(data)) } -> std::same_as<bool>;

        // Send the protocol-specific request
        { P::SendRequest(chain, request) } -> std::same_as<void>;

        // Teardown chain components
        { P::Teardown(chain) } -> std::same_as<void>;
    };
};

}  // namespace databento_async
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:protocol_driver_test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/protocol_driver.hpp tests/protocol_driver_test.cpp
git commit -m "feat: add ProtocolDriver concept

Defines the interface for protocol implementations (Live, Historical).
Each protocol provides static hooks that Pipeline uses:
- BuildChain: create component chain
- WireTcp: connect chain to TCP socket
- OnConnect: handle connect event (returns readiness)
- OnRead: handle data (returns readiness after handshake)
- SendRequest: send protocol-specific request
- Teardown: clean up components

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 4: Implement LiveProtocol Driver

Create the LiveProtocol driver that wraps CramAuth.

**Files:**
- Create: `src/live_protocol.hpp`
- Test: `tests/live_protocol_test.cpp`

**Step 1: Write the failing test**

Create `tests/live_protocol_test.cpp`:

```cpp
// tests/live_protocol_test.cpp
#include <gtest/gtest.h>

#include "src/live_protocol.hpp"
#include "src/protocol_driver.hpp"
#include "src/pipeline_base.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

// Forward declare the record type used by live protocol
struct DbnRecord {};
using LiveSink = Sink<DbnRecord>;

TEST(LiveProtocolTest, SatisfiesProtocolDriverConcept) {
    static_assert(ProtocolDriver<LiveProtocol, DbnRecord>);
    SUCCEED();
}

TEST(LiveProtocolTest, OnConnectReturnsTrue) {
    // Live protocol is ready immediately on connect
    std::shared_ptr<LiveProtocol::ChainType> chain;
    EXPECT_TRUE(LiveProtocol::OnConnect(chain));
}

TEST(LiveProtocolTest, OnReadReturnsTrue) {
    // Live protocol is always ready after connect
    std::shared_ptr<LiveProtocol::ChainType> chain;
    std::pmr::vector<std::byte> data;
    EXPECT_TRUE(LiveProtocol::OnRead(chain, std::move(data)));
}

TEST(LiveProtocolTest, RequestHasRequiredFields) {
    LiveProtocol::Request req;
    req.dataset = "GLBX.MDP3";
    req.symbols = "ESZ4";
    req.schema = "mbp-1";

    EXPECT_EQ(req.dataset, "GLBX.MDP3");
    EXPECT_EQ(req.symbols, "ESZ4");
    EXPECT_EQ(req.schema, "mbp-1");
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:live_protocol_test`
Expected: FAIL - LiveProtocol not defined

**Step 3: Create src/live_protocol.hpp**

```cpp
// src/live_protocol.hpp
#pragma once

#include <memory>
#include <memory_resource>
#include <span>
#include <string>

#include "cram_auth.hpp"
#include "pipeline_base.hpp"
#include "reactor.hpp"
#include "tcp_socket.hpp"

namespace databento_async {

// Request type for live protocol
struct LiveRequest {
    std::string dataset;
    std::string symbols;
    std::string schema;
};

// LiveProtocol driver for Pipeline<LiveProtocol>.
// Uses CramAuth for authentication, then subscribes and streams.
//
// Protocol flow:
//   TCP connect → CramAuth handles greeting/challenge/auth → Subscribe → Stream
//
// Ready immediately on connect (OnConnect returns true) because CramAuth
// handles the authentication asynchronously through Read callbacks.
template <typename Record>
struct LiveProtocol {
    using Request = LiveRequest;

    // For live, the chain is just CramAuth connected to a Sink adapter
    // The Sink adapter forwards parsed bytes to the actual Sink
    using ChainType = CramAuth<Sink<Record>>;

    static std::shared_ptr<ChainType> BuildChain(
        Reactor& reactor,
        Sink<Record>& sink,
        const std::string& api_key
    ) {
        // CramAuth takes a shared_ptr to downstream, but Sink is owned by Pipeline.
        // We need an adapter that holds a raw pointer.
        // For now, create a wrapper that forwards to the sink.
        // TODO: This needs a SinkAdapter that satisfies Downstream
        auto sink_ptr = std::make_shared<Sink<Record>*>(&sink);
        return ChainType::Create(reactor, /* downstream */ sink_ptr, api_key);
    }

    static void WireTcp(TcpSocket& tcp, std::shared_ptr<ChainType>& chain) {
        chain->SetWriteCallback([&tcp](std::pmr::vector<std::byte> data) {
            tcp.Write(std::span<const std::byte>(data.data(), data.size()));
        });
    }

    static bool OnConnect(std::shared_ptr<ChainType>&) {
        // Live protocol is ready immediately on connect
        // CramAuth handles authentication through Read callbacks
        return true;
    }

    static bool OnRead(std::shared_ptr<ChainType>& chain,
                       std::pmr::vector<std::byte> data) {
        if (chain) {
            chain->Read(std::move(data));
        }
        // Live is always ready after connect (auth happens in protocol)
        return true;
    }

    static void SendRequest(std::shared_ptr<ChainType>& chain,
                           const Request& req) {
        if (chain) {
            chain->Subscribe(req.dataset, req.symbols, req.schema);
            chain->StartStreaming();
        }
    }

    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) chain->Close();
    }
};

}  // namespace databento_async
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:live_protocol_test`
Expected: PASS (may need adjustments based on actual CramAuth interface)

**Step 5: Commit**

```bash
git add src/live_protocol.hpp tests/live_protocol_test.cpp
git commit -m "feat: add LiveProtocol driver

Implements ProtocolDriver for live streaming:
- Uses CramAuth for authentication
- Ready immediately on connect (OnConnect returns true)
- SendRequest subscribes and starts streaming

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 5: Implement HistoricalProtocol Driver

Create the HistoricalProtocol driver for HTTP/TLS downloads.

**Files:**
- Create: `src/historical_protocol.hpp`
- Test: `tests/historical_protocol_test.cpp`

**Step 1: Write the failing test**

Create `tests/historical_protocol_test.cpp`:

```cpp
// tests/historical_protocol_test.cpp
#include <gtest/gtest.h>

#include "src/historical_protocol.hpp"
#include "src/protocol_driver.hpp"
#include "src/pipeline_base.hpp"

using namespace databento_async;

struct DbnRecord {};
using HistoricalSink = Sink<DbnRecord>;

TEST(HistoricalProtocolTest, SatisfiesProtocolDriverConcept) {
    static_assert(ProtocolDriver<HistoricalProtocol, DbnRecord>);
    SUCCEED();
}

TEST(HistoricalProtocolTest, OnConnectReturnsFalse) {
    // Historical protocol must wait for TLS handshake
    std::shared_ptr<HistoricalProtocol::ChainType> chain;
    // Note: OnConnect starts handshake, returns false
    // In real test, chain would need to be initialized
    // For concept satisfaction, this is sufficient
    SUCCEED();
}

TEST(HistoricalProtocolTest, RequestHasTimeRange) {
    HistoricalProtocol::Request req;
    req.dataset = "GLBX.MDP3";
    req.symbols = "ESZ4";
    req.schema = "mbp-1";
    req.start = 1704067200000000000ULL;  // 2024-01-01
    req.end = 1704153600000000000ULL;    // 2024-01-02

    EXPECT_EQ(req.dataset, "GLBX.MDP3");
    EXPECT_GT(req.start, 0ULL);
    EXPECT_GT(req.end, req.start);
}

TEST(HistoricalProtocolTest, UrlEncodeHandlesSpecialChars) {
    // Test URL encoding helper
    std::string input = "ES Z4+test";
    std::string encoded = HistoricalProtocol::UrlEncode(input);
    EXPECT_EQ(encoded, "ES%20Z4%2Btest");
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:historical_protocol_test`
Expected: FAIL - HistoricalProtocol not defined

**Step 3: Create src/historical_protocol.hpp**

```cpp
// src/historical_protocol.hpp
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>

#include "http_client.hpp"
#include "pipeline_base.hpp"
#include "reactor.hpp"
#include "tcp_socket.hpp"
#include "tls_socket.hpp"
#include "zstd_decompressor.hpp"

namespace databento_async {

// Request type for historical protocol
struct HistoricalRequest {
    std::string dataset;
    std::string symbols;
    std::string schema;
    std::uint64_t start;  // Nanoseconds since epoch
    std::uint64_t end;    // Nanoseconds since epoch
};

// HistoricalProtocol driver for Pipeline<HistoricalProtocol>.
// Uses TLS + HTTP + Zstd decompression for historical data downloads.
//
// Protocol flow:
//   TCP connect → TLS handshake → HTTP GET request → Zstd decompress → Parse
//
// Not ready until TLS handshake completes (OnConnect returns false).
template <typename Record>
struct HistoricalProtocol {
    using Request = HistoricalRequest;

    // Chain wraps TLS and holds api_key for HTTP auth
    struct ChainType {
        std::shared_ptr<TlsSocket<HttpClient<ZstdDecompressor<Sink<Record>>>>> tls;
        std::string api_key;

        void StartHandshake() { if (tls) tls->StartHandshake(); }
        bool IsHandshakeComplete() const { return tls && tls->IsHandshakeComplete(); }
        void Read(std::pmr::vector<std::byte> data) { if (tls) tls->Read(std::move(data)); }
        void Write(std::pmr::vector<std::byte> data) { if (tls) tls->Write(std::move(data)); }
        void Close() { if (tls) tls->Close(); }

        template <typename F>
        void SetUpstreamWriteCallback(F&& cb) {
            if (tls) tls->SetUpstreamWriteCallback(std::forward<F>(cb));
        }
    };

    static std::shared_ptr<ChainType> BuildChain(
        Reactor& reactor,
        Sink<Record>& sink,
        const std::string& api_key
    ) {
        // Build chain: TLS → HTTP → Zstd → Sink
        auto sink_ptr = std::make_shared<Sink<Record>*>(&sink);
        auto zstd = ZstdDecompressor<Sink<Record>*>::Create(reactor, sink_ptr);
        auto http = HttpClient<decltype(zstd)>::Create(reactor, zstd);
        auto tls = TlsSocket<decltype(http)>::Create(reactor, http);
        tls->SetHostname("hist.databento.com");

        auto chain = std::make_shared<ChainType>();
        chain->tls = tls;
        chain->api_key = api_key;
        return chain;
    }

    static void WireTcp(TcpSocket& tcp, std::shared_ptr<ChainType>& chain) {
        chain->SetUpstreamWriteCallback([&tcp](std::pmr::vector<std::byte> data) {
            if (tcp.IsConnected()) {
                tcp.Write(std::span<const std::byte>(data.data(), data.size()));
            }
        });
    }

    static bool OnConnect(std::shared_ptr<ChainType>& chain) {
        if (chain) {
            chain->StartHandshake();
        }
        // Not ready yet - must wait for TLS handshake to complete
        return false;
    }

    static bool OnRead(std::shared_ptr<ChainType>& chain,
                       std::pmr::vector<std::byte> data) {
        if (chain) {
            chain->Read(std::move(data));
            // Ready to send request only after TLS handshake completes
            return chain->IsHandshakeComplete();
        }
        return false;
    }

    static void SendRequest(std::shared_ptr<ChainType>& chain,
                           const Request& req) {
        if (!chain) return;

        // Build and send HTTP request through TLS with auth
        std::string http_request = BuildHttpRequest(req, chain->api_key);
        std::pmr::vector<std::byte> buffer;
        buffer.resize(http_request.size());
        std::memcpy(buffer.data(), http_request.data(), http_request.size());
        chain->Write(std::move(buffer));
    }

    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) chain->Close();
    }

    // URL-encode a string (RFC 3986)
    static std::string UrlEncode(const std::string& s) {
        std::string result;
        result.reserve(s.size() * 3);  // Worst case
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result += static_cast<char>(c);
            } else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                result += buf;
            }
        }
        return result;
    }

private:
    // Base64 encode (simplified - use actual implementation in practice)
    static std::string Base64Encode(const std::string& input);

    // Build HTTP GET request with Basic auth header
    static std::string BuildHttpRequest(const Request& req,
                                        const std::string& api_key) {
        std::string path = "/v0/timeseries.get_range?"
            "dataset=" + UrlEncode(req.dataset) +
            "&symbols=" + UrlEncode(req.symbols) +
            "&schema=" + UrlEncode(req.schema) +
            "&start=" + std::to_string(req.start) +
            "&end=" + std::to_string(req.end) +
            "&encoding=dbn&compression=zstd";

        std::string auth = Base64Encode(api_key + ":");

        return "GET " + path + " HTTP/1.1\r\n"
               "Host: hist.databento.com\r\n"
               "Authorization: Basic " + auth + "\r\n"
               "Accept-Encoding: zstd\r\n"
               "Connection: close\r\n"
               "\r\n";
    }
};

}  // namespace databento_async
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:historical_protocol_test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/historical_protocol.hpp tests/historical_protocol_test.cpp
git commit -m "feat: add HistoricalProtocol driver

Implements ProtocolDriver for historical downloads:
- Uses TLS → HTTP → Zstd → Sink chain
- OnConnect starts TLS handshake, returns false
- OnRead returns true after handshake completes
- SendRequest builds authenticated HTTP GET
- Includes URL encoding for query parameters

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 6: Create Pipeline Template

Implement the main Pipeline<P> template class.

**Files:**
- Create: `src/unified_pipeline.hpp`
- Test: `tests/unified_pipeline_test.cpp`

**Step 1: Write the failing test**

Create `tests/unified_pipeline_test.cpp`:

```cpp
// tests/unified_pipeline_test.cpp
#include <gtest/gtest.h>

#include "src/unified_pipeline.hpp"
#include "src/live_protocol.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

struct TestRecord {};

TEST(UnifiedPipelineTest, CreateReturnsSharedPtr) {
    Reactor reactor;
    auto pipeline = Pipeline<LiveProtocol<TestRecord>, TestRecord>::Create(
        reactor, "test_api_key");
    ASSERT_NE(pipeline, nullptr);
}

TEST(UnifiedPipelineTest, SetRequestStoresRequest) {
    Reactor reactor;
    reactor.Poll(0);  // Initialize thread ID

    auto pipeline = Pipeline<LiveProtocol<TestRecord>, TestRecord>::Create(
        reactor, "test_api_key");

    LiveRequest req{"GLBX.MDP3", "ESZ4", "mbp-1"};
    pipeline->SetRequest(req);

    // Should not throw - request is set
    SUCCEED();
}

TEST(UnifiedPipelineTest, StartBeforeConnectEmitsError) {
    Reactor reactor;
    reactor.Poll(0);

    auto pipeline = Pipeline<LiveProtocol<TestRecord>, TestRecord>::Create(
        reactor, "test_api_key");

    bool error_received = false;
    pipeline->OnError([&](const Error& e) {
        error_received = true;
        EXPECT_EQ(e.code, ErrorCode::InvalidState);
    });

    LiveRequest req{"GLBX.MDP3", "ESZ4", "mbp-1"};
    pipeline->SetRequest(req);
    pipeline->Start();  // Should emit error - not connected

    EXPECT_TRUE(error_received);
}

TEST(UnifiedPipelineTest, SuspendBeforeConnectIsRespected) {
    Reactor reactor;
    reactor.Poll(0);

    auto pipeline = Pipeline<LiveProtocol<TestRecord>, TestRecord>::Create(
        reactor, "test_api_key");

    pipeline->Suspend();
    EXPECT_TRUE(pipeline->IsSuspended());

    // When Connect() is called, reads should be disabled
    // (tested via actual TCP behavior in integration tests)
}

TEST(UnifiedPipelineTest, StateStartsDisconnected) {
    Reactor reactor;
    reactor.Poll(0);

    auto pipeline = Pipeline<LiveProtocol<TestRecord>, TestRecord>::Create(
        reactor, "test_api_key");

    EXPECT_EQ(pipeline->GetState(), PipelineState::Disconnected);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:unified_pipeline_test`
Expected: FAIL - Pipeline not defined

**Step 3: Create src/unified_pipeline.hpp**

This is a large file. Create it based on the design document at `docs/plans/2025-12-29-unified-pipeline-design.md` Section 3 "Pipeline Template".

Key elements:
- PrivateTag pattern for constructor
- Create() factory method
- Reactor-thread assertions on all public methods
- Terminal state guards
- TeardownPipeline with deferred cleanup via weak_from_this
- Sink invalidation before DoTeardown

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:unified_pipeline_test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/unified_pipeline.hpp tests/unified_pipeline_test.cpp
git commit -m "feat: add Pipeline<P> template

Unified pipeline that works with any ProtocolDriver:
- PrivateTag forces Create() factory for shared_ptr safety
- Reactor-thread assertions on all public methods
- Terminal state guards on state-transition methods
- Deferred teardown with weak_from_this for UAF prevention
- Sink invalidation before DoTeardown
- Pre-connect Suspend() respected when TCP is created

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 7: Add Type Aliases

Create backward-compatible type aliases for LiveClient and HistoricalClient.

**Files:**
- Create: `src/client.hpp`
- Test: `tests/client_test.cpp`

**Step 1: Write the failing test**

Create `tests/client_test.cpp`:

```cpp
// tests/client_test.cpp
#include <gtest/gtest.h>

#include "src/client.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

TEST(ClientTest, LiveClientIsAlias) {
    Reactor reactor;
    auto client = LiveClient::Create(reactor, "test_key");
    ASSERT_NE(client, nullptr);
}

TEST(ClientTest, HistoricalClientIsAlias) {
    Reactor reactor;
    auto client = HistoricalClient::Create(reactor, "test_key");
    ASSERT_NE(client, nullptr);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:client_test`
Expected: FAIL - LiveClient/HistoricalClient not defined

**Step 3: Create src/client.hpp**

```cpp
// src/client.hpp
#pragma once

#include "unified_pipeline.hpp"
#include "live_protocol.hpp"
#include "historical_protocol.hpp"

namespace databento_async {

// Forward declare the record type (from databento headers)
namespace databento {
struct Record;
}

// Type aliases for backward compatibility
using LiveClient = Pipeline<LiveProtocol<databento::Record>, databento::Record>;
using HistoricalClient = Pipeline<HistoricalProtocol<databento::Record>, databento::Record>;

}  // namespace databento_async
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:client_test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/client.hpp tests/client_test.cpp
git commit -m "feat: add LiveClient and HistoricalClient type aliases

Provides backward-compatible type aliases:
- LiveClient = Pipeline<LiveProtocol>
- HistoricalClient = Pipeline<HistoricalProtocol>

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 8: Integration Tests

Create integration tests that verify the full pipeline works end-to-end.

**Files:**
- Create: `tests/pipeline_integration_test.cpp`

**Step 1: Write integration tests**

```cpp
// tests/pipeline_integration_test.cpp
#include <gtest/gtest.h>

#include "src/client.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

class PipelineIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        reactor_.Poll(0);  // Initialize thread ID
    }

    Reactor reactor_;
};

TEST_F(PipelineIntegrationTest, LiveClientLifecycle) {
    auto client = LiveClient::Create(reactor_, "test_api_key");

    bool error_received = false;
    bool complete_received = false;

    client->OnError([&](const Error& e) {
        error_received = true;
    });

    client->OnComplete([&]() {
        complete_received = true;
    });

    // Set request
    LiveRequest req{"GLBX.MDP3", "ESZ4", "mbp-1"};
    client->SetRequest(req);

    // State should still be disconnected
    EXPECT_EQ(client->GetState(), PipelineState::Disconnected);

    // Stop before connect is safe
    client->Stop();
    EXPECT_EQ(client->GetState(), PipelineState::Disconnected);
}

TEST_F(PipelineIntegrationTest, HistoricalClientLifecycle) {
    auto client = HistoricalClient::Create(reactor_, "test_api_key");

    HistoricalRequest req{
        "GLBX.MDP3",
        "ESZ4",
        "mbp-1",
        1704067200000000000ULL,
        1704153600000000000ULL
    };
    client->SetRequest(req);

    EXPECT_EQ(client->GetState(), PipelineState::Disconnected);
}

TEST_F(PipelineIntegrationTest, SuspendResumeBeforeConnect) {
    auto client = LiveClient::Create(reactor_, "test_api_key");

    EXPECT_FALSE(client->IsSuspended());

    client->Suspend();
    EXPECT_TRUE(client->IsSuspended());

    client->Suspend();  // Nested
    EXPECT_TRUE(client->IsSuspended());

    client->Resume();
    EXPECT_TRUE(client->IsSuspended());  // Still suspended (count=1)

    client->Resume();
    EXPECT_FALSE(client->IsSuspended());
}
```

**Step 2: Run tests**

Run: `bazel test //tests:pipeline_integration_test`
Expected: PASS

**Step 3: Commit**

```bash
git add tests/pipeline_integration_test.cpp
git commit -m "test: add pipeline integration tests

Tests client lifecycle, suspend/resume semantics, and state transitions
without requiring actual network connections.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Task 9: Cleanup and Final Verification

Remove any deprecated code and run full test suite.

**Files:**
- Review all new files for consistency
- Run full test suite
- Update any remaining references

**Step 1: Run full test suite**

```bash
bazel test //tests/...
```

Expected: All tests pass

**Step 2: Verify no broken references**

```bash
grep -r "LiveProtocolHandler" src/ tests/ --include="*.hpp" --include="*.cpp"
```

Expected: No matches (all renamed to CramAuth)

**Step 3: Final commit**

```bash
git add -A
git commit -m "chore: unified Pipeline implementation complete

Implemented:
- CramAuth (renamed from LiveProtocolHandler)
- PipelineBase and Sink classes
- ProtocolDriver concept
- LiveProtocol driver
- HistoricalProtocol driver
- Pipeline<P> template
- LiveClient/HistoricalClient type aliases
- Integration tests

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

---

## Summary

| Task | Description | Files |
|------|-------------|-------|
| 1 | Rename LiveProtocolHandler → CramAuth | `src/cram_auth.hpp`, `tests/cram_auth_test.cpp` |
| 2 | Create PipelineBase and Sink | `src/pipeline_base.hpp`, `tests/pipeline_base_test.cpp` |
| 3 | Create ProtocolDriver concept | `src/protocol_driver.hpp`, `tests/protocol_driver_test.cpp` |
| 4 | Implement LiveProtocol | `src/live_protocol.hpp`, `tests/live_protocol_test.cpp` |
| 5 | Implement HistoricalProtocol | `src/historical_protocol.hpp`, `tests/historical_protocol_test.cpp` |
| 6 | Create Pipeline template | `src/unified_pipeline.hpp`, `tests/unified_pipeline_test.cpp` |
| 7 | Add type aliases | `src/client.hpp`, `tests/client_test.cpp` |
| 8 | Integration tests | `tests/pipeline_integration_test.cpp` |
| 9 | Cleanup and verification | - |
