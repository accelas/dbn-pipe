// tests/pipeline_test.cpp
#include <gtest/gtest.h>

#include <vector>

#include "src/pipeline.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

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

// Mock upstream that satisfies Upstream concept
struct MockUpstream {
    void Write(std::pmr::vector<std::byte>) {}
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

    // Suspendable hooks (required by base)
    void OnSuspend() {}
    void OnResume() {}
    void FlushAndComplete() {}
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
