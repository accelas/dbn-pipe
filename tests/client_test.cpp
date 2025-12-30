// tests/client_test.cpp
#include <gtest/gtest.h>

#include "src/client.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

TEST(ClientTest, LiveClientIsAlias) {
    Reactor reactor;
    auto client = LiveClient::Create(reactor, "test_key");
    ASSERT_NE(client, nullptr);
}

TEST(ClientTest, HistoricalClientIsAlias) {
    Reactor reactor;
    auto client = HistoricalClient::Create(reactor, "test_key");
    ASSERT_NE(client, nullptr);
}
