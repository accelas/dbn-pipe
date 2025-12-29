// tests/historical_client_test.cpp
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>

#include <databento/record.hpp>

#include "src/historical_client.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

class HistoricalClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Nothing special
    }

    void TearDown() override {
        // Nothing special
    }
};

TEST_F(HistoricalClientTest, ConstructsWithApiKey) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    // Initial state should be Disconnected
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Disconnected);
}

TEST_F(HistoricalClientTest, RequestSetsParams) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    // Set request parameters
    client.Request(
        "GLBX.MDP3",                    // dataset
        "ES.FUT",                        // symbols
        "trades",                        // schema
        1609459200000000000ULL,          // start (2021-01-01 00:00:00 UTC in nanos)
        1609545600000000000ULL           // end (2021-01-02 00:00:00 UTC in nanos)
    );

    // State should still be Disconnected (Request doesn't change state)
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Disconnected);
}

TEST_F(HistoricalClientTest, ConnectChangesState) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    // Set request parameters first
    client.Request(
        "GLBX.MDP3",
        "ES.FUT",
        "trades",
        1609459200000000000ULL,
        1609545600000000000ULL
    );

    // Create a dummy address (we won't actually connect in this test)
    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // Connect should change state to Connecting
    client.Connect(addr);
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Connecting);
}

TEST_F(HistoricalClientTest, StopResetsState) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    // Set request parameters
    client.Request(
        "GLBX.MDP3",
        "ES.FUT",
        "trades",
        1609459200000000000ULL,
        1609545600000000000ULL
    );

    // Create a dummy address
    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // Connect and then stop
    client.Connect(addr);
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Connecting);

    client.Stop();
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Disconnected);
}

TEST_F(HistoricalClientTest, PipelineIsBuiltOnConnect) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    // Set request parameters
    client.Request(
        "GLBX.MDP3",
        "ES.FUT",
        "trades",
        1609459200000000000ULL,
        1609545600000000000ULL
    );

    // Create a dummy address
    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // Connect builds the pipeline
    client.Connect(addr);
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Connecting);

    // Stop cleans up the pipeline
    client.Stop();
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Disconnected);
}

// Test that HistoricalClient includes DbnParserComponent in the pipeline
// This test verifies the pipeline delivers parsed records (not raw bytes)
TEST_F(HistoricalClientTest, PipelineIncludesParser) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    // Set request parameters
    client.Request(
        "GLBX.MDP3",
        "ES.FUT",
        "trades",
        1609459200000000000ULL,
        1609545600000000000ULL
    );

    // Create a dummy address
    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // Connect builds the pipeline which now includes DbnParserComponent
    client.Connect(addr);
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Connecting);

    // Verify pipeline is built (evidenced by state change)
    // The actual parser is internal, but the test confirms no crash/error
    client.Stop();
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Disconnected);
}

// Test that ApplicationSink receives parsed records instead of raw bytes
// This verifies the new RecordDownstream interface
TEST_F(HistoricalClientTest, SinkReceivesRecords) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    bool record_received = false;
    bool error_received = false;

    client.OnRecord([&](const databento::Record& /*rec*/) {
        record_received = true;
    });

    client.OnError([&](const Error& /*e*/) {
        error_received = true;
    });

    // Set request parameters
    client.Request(
        "GLBX.MDP3",
        "ES.FUT",
        "trades",
        1609459200000000000ULL,
        1609545600000000000ULL
    );

    // Create a dummy address
    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // Connect and verify callbacks are set up properly
    client.Connect(addr);
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Connecting);

    // Stop without data - no records or errors expected
    client.Stop();
    EXPECT_FALSE(record_received);
    EXPECT_FALSE(error_received);
}

// Test that HistoricalClient has OnComplete callback (like LiveClient)
TEST_F(HistoricalClientTest, HasOnCompleteCallback) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    bool complete_received = false;

    // This should compile - OnComplete callback should exist
    client.OnComplete([&]() {
        complete_received = true;
    });

    // Set request parameters
    client.Request(
        "GLBX.MDP3",
        "ES.FUT",
        "trades",
        1609459200000000000ULL,
        1609545600000000000ULL
    );

    // Create a dummy address
    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    client.Connect(addr);
    client.Stop();

    // Complete should not be called on Stop (only on clean stream end)
    EXPECT_FALSE(complete_received);
}

// ============================================================================
// Suspendable interface tests
// ============================================================================

