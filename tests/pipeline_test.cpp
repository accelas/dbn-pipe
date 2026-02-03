// SPDX-License-Identifier: MIT

// tests/pipeline_test.cpp
// Tests for StreamingClient (the unified pipeline wrapper)
#include <gtest/gtest.h>

#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/client.hpp"

using namespace dbn_pipe;

TEST(StreamingClientTest, CreateReturnsSharedPtr) {
    EpollEventLoop loop;
    auto client = LiveClient::Create(loop, "test_api_key");
    ASSERT_NE(client, nullptr);
}

TEST(StreamingClientTest, SetRequestStoresRequest) {
    EpollEventLoop loop;
    loop.Poll(0);  // Initialize thread ID

    auto client = LiveClient::Create(loop, "test_api_key");

    LiveRequest req{"GLBX.MDP3", "ESZ4", "mbp-1"};
    client->SetRequest(req);

    // Should not throw - request is set
    SUCCEED();
}

TEST(StreamingClientTest, StartBeforeConnectEmitsError) {
    EpollEventLoop loop;
    loop.Poll(0);

    auto client = LiveClient::Create(loop, "test_api_key");

    bool error_received = false;
    client->OnError([&](const Error& e) {
        error_received = true;
        EXPECT_EQ(e.code, ErrorCode::InvalidState);
    });

    LiveRequest req{"GLBX.MDP3", "ESZ4", "mbp-1"};
    client->SetRequest(req);
    client->Start();  // Should emit error - not connected

    EXPECT_TRUE(error_received);
}

TEST(StreamingClientTest, SuspendBeforeConnectIsRespected) {
    EpollEventLoop loop;
    loop.Poll(0);

    auto client = LiveClient::Create(loop, "test_api_key");

    client->Suspend();
    EXPECT_TRUE(client->IsSuspended());
}

TEST(StreamingClientTest, StateStartsDisconnected) {
    EpollEventLoop loop;
    loop.Poll(0);

    auto client = LiveClient::Create(loop, "test_api_key");

    EXPECT_EQ(client->GetState(), ClientState::Disconnected);
}
