// tests/pipeline_test.cpp
#include <gtest/gtest.h>

#include <vector>

#include <databento/record.hpp>

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

// Mock record downstream that satisfies RecordDownstream concept
struct MockRecordDownstream {
    int record_count = 0;
    bool error_called = false;
    bool done_called = false;

    void OnRecord(const databento::Record&) { ++record_count; }
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
static_assert(TerminalDownstream<MockRecordDownstream>);
static_assert(Downstream<MockDownstream>);
static_assert(RecordDownstream<MockRecordDownstream>);
static_assert(Upstream<MockUpstream>);

// Verify MockDownstream does not satisfy RecordDownstream
static_assert(!RecordDownstream<MockDownstream>);

// Verify MockRecordDownstream does not satisfy Downstream
static_assert(!Downstream<MockRecordDownstream>);

TEST(PipelineTest, ConceptsSatisfied) {
    MockDownstream ds;
    static_assert(Downstream<MockDownstream>);
    SUCCEED();
}

TEST(PipelineTest, TerminalDownstreamConcept) {
    // Both MockDownstream and MockRecordDownstream satisfy TerminalDownstream
    static_assert(TerminalDownstream<MockDownstream>);
    static_assert(TerminalDownstream<MockRecordDownstream>);
    SUCCEED();
}

TEST(PipelineTest, RecordDownstreamConcept) {
    // MockRecordDownstream satisfies RecordDownstream
    static_assert(RecordDownstream<MockRecordDownstream>);
    // MockDownstream does not satisfy RecordDownstream (no OnRecord)
    static_assert(!RecordDownstream<MockDownstream>);
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
