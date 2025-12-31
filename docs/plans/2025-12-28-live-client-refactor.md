# LiveClient Refactor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Refactor LiveClient to use pipeline architecture matching HistoricalClient, with DbnParser as a pipeline component.

**Architecture:** LiveClient becomes an orchestrator that builds the pipeline: TcpSocket → LiveProtocolHandler → DbnParserComponent → Sink. LiveClient itself is not a downstream - it receives parsed records via Sink callbacks. This mirrors HistoricalClient's structure and enables consistent lifetime management via TryGuard pattern.

**Tech Stack:** C++23 concepts, CRTP PipelineComponent base, databento-cpp record types

---

## Design Notes (from Codex Review)

### Issue 1: EmitError/EmitDone constrained to Downstream
**Problem:** `PipelineComponent::EmitError/EmitDone` are constrained to `Downstream` concept, but `RecordDownstream` is different. Won't compile.

**Solution:** Add `TerminalDownstream` concept that only requires `OnError/OnDone`, have both `Downstream` and `RecordDownstream` refine it. Add overloads in PipelineComponent for TerminalDownstream.

```cpp
template<typename D>
concept TerminalDownstream = requires(D& d, const Error& e) {
    { d.OnError(e) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

template<typename D>
concept Downstream = TerminalDownstream<D> && requires(D& d, std::pmr::vector<std::byte> data) {
    { d.Read(std::move(data)) } -> std::same_as<void>;
};

template<typename D>
concept RecordDownstream = TerminalDownstream<D> && requires(D& d, const databento::Record& rec) {
    { d.OnRecord(rec) } -> std::same_as<void>;
};
```

### Issue 2: Record size validation missing
**Problem:** `DbnParserComponent::DrainBuffer()` doesn't validate `hdr->Size()` - could be 0 or smaller than header, causing infinite loop.

**Solution:** Add validation in `HasCompleteRecord()`:
```cpp
std::size_t size = PeekRecordSize();
if (size == 0 || size < sizeof(databento::RecordHeader)) {
    // Emit parse error
    return false;
}
```

### Issue 3: LiveProtocolHandler ignores text after auth
**Problem:** After `StartStreaming()`, all data goes to binary passthrough. Server errors or subscription errors as text would be misrouted.

**Solution:** Wait for binary data marker or handle text lines in Ready/Streaming states:
- Option A: First byte check - if line starts with printable ASCII, treat as text
- Option B: Protocol-level: server sends a specific marker before binary data
- For now: Document limitation, can enhance later based on actual protocol behavior

### Issue 4: Unbounded buffer growth
**Problem:** Both components buffer without caps when suspended.

**Solution:** Add `kMaxPendingInput` limit (like ZstdDecompressor has 16MB cap). Emit error on overflow.

### Issue 5: Record lifetime
**Problem:** `databento::Record` points into internal buffer which can be compacted/reallocated.

**Solution:** Document that downstream MUST NOT store record references beyond OnRecord() call. The record is only valid for the duration of the callback.

### Issue 6: Line endings
**Problem:** Live protocol may use `\r\n` not just `\n`.

**Solution:** Trim both `\r` and `\n` in ProcessLine().

---

## Task 1: Add TerminalDownstream and RecordDownstream Concepts

**Files:**
- Modify: `src/pipeline.hpp`
- Test: `tests/pipeline_test.cpp`

**Step 1: Write failing test for new concepts**

Add to `tests/pipeline_test.cpp`:

```cpp
#include <databento/record.hpp>

// Mock that satisfies RecordDownstream
struct MockRecordDownstream {
    const databento::Record* last_record = nullptr;
    Error last_error;
    bool done = false;

    void OnRecord(const databento::Record& rec) { last_record = &rec; }
    void OnError(const Error& e) { last_error = e; }
    void OnDone() { done = true; }
};

TEST(PipelineTest, TerminalDownstreamConcept) {
    // Both Downstream and RecordDownstream should satisfy TerminalDownstream
    static_assert(TerminalDownstream<MockDownstream>);
    static_assert(TerminalDownstream<MockRecordDownstream>);
}

TEST(PipelineTest, RecordDownstreamConcept) {
    static_assert(RecordDownstream<MockRecordDownstream>);
    // Downstream should NOT satisfy RecordDownstream (no OnRecord)
    static_assert(!RecordDownstream<MockDownstream>);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:pipeline_test`
Expected: FAIL - `TerminalDownstream` and `RecordDownstream` not defined

**Step 3: Refactor concepts in pipeline.hpp**

Replace the existing `Downstream` concept section with:

```cpp
#include <databento/record.hpp>

// Base concept for terminal signals (error/done) - shared by all downstream types
template<typename D>
concept TerminalDownstream = requires(D& d, const Error& e) {
    { d.OnError(e) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

// Concept for components that receive byte data
template<typename D>
concept Downstream = TerminalDownstream<D> && requires(D& d, std::pmr::vector<std::byte> data) {
    { d.Read(std::move(data)) } -> std::same_as<void>;
};

// Concept for components that receive parsed records (final pipeline stage)
// NOTE: Records are only valid for duration of OnRecord() call - do not store references
template<typename D>
concept RecordDownstream = TerminalDownstream<D> && requires(D& d, const databento::Record& rec) {
    { d.OnRecord(rec) } -> std::same_as<void>;
};
```

**Step 4: Update PipelineComponent EmitError/EmitDone to use TerminalDownstream**

In `src/pipeline.hpp`, update EmitError and EmitDone:

```cpp
// Change from:
template<Downstream D>
void EmitError(D& downstream, const Error& e) { ... }

// To:
template<TerminalDownstream D>
void EmitError(D& downstream, const Error& e) { ... }

template<TerminalDownstream D>
void EmitDone(D& downstream) { ... }
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:pipeline_test`
Expected: PASS

