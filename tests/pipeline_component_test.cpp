// SPDX-License-Identifier: MIT

// tests/pipeline_component_test.cpp
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"

using namespace dbn_pipe;

// Mock downstream that satisfies Downstream concept (receives BufferChain)
struct MockDownstream {
    std::vector<std::byte> received;
    bool error_called = false;
    bool done_called = false;

    void OnData(BufferChain& chain) {
        // Copy data from chain to received vector
        while (!chain.Empty()) {
            size_t chunk_size = chain.ContiguousSize();
            const std::byte* ptr = chain.DataAt(0);
            received.insert(received.end(), ptr, ptr + chunk_size);
            chain.Consume(chunk_size);
        }
    }
    void OnError(const Error&) { error_called = true; }
    void OnDone() { done_called = true; }
};

// Mock upstream that satisfies Upstream concept
struct MockUpstream {
    void Write(BufferChain) {}
    void Suspend() {}
    void Resume() {}
    void Close() {}
};

// Verify concepts compile
static_assert(TerminalDownstream<MockDownstream>);
static_assert(Downstream<MockDownstream>);
static_assert(Upstream<MockUpstream>);

TEST(PipelineTest, ConceptsSatisfied) {
    MockDownstream ds;
    static_assert(Downstream<MockDownstream>);
    SUCCEED();
}

TEST(PipelineTest, TerminalDownstreamConcept) {
    static_assert(TerminalDownstream<MockDownstream>);
    SUCCEED();
}

// Test component using PipelineComponent with D=void (default)
class TestComponent
    : public PipelineComponent<TestComponent>
    , public std::enable_shared_from_this<TestComponent> {
public:
    TestComponent(IEventLoop& loop) : PipelineComponent(loop) {}

    int process_count = 0;
    bool do_close_called = false;

    void Process() {
        auto guard = TryGuard();
        if (!guard) return;
        ++process_count;
    }

    void DisableWatchers() {}  // Required by base
    void DoClose() { do_close_called = true; }
    void ProcessPending() {}   // Required by base
    void FlushAndComplete() {}
};

TEST(PipelineComponentTest, TryGuardRejectsWhenClosed) {
    EpollEventLoop loop;
    auto comp = std::make_shared<TestComponent>(loop);

    comp->Process();
    EXPECT_EQ(comp->process_count, 1);

    comp->RequestClose();
    comp->Process();  // Should be rejected
    EXPECT_EQ(comp->process_count, 1);  // Still 1
}

TEST(PipelineComponentTest, ProcessingGuardDefersClose) {
    EpollEventLoop loop;
    auto comp = std::make_shared<TestComponent>(loop);

    {
        auto guard = comp->TryGuard();
        ASSERT_TRUE(guard.has_value());

        comp->RequestClose();  // Should defer, guard still active
        EXPECT_TRUE(comp->IsClosed());
        EXPECT_FALSE(comp->do_close_called);  // Not yet
    }
    // Guard destroyed, but DoClose is deferred to event loop

    loop.Poll(0);  // Run deferred callbacks
    EXPECT_TRUE(comp->do_close_called);
}

// =========================================================================
// New tests: default constructor + SetDefer
// =========================================================================

// Test component using default constructor (no IEventLoop)
class DeferTestComponent
    : public PipelineComponent<DeferTestComponent>
    , public std::enable_shared_from_this<DeferTestComponent> {
public:
    DeferTestComponent() = default;

    bool do_close_called = false;

    void DisableWatchers() {}
    void DoClose() { do_close_called = true; }
    void ProcessPending() {}
    void FlushAndComplete() {}
};

TEST(PipelineComponentTest, DefaultConstructorWithSetDefer) {
    bool deferred = false;
    std::function<void()> captured_fn;

    auto comp = std::make_shared<DeferTestComponent>();
    comp->SetDefer([&](std::function<void()> fn) {
        deferred = true;
        captured_fn = std::move(fn);
    });

    comp->RequestClose();

    // Defer should have been called
    EXPECT_TRUE(deferred);
    EXPECT_FALSE(comp->do_close_called);

    // Execute the deferred function
    captured_fn();
    EXPECT_TRUE(comp->do_close_called);
}

TEST(PipelineComponentTest, SynchronousCloseFallbackWhenNoDeferSet) {
    // Component without defer set - DoClose should be called synchronously
    auto comp = std::make_shared<DeferTestComponent>();

    comp->RequestClose();

    // DoClose should have been called synchronously (no defer set)
    EXPECT_TRUE(comp->do_close_called);
}

// =========================================================================
// New tests: Component with D=MockDownstream using parameterless helpers
// =========================================================================

