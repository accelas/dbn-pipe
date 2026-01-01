// tests/pipeline_base_test.cpp
#include <gtest/gtest.h>

#include "lib/stream/epoll_event_loop.hpp"
#include "lib/stream/error.hpp"
#include "src/pipeline_sink.hpp"

using namespace dbn_pipe;

// Mock record for testing
struct MockRecord {
    int value;
};

using TestPipelineBase = PipelineBase<MockRecord>;
using TestSink = Sink<MockRecord>;

TEST(PipelineBaseTest, SinkInvalidationStopsCallbacks) {
    EpollEventLoop loop;
    loop.Poll(0);  // Initialize thread ID

    bool record_called = false;
    bool error_called = false;
    bool complete_called = false;

    // Create a mock pipeline that tracks calls
    struct MockPipeline : TestPipelineBase {
        bool* record_flag;
        bool* error_flag;
        bool* complete_flag;

        void HandleRecord(const MockRecord&) override { *record_flag = true; }
        void HandleRecordBatch(RecordBatch&&) override {}
        void HandlePipelineError(const Error&) override { *error_flag = true; }
        void HandlePipelineComplete() override { *complete_flag = true; }
    };

    MockPipeline pipeline;
    pipeline.record_flag = &record_called;
    pipeline.error_flag = &error_called;
    pipeline.complete_flag = &complete_called;

    TestSink sink(loop, &pipeline);

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

TEST(PipelineBaseTest, SinkRequiresEventLoopThread) {
    EpollEventLoop loop;
    loop.Poll(0);  // Initialize thread ID

    struct MockPipeline : TestPipelineBase {
        void HandleRecord(const MockRecord&) override {}
        void HandleRecordBatch(RecordBatch&&) override {}
        void HandlePipelineError(const Error&) override {}
        void HandlePipelineComplete() override {}
    };

    MockPipeline pipeline;
    TestSink sink(loop, &pipeline);

    // Should not crash when called from event loop thread
    sink.OnComplete();
    SUCCEED();
}