**Step 5: Commit**

```bash
git add src/pipeline.hpp tests/pipeline_test.cpp
git commit -m "feat(pipeline): add RecordDownstream concept for parsed records"
```

---

## Task 2: Create DbnParserComponent

**Files:**
- Create: `src/dbn_parser_component.hpp`
- Create: `tests/dbn_parser_component_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write failing test for DbnParserComponent**

Create `tests/dbn_parser_component_test.cpp`:

```cpp
// tests/dbn_parser_component_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include <databento/record.hpp>

#include "src/dbn_parser_component.hpp"
#include "src/pipeline.hpp"
#include "src/reactor.hpp"

using namespace dbn_pipe;

struct MockSink {
    std::vector<databento::RecordHeader> headers;
    Error last_error;
    bool done = false;

    void OnRecord(const databento::Record& rec) {
        headers.push_back(rec.Header());
    }
    void OnError(const Error& e) { last_error = e; }
    void OnDone() { done = true; }
};

TEST(DbnParserComponentTest, FactoryCreation) {
    Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = DbnParserComponent<MockSink>::Create(reactor, downstream);
    ASSERT_NE(parser, nullptr);
}

TEST(DbnParserComponentTest, ParsesSingleRecord) {
    Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = DbnParserComponent<MockSink>::Create(reactor, downstream);

    // Create a minimal valid record header
    databento::RecordHeader hdr;
    hdr.length = sizeof(databento::RecordHeader) / 4;  // length in 32-bit words
    hdr.rtype = 0;
    hdr.publisher_id = 1;
    hdr.instrument_id = 100;
    hdr.ts_event = 1234567890;

    std::pmr::vector<std::byte> data;
    auto* bytes = reinterpret_cast<const std::byte*>(&hdr);
    data.assign(bytes, bytes + sizeof(hdr));

    parser->Read(std::move(data));

    ASSERT_EQ(downstream->headers.size(), 1);
    EXPECT_EQ(downstream->headers[0].instrument_id, 100);
}

TEST(DbnParserComponentTest, ForwardsOnDone) {
    Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = DbnParserComponent<MockSink>::Create(reactor, downstream);

    parser->OnDone();

    EXPECT_TRUE(downstream->done);
}

