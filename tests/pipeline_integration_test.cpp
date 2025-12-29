// tests/pipeline_integration_test.cpp
#include <gtest/gtest.h>

#include "src/client.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

class PipelineIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        reactor_.Poll(0);  // Initialize thread ID
    }

    Reactor reactor_;
};

TEST_F(PipelineIntegrationTest, LiveClientLifecycle) {
    auto client = LiveClient::Create(reactor_, "test_api_key");

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
    EXPECT_EQ(client->GetState(), PipelineState::Disconnected);

    // Stop before connect is safe
    client->Stop();
    EXPECT_EQ(client->GetState(), PipelineState::Stopping);
}

TEST_F(PipelineIntegrationTest, HistoricalClientLifecycle) {
    auto client = HistoricalClient::Create(reactor_, "test_api_key");

    HistoricalRequest req{
        "GLBX.MDP3",
        "ESZ4",
        "mbp-1",
        1704067200000000000ULL,
        1704153600000000000ULL
    };
    client->SetRequest(req);

    EXPECT_EQ(client->GetState(), PipelineState::Disconnected);
}

TEST_F(PipelineIntegrationTest, SuspendResumeBeforeConnect) {
    auto client = LiveClient::Create(reactor_, "test_api_key");

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
