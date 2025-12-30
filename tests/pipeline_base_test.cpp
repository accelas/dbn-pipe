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

using TestPipelineBase = PipelineBase<MockRecord>;
using TestSink = Sink<MockRecord>;

TEST(PipelineBaseTest, SinkInvalidationStopsCallbacks) {
    Reactor reactor;
    reactor.Poll(0);  // Initialize thread ID

    bool record_called = false;
    bool error_called = false;
    bool complete_called = false;

    // Create a mock pipeline that tracks calls
    struct MockPipeline : TestPipelineBase {
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

    TestSink sink(reactor, &pipeline);

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

    struct MockPipeline : TestPipelineBase {
        void HandleRecord(const MockRecord&) override {}
        void HandlePipelineError(const Error&) override {}
        void HandlePipelineComplete() override {}
    };

    MockPipeline pipeline;
    TestSink sink(reactor, &pipeline);

    // Should not crash when called from reactor thread
    sink.OnComplete();
    SUCCEED();
}