TEST(DbnParserComponentTest, ForwardsOnError) {
    Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = DbnParserComponent<MockSink>::Create(reactor, downstream);

    parser->OnError(Error{ErrorCode::ConnectionFailed, "test error"});

    EXPECT_EQ(downstream->last_error.code, ErrorCode::ConnectionFailed);
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:dbn_parser_component_test`
Expected: FAIL - cannot find `src/dbn_parser_component.hpp`

**Step 3: Write DbnParserComponent header**

Create `src/dbn_parser_component.hpp`:

```cpp
// src/dbn_parser_component.hpp
#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <vector>

#include <databento/record.hpp>

#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"
#include "tls_socket.hpp"  // For Suspendable

namespace dbn_pipe {

template <RecordDownstream D>
class DbnParserComponent
    : public PipelineComponent<DbnParserComponent<D>>,
      public Suspendable,
      public std::enable_shared_from_this<DbnParserComponent<D>> {

    using Base = PipelineComponent<DbnParserComponent<D>>;
    friend Base;

    // Enable shared_from_this in constructor
    struct MakeSharedEnabler;

public:
    static std::shared_ptr<DbnParserComponent> Create(
        Reactor& reactor,
        std::shared_ptr<D> downstream
    ) {
        return std::make_shared<MakeSharedEnabler>(reactor, std::move(downstream));
    }

    ~DbnParserComponent() = default;

    // Downstream interface (bytes in)
    void Read(std::pmr::vector<std::byte> data);
    void OnError(const Error& e);
    void OnDone();

    // Suspendable interface
    void Suspend() override;
    void Resume() override;
    void Close() override { this->RequestClose(); }

    void SetUpstream(Suspendable* up) { upstream_ = up; }

    // PipelineComponent requirements
    void DisableWatchers() {}
    void DoClose();

private:
    DbnParserComponent(Reactor& reactor, std::shared_ptr<D> downstream);

    void DrainBuffer();
    bool HasCompleteRecord();  // non-const: may emit error on invalid size
    std::size_t PeekRecordSize() const;

    std::shared_ptr<D> downstream_;
    Suspendable* upstream_ = nullptr;

    std::vector<std::byte> buffer_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;

    bool suspended_ = false;

    static constexpr std::size_t kInitialBufferSize = 64 * 1024;
    static constexpr std::size_t kMaxBufferSize = 16 * 1024 * 1024;  // 16MB cap (Issue 4 fix)
};

template <RecordDownstream D>
struct DbnParserComponent<D>::MakeSharedEnabler : public DbnParserComponent<D> {
    MakeSharedEnabler(Reactor& reactor, std::shared_ptr<D> downstream)
        : DbnParserComponent<D>(reactor, std::move(downstream)) {}
};

// Implementation

template <RecordDownstream D>
DbnParserComponent<D>::DbnParserComponent(Reactor& reactor, std::shared_ptr<D> downstream)
    : Base(reactor)
    , downstream_(std::move(downstream))
    , buffer_(kInitialBufferSize)
{}

template <RecordDownstream D>
void DbnParserComponent<D>::DoClose() {
    downstream_.reset();
    if (upstream_) upstream_->Close();
}

template <RecordDownstream D>
void DbnParserComponent<D>::Read(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    // Check buffer overflow (Issue 4 fix)
    std::size_t unread = write_pos_ - read_pos_;
    if (unread + data.size() > kMaxBufferSize) {
        this->EmitError(*downstream_,
            Error{ErrorCode::ParseError, "Parser buffer overflow"});
        this->RequestClose();
        return;
    }

    // Compact buffer if needed
    if (write_pos_ + data.size() > buffer_.size()) {
        if (read_pos_ > 0) {
            if (unread > 0) {
                std::memmove(buffer_.data(), buffer_.data() + read_pos_, unread);
            }
            read_pos_ = 0;
            write_pos_ = unread;
        }
        if (write_pos_ + data.size() > buffer_.size()) {
            buffer_.resize(write_pos_ + data.size());
        }
    }

    // Append data
    std::memcpy(buffer_.data() + write_pos_, data.data(), data.size());
    write_pos_ += data.size();

    if (!suspended_) {
        DrainBuffer();
    }
}

template <RecordDownstream D>
void DbnParserComponent<D>::DrainBuffer() {
    while (!suspended_ && HasCompleteRecord()) {
        auto* hdr = reinterpret_cast<databento::RecordHeader*>(
            buffer_.data() + read_pos_);
        std::size_t record_size = hdr->Size();

        databento::Record rec{hdr};
        read_pos_ += record_size;

        downstream_->OnRecord(rec);
    }
}

template <RecordDownstream D>
bool DbnParserComponent<D>::HasCompleteRecord() {
    std::size_t available = write_pos_ - read_pos_;
    if (available < sizeof(databento::RecordHeader)) {
        return false;
    }

    std::size_t size = PeekRecordSize();

    // Validate record size (Issue 2 fix)
    if (size == 0 || size < sizeof(databento::RecordHeader)) {
        this->EmitError(*downstream_,
            Error{ErrorCode::ParseError, "Invalid record size in DBN stream"});
        this->RequestClose();
        return false;
    }

    return available >= size;
}

template <RecordDownstream D>
std::size_t DbnParserComponent<D>::PeekRecordSize() const {
    if (write_pos_ - read_pos_ < sizeof(databento::RecordHeader)) {
        return 0;
    }
    auto* hdr = reinterpret_cast<const databento::RecordHeader*>(
        buffer_.data() + read_pos_);
    return hdr->Size();
}

template <RecordDownstream D>
void DbnParserComponent<D>::OnError(const Error& e) {
    auto guard = this->TryGuard();
    if (!guard) return;

    this->EmitError(*downstream_, e);
    this->RequestClose();
}

template <RecordDownstream D>
void DbnParserComponent<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    // Check for incomplete record
    if (write_pos_ > read_pos_) {
        this->EmitError(*downstream_,
            Error{ErrorCode::ParseError, "Incomplete record at end of stream"});
    } else {
        this->EmitDone(*downstream_);
    }
    this->RequestClose();
}

template <RecordDownstream D>
void DbnParserComponent<D>::Suspend() {
    auto guard = this->TryGuard();
    if (!guard) return;

    suspended_ = true;
    if (upstream_) upstream_->Suspend();
}

template <RecordDownstream D>
void DbnParserComponent<D>::Resume() {
    auto guard = this->TryGuard();
    if (!guard) return;

    suspended_ = false;
    DrainBuffer();

    if (!suspended_ && upstream_) {
        upstream_->Resume();
    }
}

}  // namespace dbn_pipe
```

**Step 4: Add BUILD rules**

Add to `src/BUILD.bazel`:

```python
cc_library(
    name = "dbn_parser_component",
    hdrs = ["dbn_parser_component.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":error",
        ":pipeline",
        ":reactor",
        ":tls_socket",
        "@databento-cpp",
    ],
)
```

Add to `tests/BUILD.bazel`:

```python
cc_test(
    name = "dbn_parser_component_test",
    srcs = ["dbn_parser_component_test.cpp"],
    deps = [
        "//src:dbn_parser_component",
        "//src:pipeline",
        "//src:reactor",
        "@databento-cpp",
        "@googletest//:gtest_main",
    ],
)
```

**Step 5: Add ParseError and BufferOverflow to error.hpp**

Add to `src/error.hpp` in the ErrorCode enum:

```cpp
// Parse errors
ParseError,
BufferOverflow,
```

**Step 6: Run tests to verify they pass**

Run: `bazel test //tests:dbn_parser_component_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/dbn_parser_component.hpp src/error.hpp src/BUILD.bazel tests/dbn_parser_component_test.cpp tests/BUILD.bazel
git commit -m "feat(parser): add DbnParserComponent as pipeline downstream"
```

---

## Task 3: Create LiveProtocolHandler

**Files:**
- Create: `src/live_protocol_handler.hpp`
- Create: `tests/live_protocol_handler_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write failing test for LiveProtocolHandler**

Create `tests/live_protocol_handler_test.cpp`:

