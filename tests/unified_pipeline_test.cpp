// tests/unified_pipeline_test.cpp
#include <gtest/gtest.h>

#include "src/unified_pipeline.hpp"
#include "src/live_protocol.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

struct TestRecord {};

TEST(UnifiedPipelineTest, CreateReturnsSharedPtr) {
    Reactor reactor;
    auto pipeline = Pipeline<LiveProtocol, TestRecord>::Create(
        reactor, "test_api_key");
    ASSERT_NE(pipeline, nullptr);
}

TEST(UnifiedPipelineTest, SetRequestStoresRequest) {
    Reactor reactor;
    reactor.Poll(0);  // Initialize thread ID

    auto pipeline = Pipeline<LiveProtocol, TestRecord>::Create(
        reactor, "test_api_key");

    LiveRequest req{"GLBX.MDP3", "ESZ4", "mbp-1"};
    pipeline->SetRequest(req);

    // Should not throw - request is set
    SUCCEED();
}

TEST(UnifiedPipelineTest, StartBeforeConnectEmitsError) {
    Reactor reactor;
    reactor.Poll(0);

    auto pipeline = Pipeline<LiveProtocol, TestRecord>::Create(
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

    auto pipeline = Pipeline<LiveProtocol, TestRecord>::Create(
        reactor, "test_api_key");

    pipeline->Suspend();
    EXPECT_TRUE(pipeline->IsSuspended());
}

TEST(UnifiedPipelineTest, StateStartsDisconnected) {
    Reactor reactor;
    reactor.Poll(0);

    auto pipeline = Pipeline<LiveProtocol, TestRecord>::Create(
        reactor, "test_api_key");

    EXPECT_EQ(pipeline->GetState(), PipelineState::Disconnected);
}
