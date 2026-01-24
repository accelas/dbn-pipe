// SPDX-License-Identifier: MIT

// tests/pipeline_integration_test.cpp
#include <gtest/gtest.h>

#include "src/client.hpp"
#include "lib/stream/epoll_event_loop.hpp"
#include "lib/stream/event.hpp"
#include "lib/stream/timer.hpp"

using namespace dbn_pipe;

class PipelineIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        loop_.Poll(0);  // Initialize thread ID
    }

    EpollEventLoop loop_;
};

TEST_F(PipelineIntegrationTest, LiveClientLifecycle) {
    auto client = LiveClient::Create(loop_, "test_api_key");

    bool error_received = false;
    bool complete_received = false;

    client->OnError([&](const Error& /*e*/) {
        error_received = true;
    });

    client->OnComplete([&]() {
        complete_received = true;
    });

    // Set request
    LiveRequest req{"GLBX.MDP3", "ESZ4", "mbp-1"};
    client->SetRequest(req);

    // State should still be disconnected
    EXPECT_EQ(client->GetState(), ClientState::Disconnected);

    // Stop before connect is safe
    client->Stop();
    EXPECT_EQ(client->GetState(), ClientState::Stopping);
}

TEST_F(PipelineIntegrationTest, HistoricalClientLifecycle) {
    auto client = HistoricalClient::Create(loop_, "test_api_key");

    HistoricalRequest req{
        "GLBX.MDP3",
        "ESZ4",
        "mbp-1",
        1704067200000000000ULL,
        1704153600000000000ULL,
        "",  // stype_in (empty = default raw_symbol)
        ""   // stype_out (empty = no symbol mappings)
    };
    client->SetRequest(req);

    EXPECT_EQ(client->GetState(), ClientState::Disconnected);
}

TEST_F(PipelineIntegrationTest, SuspendResumeBeforeConnect) {
    auto client = LiveClient::Create(loop_, "test_api_key");

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

// Tests using EpollEventLoop with Pipeline
class EpollEventLoopPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        loop_.Poll(0);  // Initialize thread ID
    }

    EpollEventLoop loop_;
};

TEST_F(EpollEventLoopPipelineTest, LiveClientWithEpollEventLoop) {
    // EpollEventLoop implements IEventLoop, so it should work with Pipeline::Create
    auto client = LiveClient::Create(loop_, "test_api_key");

    bool error_received = false;
    bool complete_received = false;

    client->OnError([&](const Error& /*e*/) {
        error_received = true;
    });

    client->OnComplete([&]() {
        complete_received = true;
    });

    LiveRequest req{"GLBX.MDP3", "ESZ4", "mbp-1"};
    client->SetRequest(req);

    EXPECT_EQ(client->GetState(), ClientState::Disconnected);

    client->Stop();
    EXPECT_EQ(client->GetState(), ClientState::Stopping);
}

TEST_F(EpollEventLoopPipelineTest, HistoricalClientWithEpollEventLoop) {
    auto client = HistoricalClient::Create(loop_, "test_api_key");

    HistoricalRequest req{
        "GLBX.MDP3",
        "ESZ4",
        "mbp-1",
        1704067200000000000ULL,
        1704153600000000000ULL,
        "",  // stype_in (empty = default raw_symbol)
        ""   // stype_out (empty = no symbol mappings)
    };
    client->SetRequest(req);

    EXPECT_EQ(client->GetState(), ClientState::Disconnected);
}

TEST_F(EpollEventLoopPipelineTest, DeferredCallbackWithEpollEventLoop) {
    auto client = LiveClient::Create(loop_, "test_api_key");

    bool deferred_executed = false;
    loop_.Defer([&]() {
        deferred_executed = true;
    });

    EXPECT_FALSE(deferred_executed);
    loop_.Poll(0);
    EXPECT_TRUE(deferred_executed);
}

TEST_F(EpollEventLoopPipelineTest, EpollEventLoopWithTimer) {
    auto client = LiveClient::Create(loop_, "test_api_key");

    Timer timer(loop_);
    bool timer_fired = false;

    timer.OnTimer([&]() {
        timer_fired = true;
        loop_.Stop();
    });

    timer.Start(10);  // 10ms one-shot timer

    loop_.Run();  // Will exit when timer fires

    EXPECT_TRUE(timer_fired);
}