```cpp
// tests/live_protocol_handler_test.cpp
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "src/live_protocol_handler.hpp"
#include "src/pipeline.hpp"
#include "src/reactor.hpp"

using namespace dbn_pipe;

struct MockLiveDownstream {
    std::vector<std::byte> received;
    Error last_error;
    bool done = false;

    void Read(std::pmr::vector<std::byte> data) {
        received.insert(received.end(), data.begin(), data.end());
    }
    void OnError(const Error& e) { last_error = e; }
    void OnDone() { done = true; }
};

// Helper to create byte vector from string
std::pmr::vector<std::byte> ToBytes(std::string_view s) {
    std::pmr::vector<std::byte> data;
    for (char c : s) {
        data.push_back(static_cast<std::byte>(c));
    }
    return data;
}

TEST(LiveProtocolHandlerTest, FactoryCreation) {
    Reactor reactor;
    auto downstream = std::make_shared<MockLiveDownstream>();
    auto handler = LiveProtocolHandler<MockLiveDownstream>::Create(
        reactor, downstream, "test-api-key");
    ASSERT_NE(handler, nullptr);
}

TEST(LiveProtocolHandlerTest, InitialStateIsWaitingGreeting) {
    Reactor reactor;
    auto downstream = std::make_shared<MockLiveDownstream>();
    auto handler = LiveProtocolHandler<MockLiveDownstream>::Create(
        reactor, downstream, "test-api-key");
    EXPECT_EQ(handler->GetState(), LiveProtocolHandler<MockLiveDownstream>::State::WaitingGreeting);
}

TEST(LiveProtocolHandlerTest, ReceivesGreeting) {
    Reactor reactor;
    auto downstream = std::make_shared<MockLiveDownstream>();
    auto handler = LiveProtocolHandler<MockLiveDownstream>::Create(
        reactor, downstream, "test-api-key");

    handler->Read(ToBytes("session123|version1\n"));

    EXPECT_EQ(handler->GetState(), LiveProtocolHandler<MockLiveDownstream>::State::WaitingChallenge);
    EXPECT_EQ(handler->GetSessionId(), "session123");
}

TEST(LiveProtocolHandlerTest, StreamingPassesBinaryData) {
    Reactor reactor;
    auto downstream = std::make_shared<MockLiveDownstream>();
    auto handler = LiveProtocolHandler<MockLiveDownstream>::Create(
        reactor, downstream, "test-api-key");

    // Simulate completing auth and entering streaming state
    handler->SetStateForTesting(LiveProtocolHandler<MockLiveDownstream>::State::Streaming);

    std::pmr::vector<std::byte> binary_data;
    binary_data.push_back(std::byte{0x01});
    binary_data.push_back(std::byte{0x02});
    binary_data.push_back(std::byte{0x03});

    handler->Read(std::move(binary_data));

    ASSERT_EQ(downstream->received.size(), 3);
    EXPECT_EQ(downstream->received[0], std::byte{0x01});
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:live_protocol_handler_test`
Expected: FAIL - cannot find `src/live_protocol_handler.hpp`

**Step 3: Write LiveProtocolHandler header**

Create `src/live_protocol_handler.hpp`:

