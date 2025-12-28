// tests/historical_client_test.cpp
#include <gtest/gtest.h>

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