class DownstreamTestComponent
    : public PipelineComponent<DownstreamTestComponent, MockDownstream>
    , public std::enable_shared_from_this<DownstreamTestComponent> {
    using Base = PipelineComponent<DownstreamTestComponent, MockDownstream>;
public:
    DownstreamTestComponent() = default;

    // Expose DownstreamStorage methods (protected inheritance)
    using Base::SetDownstream;
    using Base::GetDownstream;
    using Base::GetDownstreamPtr;
    using Base::ResetDownstream;

    bool do_close_called = false;

    void DisableWatchers() {}
    void DoClose() { do_close_called = true; }
    void ProcessPending() {}
    void FlushAndComplete() {}

    // Expose parameterless helpers for testing
    void TestEmitError(const Error& e) {
        EmitError(e);
    }
    void TestEmitDone() {
        EmitDone();
    }
    bool TestForwardData(BufferChain& chain) {
        return ForwardData(chain);
    }
    void TestPropagateError(const Error& e) {
        PropagateError(e);
    }
};

TEST(PipelineComponentTest, ParameterlessEmitDoneUsesStoredDownstream) {
    auto ds = std::make_shared<MockDownstream>();
    auto comp = std::make_shared<DownstreamTestComponent>();
    comp->SetDownstream(ds);

    comp->TestEmitDone();

    EXPECT_TRUE(ds->done_called);
    EXPECT_TRUE(comp->IsFinalized());
}

TEST(PipelineComponentTest, ParameterlessEmitErrorUsesStoredDownstream) {
    auto ds = std::make_shared<MockDownstream>();
    auto comp = std::make_shared<DownstreamTestComponent>();
    comp->SetDownstream(ds);

    Error err{ErrorCode::ConnectionFailed, "test error"};
    comp->TestEmitError(err);

    EXPECT_TRUE(ds->error_called);
    EXPECT_TRUE(comp->IsFinalized());
}

TEST(PipelineComponentTest, ParameterlessForwardDataUsesStoredDownstream) {
    auto ds = std::make_shared<MockDownstream>();
    auto comp = std::make_shared<DownstreamTestComponent>();
    comp->SetDownstream(ds);

    SegmentAllocator alloc;
    auto seg = alloc.Allocate();
    const std::byte data[] = {std::byte{0x41}, std::byte{0x42}, std::byte{0x43}};
    seg->Append(data, 3);
    BufferChain chain;
    chain.Append(std::move(seg));

    bool suspended = comp->TestForwardData(chain);

    EXPECT_FALSE(suspended);
    EXPECT_EQ(ds->received.size(), 3u);
    EXPECT_EQ(ds->received[0], std::byte{0x41});
    EXPECT_EQ(ds->received[1], std::byte{0x42});
    EXPECT_EQ(ds->received[2], std::byte{0x43});
}

TEST(PipelineComponentTest, ParameterlessPropagateErrorUsesStoredDownstream) {
    auto ds = std::make_shared<MockDownstream>();
    auto comp = std::make_shared<DownstreamTestComponent>();
    comp->SetDownstream(ds);

    Error err{ErrorCode::ConnectionFailed, "propagated error"};
    comp->TestPropagateError(err);

    EXPECT_TRUE(ds->error_called);
    EXPECT_TRUE(comp->IsFinalized());
    EXPECT_TRUE(comp->IsClosed());
}

TEST(PipelineComponentTest, DownstreamStorageSetAndReset) {
    auto ds = std::make_shared<MockDownstream>();
    auto comp = std::make_shared<DownstreamTestComponent>();

    comp->SetDownstream(ds);
    EXPECT_EQ(&comp->GetDownstream(), ds.get());
    EXPECT_EQ(comp->GetDownstreamPtr().use_count(), 2);  // ds + downstream_

    comp->ResetDownstream();
    EXPECT_EQ(comp->GetDownstreamPtr(), nullptr);
}

// Component without enable_shared_from_this — tests raw-pointer path in ScheduleClose
class NonSharedComponent
    : public PipelineComponent<NonSharedComponent> {
public:
    NonSharedComponent() = default;

    bool do_close_called = false;

    void DisableWatchers() {}
    void DoClose() { do_close_called = true; }
    void ProcessPending() {}
    void FlushAndComplete() {}
};

TEST(PipelineComponentTest, DeferredCloseWithoutSharedFromThis) {
    std::function<void()> captured_fn;
    NonSharedComponent comp;
    comp.SetDefer([&](std::function<void()> fn) {
        captured_fn = std::move(fn);
    });

    comp.RequestClose();
    EXPECT_FALSE(comp.do_close_called);

    captured_fn();
    EXPECT_TRUE(comp.do_close_called);
}

TEST(PipelineComponentTest, SynchronousCloseWithoutSharedFromThis) {
    NonSharedComponent comp;
    // No defer set — synchronous fallback
    comp.RequestClose();
    EXPECT_TRUE(comp.do_close_called);
}

TEST(PipelineComponentTest, SetDeferWithEventLoopBridge) {
    EpollEventLoop loop;
    auto comp = std::make_shared<TestComponent>(loop);

    // Bridge constructor should have set up defer_ internally
    // Verify by closing - should defer to event loop
    comp->RequestClose();
    EXPECT_FALSE(comp->do_close_called);

    loop.Poll(0);
    EXPECT_TRUE(comp->do_close_called);
}