```cpp
// src/live_protocol_handler.hpp
#pragma once

#include <memory>
#include <memory_resource>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cram_auth.hpp"
#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"
#include "tls_socket.hpp"  // For Suspendable

namespace dbn_pipe {

// Handles the Databento Live API protocol:
// - Text-based authentication phase (greeting, challenge, auth response)
// - Binary streaming phase after start_session
//
// LIMITATION: After entering Streaming state, all data is treated as binary.
// If the server sends text error messages after start_session, they will be
// passed to downstream as binary data. This may need enhancement based on
// actual protocol behavior.
template <Downstream D>
class LiveProtocolHandler
    : public PipelineComponent<LiveProtocolHandler<D>>,
      public Suspendable,
      public std::enable_shared_from_this<LiveProtocolHandler<D>> {

    using Base = PipelineComponent<LiveProtocolHandler<D>>;
    friend Base;

    struct MakeSharedEnabler;

public:
    enum class State {
        WaitingGreeting,
        WaitingChallenge,
        Authenticating,
        Ready,
        Streaming
    };

    using WriteCallback = std::function<void(std::pmr::vector<std::byte>)>;

    static std::shared_ptr<LiveProtocolHandler> Create(
        Reactor& reactor,
        std::shared_ptr<D> downstream,
        std::string api_key
    ) {
        return std::make_shared<MakeSharedEnabler>(
            reactor, std::move(downstream), std::move(api_key));
    }

    ~LiveProtocolHandler() = default;

    // From TcpSocket
    void Read(std::pmr::vector<std::byte> data);
    void OnError(const Error& e);
    void OnDone();

    // Suspendable interface
    void Suspend() override;
    void Resume() override;
    void Close() override { this->RequestClose(); }

    void SetUpstream(Suspendable* up) { upstream_ = up; }
    void SetWriteCallback(WriteCallback cb) { write_callback_ = std::move(cb); }

    // Subscription control
    void Subscribe(std::string_view dataset, std::string_view symbols, std::string_view schema);
    void StartStreaming();

    // State access
    State GetState() const { return state_; }
    const std::string& GetSessionId() const { return session_id_; }

    // PipelineComponent requirements
    void DisableWatchers() {}
    void DoClose();

    // For testing only
    void SetStateForTesting(State s) { state_ = s; }

private:
    LiveProtocolHandler(Reactor& reactor, std::shared_ptr<D> downstream, std::string api_key);

    void ProcessLine(std::string_view line);
    void HandleGreeting(std::string_view line);
    void HandleChallenge(std::string_view line);
    void HandleAuthResponse(std::string_view line);

    void SendAuth(std::string_view challenge);
    void SendSubscription();
    void SendStart();
    void WriteString(std::string_view msg);

    std::shared_ptr<D> downstream_;
    Suspendable* upstream_ = nullptr;
    WriteCallback write_callback_;

    std::string api_key_;
    State state_ = State::WaitingGreeting;
    std::string session_id_;
    std::string line_buffer_;

    // Subscription params
    std::string dataset_;
    std::string symbols_;
    std::string schema_;
    std::uint32_t sub_counter_ = 0;

    bool suspended_ = false;
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::vector<std::byte> pending_data_{&pool_};

    static constexpr std::size_t kMaxPendingData = 16 * 1024 * 1024;  // 16MB cap (Issue 4 fix)
    static constexpr std::size_t kMaxLineLength = 8192;  // 8KB max line in text protocol
};

template <Downstream D>
struct LiveProtocolHandler<D>::MakeSharedEnabler : public LiveProtocolHandler<D> {
    MakeSharedEnabler(Reactor& reactor, std::shared_ptr<D> downstream, std::string api_key)
        : LiveProtocolHandler<D>(reactor, std::move(downstream), std::move(api_key)) {}
};

// Implementation

template <Downstream D>
LiveProtocolHandler<D>::LiveProtocolHandler(
    Reactor& reactor, std::shared_ptr<D> downstream, std::string api_key)
    : Base(reactor)
    , downstream_(std::move(downstream))
    , api_key_(std::move(api_key))
{}

template <Downstream D>
void LiveProtocolHandler<D>::DoClose() {
    downstream_.reset();
    if (upstream_) upstream_->Close();
}

template <Downstream D>
void LiveProtocolHandler<D>::Read(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    if (state_ == State::Streaming) {
        // Binary mode - pass directly to downstream
        if (suspended_) {
            // Check buffer overflow (Issue 4 fix)
            if (pending_data_.size() + data.size() > kMaxPendingData) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::BufferOverflow, "Protocol handler buffer overflow"});
                this->RequestClose();
                return;
            }
            pending_data_.insert(pending_data_.end(), data.begin(), data.end());
        } else {
            downstream_->Read(std::move(data));
        }
        return;
    }

    // Text protocol mode - parse lines
    for (auto byte : data) {
        char c = static_cast<char>(byte);
        line_buffer_ += c;

        // Check line buffer overflow
        if (line_buffer_.size() > kMaxLineLength) {
            this->EmitError(*downstream_,
                Error{ErrorCode::ParseError, "Protocol line too long"});
            this->RequestClose();
            return;
        }

        if (c == '\n') {
            ProcessLine(line_buffer_);
            line_buffer_.clear();
        }
    }
}

template <Downstream D>
void LiveProtocolHandler<D>::ProcessLine(std::string_view line) {
    // Remove trailing \r\n or \n (Issue 6 fix)
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line = line.substr(0, line.size() - 1);
    }

    switch (state_) {
        case State::WaitingGreeting:
            HandleGreeting(line);
            break;
        case State::WaitingChallenge:
            HandleChallenge(line);
            break;
        case State::Authenticating:
            HandleAuthResponse(line);
            break;
        default:
            break;
    }
}

template <Downstream D>
void LiveProtocolHandler<D>::HandleGreeting(std::string_view line) {
    auto pipe_pos = line.find('|');
    if (pipe_pos != std::string_view::npos) {
        session_id_ = line.substr(0, pipe_pos);
    } else {
        session_id_ = line;
    }
    state_ = State::WaitingChallenge;
}

template <Downstream D>
void LiveProtocolHandler<D>::HandleChallenge(std::string_view line) {
    auto challenge = CramAuth::ParseChallenge(std::string(line) + "\n");
    if (!challenge) {
        this->EmitError(*downstream_,
            Error{ErrorCode::InvalidChallenge, "Invalid challenge: " + std::string(line)});
        this->RequestClose();
        return;
    }

    SendAuth(*challenge);
    state_ = State::Authenticating;
}

template <Downstream D>
void LiveProtocolHandler<D>::HandleAuthResponse(std::string_view line) {
    bool found_success = false;
    bool is_success = false;
    std::string error_msg;

    std::string_view remaining = line;
    while (!remaining.empty()) {
        auto pipe_pos = remaining.find('|');
        std::string_view kv;
        if (pipe_pos != std::string_view::npos) {
            kv = remaining.substr(0, pipe_pos);
            remaining = remaining.substr(pipe_pos + 1);
        } else {
            kv = remaining;
            remaining = {};
        }

        if (kv.empty()) continue;

        auto eq_pos = kv.find('=');
        if (eq_pos == std::string_view::npos) continue;

        auto key = kv.substr(0, eq_pos);
        auto value = kv.substr(eq_pos + 1);

        if (key == "success") {
            found_success = true;
            is_success = (value == "1");
        } else if (key == "error") {
            error_msg = value;
        } else if (key == "session_id") {
            session_id_ = value;
        }
    }

    if (!found_success || !is_success) {
        std::string msg = "Authentication failed";
        if (!error_msg.empty()) {
            msg += ": " + error_msg;
        }
        this->EmitError(*downstream_, Error{ErrorCode::AuthFailed, msg});
        this->RequestClose();
        return;
    }

    state_ = State::Ready;

    // If subscription was set before auth completed, send now
    if (!dataset_.empty()) {
        SendSubscription();
    }
}

template <Downstream D>
void LiveProtocolHandler<D>::SendAuth(std::string_view challenge) {
    std::string response = CramAuth::ComputeResponse(challenge, api_key_);

    constexpr std::size_t kBucketIdLength = 5;
    std::string bucket_id;
    if (api_key_.size() >= kBucketIdLength) {
        bucket_id = api_key_.substr(api_key_.size() - kBucketIdLength);
    }

    std::ostringstream auth_msg;
    auth_msg << "auth=" << response;
    if (!bucket_id.empty()) {
        auth_msg << "-" << bucket_id;
    }
    auth_msg << "|dataset=" << dataset_
             << "|encoding=dbn"
             << "|ts_out=0"
             << "\n";

    WriteString(auth_msg.str());
}

template <Downstream D>
void LiveProtocolHandler<D>::Subscribe(
    std::string_view dataset, std::string_view symbols, std::string_view schema) {
    dataset_ = dataset;
    symbols_ = symbols;
    schema_ = schema;

    if (state_ == State::Ready) {
        SendSubscription();
    }
}

template <Downstream D>
void LiveProtocolHandler<D>::SendSubscription() {
    ++sub_counter_;

    std::ostringstream sub_msg;
    sub_msg << "schema=" << schema_
            << "|stype_in=raw_symbol"
            << "|id=" << sub_counter_
            << "|symbols=" << symbols_
            << "|snapshot=0"
            << "|is_last=1"
            << "\n";

    WriteString(sub_msg.str());
}

template <Downstream D>
void LiveProtocolHandler<D>::StartStreaming() {
    if (state_ != State::Ready) return;

    WriteString("start_session\n");
    state_ = State::Streaming;
}

template <Downstream D>
void LiveProtocolHandler<D>::SendStart() {
    StartStreaming();
}

template <Downstream D>
void LiveProtocolHandler<D>::WriteString(std::string_view msg) {
    if (!write_callback_) return;

    std::pmr::vector<std::byte> data{&pool_};
    data.reserve(msg.size());
    for (char c : msg) {
        data.push_back(static_cast<std::byte>(c));
    }
    write_callback_(std::move(data));
}

template <Downstream D>
void LiveProtocolHandler<D>::OnError(const Error& e) {
    auto guard = this->TryGuard();
    if (!guard) return;

    this->EmitError(*downstream_, e);
    this->RequestClose();
}

template <Downstream D>
void LiveProtocolHandler<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    this->EmitDone(*downstream_);
    this->RequestClose();
}

template <Downstream D>
void LiveProtocolHandler<D>::Suspend() {
    auto guard = this->TryGuard();
    if (!guard) return;

    suspended_ = true;
    if (upstream_) upstream_->Suspend();
}

template <Downstream D>
void LiveProtocolHandler<D>::Resume() {
    auto guard = this->TryGuard();
    if (!guard) return;

    suspended_ = false;

    if (!pending_data_.empty()) {
        auto data = std::move(pending_data_);
        pending_data_ = std::pmr::vector<std::byte>{&pool_};
        downstream_->Read(std::move(data));
    }

    if (!suspended_ && upstream_) {
        upstream_->Resume();
    }
}

}  // namespace dbn_pipe
```

