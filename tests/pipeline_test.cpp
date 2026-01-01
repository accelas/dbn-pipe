// tests/unified_pipeline_test.cpp
#include <gtest/gtest.h>

#include "lib/stream/epoll_event_loop.hpp"
#include "src/live_protocol.hpp"
#include "src/pipeline.hpp"

using namespace dbn_pipe;

struct TestRecord {};

TEST(UnifiedPipelineTest, CreateReturnsSharedPtr) {
    EpollEventLoop loop;
    auto pipeline = Pipeline<LiveProtocol, TestRecord>::Create(
        loop, "test_api_key");
    ASSERT_NE(pipeline, nullptr);
}

TEST(UnifiedPipelineTest, SetRequestStoresRequest) {
    EpollEventLoop loop;
    loop.Poll(0);  // Initialize thread ID

    auto pipeline = Pipeline<LiveProtocol, TestRecord>::Create(
        loop, "test_api_key");

    LiveRequest req{"GLBX.MDP3", "ESZ4", "mbp-1"};
    pipeline->SetRequest(req);

    // Should not throw - request is set
    SUCCEED();
}

TEST(UnifiedPipelineTest, StartBeforeConnectEmitsError) {
    EpollEventLoop loop;
    loop.Poll(0);

    auto pipeline = Pipeline<LiveProtocol, TestRecord>::Create(
        loop, "test_api_key");

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
    EpollEventLoop loop;
    loop.Poll(0);

    auto pipeline = Pipeline<LiveProtocol, TestRecord>::Create(
        loop, "test_api_key");

    pipeline->Suspend();
    EXPECT_TRUE(pipeline->IsSuspended());
}

TEST(UnifiedPipelineTest, StateStartsDisconnected) {
    EpollEventLoop loop;
    loop.Poll(0);

    auto pipeline = Pipeline<LiveProtocol, TestRecord>::Create(
        loop, "test_api_key");

    EXPECT_EQ(pipeline->GetState(), PipelineState::Disconnected);
}