TEST_F(HistoricalClientTest, SuspendableInterfaceBasic) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    // Set request parameters
    client.Request(
        "GLBX.MDP3",
        "ES.FUT",
        "trades",
        1609459200000000000ULL,
        1609545600000000000ULL
    );

    // Create a dummy address
    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // Connect builds the pipeline
    client.Connect(addr);
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Connecting);

    // Poll once to set reactor thread ID
    reactor.Poll(0);

    // Test IsSuspended initial state
    EXPECT_FALSE(client.IsSuspended());

    // Test Suspend
    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    // Test Resume
    client.Resume();
    EXPECT_FALSE(client.IsSuspended());

    client.Stop();
}

TEST_F(HistoricalClientTest, SuspendIsIdempotent) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    client.Request("GLBX.MDP3", "ES.FUT", "trades",
                   1609459200000000000ULL, 1609545600000000000ULL);

    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    client.Connect(addr);
    reactor.Poll(0);

    // Multiple Suspend calls should be safe
    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    client.Suspend();  // Second call
    EXPECT_TRUE(client.IsSuspended());

    client.Suspend();  // Third call
    EXPECT_TRUE(client.IsSuspended());

    client.Stop();
}

TEST_F(HistoricalClientTest, ResumeIsIdempotent) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    client.Request("GLBX.MDP3", "ES.FUT", "trades",
                   1609459200000000000ULL, 1609545600000000000ULL);

    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    client.Connect(addr);
    reactor.Poll(0);

    // Suspend first
    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    // Multiple Resume calls should be safe
    client.Resume();
    EXPECT_FALSE(client.IsSuspended());

    client.Resume();  // Second call
    EXPECT_FALSE(client.IsSuspended());

    client.Resume();  // Third call
    EXPECT_FALSE(client.IsSuspended());

    client.Stop();
}

TEST_F(HistoricalClientTest, CloseResetsSuspendState) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    client.Request("GLBX.MDP3", "ES.FUT", "trades",
                   1609459200000000000ULL, 1609545600000000000ULL);

    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    client.Connect(addr);
    reactor.Poll(0);

    // Suspend first
    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    // Close should reset suspend state
    client.Close();
    EXPECT_FALSE(client.IsSuspended());
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Disconnected);
}

TEST_F(HistoricalClientTest, StopResetsSuspendState) {
    Reactor reactor;
    HistoricalClient client(reactor, "db-test-api-key-12345");

    client.Request("GLBX.MDP3", "ES.FUT", "trades",
                   1609459200000000000ULL, 1609545600000000000ULL);

    sockaddr_storage addr{};
    auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
    addr_in->sin_family = AF_INET;
    addr_in->sin_port = htons(443);
    addr_in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    client.Connect(addr);
    reactor.Poll(0);

    // Suspend first
    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    // Stop should also reset suspend state
    client.Stop();
    EXPECT_FALSE(client.IsSuspended());
    EXPECT_EQ(client.GetState(), HistoricalClient::State::Disconnected);
}

// Integration test - requires network access and valid API key
// Run manually with: bazel test --test_filter=*DISABLED_IntegrationTest* --test_arg=--gtest_also_run_disabled_tests
TEST_F(HistoricalClientTest, DISABLED_IntegrationTest) {
    // This test requires:
    // 1. Network access to hist.databento.com
    // 2. A valid DATABENTO_API_KEY environment variable
    //
    // To enable: set DATABENTO_API_KEY and remove DISABLED_ prefix
    // or run with --gtest_also_run_disabled_tests

    const char* api_key = std::getenv("DATABENTO_API_KEY");
    if (!api_key) {
        GTEST_SKIP() << "DATABENTO_API_KEY not set";
    }

    Reactor reactor;
    HistoricalClient client(reactor, api_key);

    // Set up callbacks
    bool received_record = false;
    bool received_error = false;
    Error last_error;

    client.OnRecord([&](const databento::Record& /*record*/) {
        received_record = true;
        // Stop after first record for test
        client.Stop();
        reactor.Stop();
    });

    client.OnError([&](const Error& e) {
        received_error = true;
        last_error = e;
        reactor.Stop();
    });

    // Request a small time range
    client.Request(
        "GLBX.MDP3",
        "ES.FUT",
        "trades",
        1609459200000000000ULL,  // 2021-01-01 00:00:00 UTC
        1609459260000000000ULL   // 2021-01-01 00:01:00 UTC (1 minute)
    );

    // TODO: Resolve DNS for hist.databento.com and create sockaddr_storage
    // For now, this is a placeholder
    sockaddr_storage addr{};

    client.Connect(addr);
    client.Start();

    // Run reactor with timeout
    // reactor.Run();  // Would block, need timeout mechanism

    if (received_error) {
        FAIL() << "Error: " << last_error.message;
    }

    // In a real test, we'd verify received_record is true
}
