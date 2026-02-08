// SPDX-License-Identifier: MIT

// tests/client_test.cpp
#include <gtest/gtest.h>

#include <memory_resource>

#include "dbn_pipe/client.hpp"
#include "dbn_pipe/stream/epoll_event_loop.hpp"

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

TEST(ClientTest, LiveClientWithCustomResource) {
    EpollEventLoop loop;
    auto resource = std::make_shared<std::pmr::synchronized_pool_resource>();
    auto client = LiveClient::Create(loop, "test_key", resource);
    ASSERT_NE(client, nullptr);
}

TEST(ClientTest, HistoricalClientWithCustomResource) {
    EpollEventLoop loop;
    auto resource = std::make_shared<std::pmr::synchronized_pool_resource>();
    auto client = HistoricalClient::Create(loop, "test_key", resource);
    ASSERT_NE(client, nullptr);
}
