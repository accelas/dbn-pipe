// tests/client_test.cpp
#include <gtest/gtest.h>

#include "src/client.hpp"
#include "src/epoll_event_loop.hpp"

using namespace dbn_pipe;

TEST(ClientTest, LiveClientIsAlias) {
    EpollEventLoop loop;
    auto client = LiveClient::Create(loop, "test_key");
    ASSERT_NE(client, nullptr);
}

TEST(ClientTest, HistoricalClientIsAlias) {
    EpollEventLoop loop;
    auto client = HistoricalClient::Create(loop, "test_key");
    ASSERT_NE(client, nullptr);
}