**Step 4: Add BUILD rules**

Add to `src/BUILD.bazel`:

```python
cc_library(
    name = "live_protocol_handler",
    hdrs = ["live_protocol_handler.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":cram_auth",
        ":error",
        ":pipeline",
        ":reactor",
        ":tls_socket",
    ],
)
```

Add to `tests/BUILD.bazel`:

```python
cc_test(
    name = "live_protocol_handler_test",
    srcs = ["live_protocol_handler_test.cpp"],
    deps = [
        "//src:live_protocol_handler",
        "//src:pipeline",
        "//src:reactor",
        "@googletest//:gtest_main",
    ],
)
```

**Step 5: Run tests to verify they pass**

Run: `bazel test //tests:live_protocol_handler_test`
Expected: PASS

**Step 6: Commit**

```bash
git add src/live_protocol_handler.hpp src/BUILD.bazel tests/live_protocol_handler_test.cpp tests/BUILD.bazel
git commit -m "feat(live): add LiveProtocolHandler pipeline component"
```

---

## Task 4: Refactor LiveClient as Orchestrator

**Files:**
- Modify: `src/live_client.hpp`
- Modify: `src/live_client.cpp`
- Modify: `tests/live_client_test.cpp`
- Modify: `src/BUILD.bazel`

**Step 1: Update LiveClient header**

Replace `src/live_client.hpp` with:

```cpp
// src/live_client.hpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <databento/record.hpp>

#include "dbn_parser_component.hpp"
#include "error.hpp"
#include "live_protocol_handler.hpp"
#include "reactor.hpp"
#include "tcp_socket.hpp"

namespace dbn_pipe {

class LiveClient;

// Sink bridges parsed records back to LiveClient
class Sink {
public:
    explicit Sink(LiveClient* client) : client_(client) {}

    void Invalidate() { valid_ = false; }

    void OnRecord(const databento::Record& rec);
    void OnError(const Error& e);
    void OnDone();

private:
    LiveClient* client_;
    bool valid_ = true;
};

class LiveClient {
public:
    enum class State {
        Disconnected,
        Connecting,
        Authenticating,
        Ready,
        Streaming,
        Complete,
        Error
    };

    LiveClient(Reactor& reactor, std::string api_key);
    ~LiveClient();

    // Non-copyable, non-movable
    LiveClient(const LiveClient&) = delete;
    LiveClient& operator=(const LiveClient&) = delete;
    LiveClient(LiveClient&&) = delete;
    LiveClient& operator=(LiveClient&&) = delete;

    // Connection (caller responsible for DNS resolution)
    void Connect(const sockaddr_storage& addr);

    // Subscription (call after Connect, before Start)
    void Subscribe(std::string_view dataset, std::string_view symbols, std::string_view schema);

    // Begin streaming
    void Start();
    void Stop();

    // Backpressure
    void Pause();
    void Resume();

    // Callbacks
    template <typename Handler>
    void OnRecord(Handler&& h) { record_handler_ = std::forward<Handler>(h); }

    template <typename Handler>
    void OnError(Handler&& h) { error_handler_ = std::forward<Handler>(h); }

    template <typename Handler>
    void OnComplete(Handler&& h) { complete_handler_ = std::forward<Handler>(h); }

    // State
    State GetState() const { return state_; }
    const std::string& GetSessionId() const;

private:
    friend class Sink;

    void BuildPipeline();
    void HandleTcpConnect();
    void HandleTcpRead(std::span<const std::byte> data);
    void HandleTcpError(std::error_code ec);

    void HandleRecord(const databento::Record& rec);
    void HandlePipelineError(const Error& e);
    void HandlePipelineComplete();

    Reactor& reactor_;
    std::string api_key_;
    State state_ = State::Disconnected;
    bool stopped_ = false;

    // Pipeline components
    using ParserType = DbnParserComponent<Sink>;
    using ProtocolType = LiveProtocolHandler<ParserType>;

    std::shared_ptr<Sink> sink_;
    std::shared_ptr<ParserType> parser_;
    std::shared_ptr<ProtocolType> protocol_;
    std::unique_ptr<TcpSocket> tcp_;

    std::function<void(const databento::Record&)> record_handler_;
    std::function<void(const Error&)> error_handler_;
    std::function<void()> complete_handler_;
};

}  // namespace dbn_pipe
```

