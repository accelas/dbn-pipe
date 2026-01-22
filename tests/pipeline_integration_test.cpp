// tests/pipeline_integration_test.cpp
#include <gtest/gtest.h>

#include "src/client.hpp"
#include "lib/stream/epoll_event_loop.hpp"
#include "lib/stream/reactor.hpp"

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

// Tests using Reactor (the legacy event loop) with Pipeline
class ReactorPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        reactor_.Poll(0);  // Initialize thread ID
    }

    Reactor reactor_;
};

TEST_F(ReactorPipelineTest, LiveClientWithReactor) {
    // Reactor implements IEventLoop, so it should work with Pipeline::Create
    auto client = LiveClient::Create(reactor_, "test_api_key");

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

TEST_F(ReactorPipelineTest, HistoricalClientWithReactor) {
    auto client = HistoricalClient::Create(reactor_, "test_api_key");

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

TEST_F(ReactorPipelineTest, DeferredCallbackWithReactor) {
    auto client = LiveClient::Create(reactor_, "test_api_key");

    bool deferred_executed = false;
    reactor_.Defer([&]() {
        deferred_executed = true;
    });

    EXPECT_FALSE(deferred_executed);
    reactor_.Poll(0);
    EXPECT_TRUE(deferred_executed);
}

TEST_F(ReactorPipelineTest, ReactorWithTimer) {
    auto client = LiveClient::Create(reactor_, "test_api_key");

    Timer timer(reactor_);
    bool timer_fired = false;

    timer.OnTimer([&]() {
        timer_fired = true;
        reactor_.Stop();
    });

    timer.Start(10);  // 10ms one-shot timer

    reactor_.Run();  // Will exit when timer fires

    EXPECT_TRUE(timer_fired);
}