**Step 2: Update LiveClient implementation**

Replace `src/live_client.cpp` with:

```cpp
// src/live_client.cpp
#include "live_client.hpp"

namespace dbn_pipe {

// Sink implementation

void Sink::OnRecord(const databento::Record& rec) {
    if (!valid_ || !client_) return;
    client_->HandleRecord(rec);
}

void Sink::OnError(const Error& e) {
    if (!valid_ || !client_) return;
    client_->HandlePipelineError(e);
}

void Sink::OnDone() {
    if (!valid_ || !client_) return;
    client_->HandlePipelineComplete();
}

// LiveClient implementation

LiveClient::LiveClient(Reactor& reactor, std::string api_key)
    : reactor_(reactor)
    , api_key_(std::move(api_key))
{}

LiveClient::~LiveClient() {
    Stop();
}

void LiveClient::Connect(const sockaddr_storage& addr) {
    if (state_ != State::Disconnected) {
        if (error_handler_) {
            error_handler_(Error{ErrorCode::InvalidState,
                "Connect() can only be called when disconnected"});
        }
        return;
    }

    BuildPipeline();
    state_ = State::Connecting;
    tcp_->Connect(addr);
}

void LiveClient::BuildPipeline() {
    // Build from bottom up
    sink_ = std::make_shared<Sink>(this);
    parser_ = ParserType::Create(reactor_, sink_);
    protocol_ = ProtocolType::Create(reactor_, parser_, api_key_);

    // Set upstream pointers for backpressure
    parser_->SetUpstream(protocol_.get());

    // Create TCP socket
    tcp_ = std::make_unique<TcpSocket>(reactor_);

    // Wire TCP callbacks
    tcp_->OnConnect([this]() { HandleTcpConnect(); });
    tcp_->OnRead([this](std::span<const std::byte> data) { HandleTcpRead(data); });
    tcp_->OnError([this](std::error_code ec) { HandleTcpError(ec); });

    // Wire protocol write callback to TCP
    protocol_->SetWriteCallback([this](std::pmr::vector<std::byte> data) {
        if (tcp_ && tcp_->IsConnected()) {
            tcp_->Write(std::span<const std::byte>(data.data(), data.size()));
        }
    });
}

void LiveClient::HandleTcpConnect() {
    if (stopped_) return;
    state_ = State::Authenticating;
    // Protocol handler will receive greeting and start auth
}

void LiveClient::HandleTcpRead(std::span<const std::byte> data) {
    if (stopped_) return;

    std::pmr::vector<std::byte> buffer;
    buffer.assign(data.begin(), data.end());
    protocol_->Read(std::move(buffer));

    // Update state based on protocol state
    auto proto_state = protocol_->GetState();
    if (proto_state == ProtocolType::State::Ready && state_ == State::Authenticating) {
        state_ = State::Ready;
    } else if (proto_state == ProtocolType::State::Streaming && state_ != State::Streaming) {
        state_ = State::Streaming;
    }
}

void LiveClient::HandleTcpError(std::error_code ec) {
    if (stopped_) return;
    state_ = State::Error;
    if (error_handler_) {
        error_handler_(Error{ErrorCode::ConnectionFailed, ec.message()});
    }
}

void LiveClient::Subscribe(std::string_view dataset, std::string_view symbols, std::string_view schema) {
    if (protocol_) {
        protocol_->Subscribe(dataset, symbols, schema);
    }
}

void LiveClient::Start() {
    if (state_ != State::Ready) return;

    protocol_->StartStreaming();
    state_ = State::Streaming;
}

void LiveClient::Stop() {
    stopped_ = true;

    if (sink_) {
        sink_->Invalidate();
    }

    if (tcp_) {
        tcp_->Close();
    }

    tcp_.reset();
    protocol_.reset();
    parser_.reset();
    sink_.reset();

    state_ = State::Disconnected;
}

void LiveClient::Pause() {
    if (parser_) {
        parser_->Suspend();
    }
}

void LiveClient::Resume() {
    if (parser_) {
        parser_->Resume();
    }
}

const std::string& LiveClient::GetSessionId() const {
    static const std::string empty;
    return protocol_ ? protocol_->GetSessionId() : empty;
}

void LiveClient::HandleRecord(const databento::Record& rec) {
    if (stopped_) return;
    if (record_handler_) {
        record_handler_(rec);
    }
}

void LiveClient::HandlePipelineError(const Error& e) {
    if (stopped_) return;
    state_ = State::Error;
    if (error_handler_) {
        error_handler_(e);
    }
}

void LiveClient::HandlePipelineComplete() {
    if (stopped_) return;
    state_ = State::Complete;
    if (complete_handler_) {
        complete_handler_();
    }
}

}  // namespace dbn_pipe
```

**Step 3: Update BUILD rules**

Update `src/BUILD.bazel` for live_client:

```python
cc_library(
    name = "live_client",
    srcs = ["live_client.cpp"],
    hdrs = ["live_client.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":dbn_parser_component",
        ":error",
        ":live_protocol_handler",
        ":reactor",
        ":tcp_socket",
        "@databento-cpp",
    ],
)
```

**Step 4: Update tests**

Update `tests/live_client_test.cpp` to work with new interface:

```cpp
// tests/live_client_test.cpp
#include <gtest/gtest.h>

#include <memory>

#include "src/live_client.hpp"
#include "src/reactor.hpp"

using namespace dbn_pipe;

TEST(LiveClientTest, ConstructsWithApiKey) {
    Reactor reactor;
    LiveClient client(reactor, "test-api-key");

    EXPECT_EQ(client.GetState(), LiveClient::State::Disconnected);
}

TEST(LiveClientTest, ConnectChangesState) {
    Reactor reactor;
    LiveClient client(reactor, "test-api-key");

    sockaddr_storage addr{};
    auto* sin = reinterpret_cast<sockaddr_in*>(&addr);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(12345);
    sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    client.Connect(addr);

    EXPECT_EQ(client.GetState(), LiveClient::State::Connecting);
}

TEST(LiveClientTest, StopResetsState) {
    Reactor reactor;
    LiveClient client(reactor, "test-api-key");

    sockaddr_storage addr{};
    auto* sin = reinterpret_cast<sockaddr_in*>(&addr);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(12345);
    sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    client.Connect(addr);
    client.Stop();

    EXPECT_EQ(client.GetState(), LiveClient::State::Disconnected);
}

TEST(LiveClientTest, ConnectWhenNotDisconnectedErrors) {
    Reactor reactor;
    LiveClient client(reactor, "test-api-key");

    bool got_error = false;
    client.OnError([&](const Error& e) {
        got_error = true;
        EXPECT_EQ(e.code, ErrorCode::InvalidState);
    });

    sockaddr_storage addr{};
    auto* sin = reinterpret_cast<sockaddr_in*>(&addr);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(12345);
    sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    client.Connect(addr);
    client.Connect(addr);  // Second connect should error

    EXPECT_TRUE(got_error);
}
```

**Step 5: Run tests to verify they pass**

Run: `bazel test //tests:live_client_test`
Expected: PASS

**Step 6: Commit**

```bash
git add src/live_client.hpp src/live_client.cpp src/BUILD.bazel tests/live_client_test.cpp
git commit -m "refactor(live): rewrite LiveClient as pipeline orchestrator"
```

---

## Task 5: Add DbnParserComponent to HistoricalClient

**Files:**
- Modify: `src/historical_client.hpp`
- Modify: `src/historical_client.cpp`
- Modify: `src/BUILD.bazel`

**Step 1: Update HistoricalClient to include DbnParserComponent**

Update `src/historical_client.hpp` to add DbnParserComponent after ZstdDecompressor:

```cpp
// Add to includes
#include "dbn_parser_component.hpp"

// Update Sink to be a Sink (receives records, not bytes)
class Sink {
public:
    explicit Sink(HistoricalClient* client) : client_(client) {}

    void Invalidate() { valid_ = false; }

    void OnRecord(const databento::Record& rec);
    void OnError(const Error& e);
    void OnDone();

private:
    HistoricalClient* client_;
    bool valid_ = true;
};

// Update pipeline type aliases in private section
using ParserType = DbnParserComponent<Sink>;
using ZstdType = ZstdDecompressor<ParserType>;
using HttpType = HttpClient<ZstdType>;
using TlsType = TlsSocket<HttpType>;

// Update members
std::shared_ptr<Sink> sink_;
std::shared_ptr<ParserType> parser_;
std::shared_ptr<ZstdType> decompressor_;
std::shared_ptr<HttpType> http_;
std::shared_ptr<TlsType> tls_;
std::unique_ptr<TcpSocket> tcp_;

// Update handler declarations
void HandleRecord(const databento::Record& rec);
void HandlePipelineError(const Error& e);
void HandlePipelineComplete();
```

**Step 2: Update HistoricalClient implementation**

Update `src/historical_client.cpp`:

```cpp
// Sink now receives records
void Sink::OnRecord(const databento::Record& rec) {
    if (!valid_ || !client_) return;
    client_->HandleRecord(rec);
}

// In BuildPipeline(), create parser between zstd and sink:
void HistoricalClient::BuildPipeline() {
    sink_ = std::make_shared<Sink>(this);
    parser_ = ParserType::Create(reactor_, sink_);
    decompressor_ = ZstdType::Create(reactor_, parser_);
    http_ = HttpType::Create(reactor_, decompressor_);
    tls_ = TlsType::Create(reactor_, http_);

    // Set upstream pointers
    parser_->SetUpstream(decompressor_.get());
    decompressor_->SetUpstream(http_.get());
    http_->SetUpstream(tls_.get());

    // ... rest of pipeline wiring
}

// Update handler to receive records
void HistoricalClient::HandleRecord(const databento::Record& rec) {
    if (stopped_) return;
    if (state_ == State::ReceivingResponse) {
        state_ = State::Streaming;
    }
    if (record_handler_) {
        record_handler_(rec);
    }
}
```

**Step 3: Update BUILD rules**

Add dbn_parser_component dependency to historical_client in `src/BUILD.bazel`:

```python
cc_library(
    name = "historical_client",
    srcs = ["historical_client.cpp"],
    hdrs = ["historical_client.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":dbn_parser_component",
        ":error",
        ":http_client",
        ":reactor",
        ":tcp_socket",
        ":tls_socket",
        ":zstd_decompressor",
        "@databento-cpp",
    ],
)
```

**Step 4: Run all tests**

Run: `bazel test //tests/...`
Expected: All tests PASS

**Step 5: Commit**

```bash
git add src/historical_client.hpp src/historical_client.cpp src/BUILD.bazel
git commit -m "feat(historical): add DbnParserComponent to pipeline"
```

---

## Summary

| Task | Component | Purpose |
|------|-----------|---------|
| 1 | RecordDownstream | Concept for record-receiving components |
| 2 | DbnParserComponent | Pipeline component that parses DBN bytes → records |
| 3 | LiveProtocolHandler | Pipeline component handling Live API text/binary protocol |
| 4 | LiveClient refactor | Orchestrator pattern matching HistoricalClient |
| 5 | HistoricalClient update | Add DbnParserComponent to pipeline |

Final pipeline architectures:

**LiveClient:**
```
TcpSocket → LiveProtocolHandler → DbnParserComponent → Sink → LiveClient
```

**HistoricalClient:**
```
TcpSocket → TlsSocket → HttpClient → ZstdDecompressor → DbnParserComponent → Sink → HistoricalClient
```
