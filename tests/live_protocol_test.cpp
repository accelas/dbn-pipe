// SPDX-License-Identifier: MIT

// tests/live_protocol_test.cpp
#include <gtest/gtest.h>

#include <memory>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/stream/protocol.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"
#include "dbn_pipe/stream/sink.hpp"
#include "dbn_pipe/live_protocol.hpp"

using namespace dbn_pipe;

TEST(LiveProtocolTest, SatisfiesProtocolConcept) {
    static_assert(Protocol<LiveProtocol>);
    SUCCEED();
}

TEST(LiveProtocolTest, RequestHasRequiredFields) {
    LiveProtocol::Request req;
    req.dataset = "GLBX.MDP3";
    req.symbols = "ESZ4";
    req.schema = "mbp-1";

    EXPECT_EQ(req.dataset, "GLBX.MDP3");
    EXPECT_EQ(req.symbols, "ESZ4");
    EXPECT_EQ(req.schema, "mbp-1");
}

TEST(LiveProtocolTest, BuildChainWithoutAllocator) {
    EpollEventLoop loop;
    StreamRecordSink sink(
        [](RecordBatch&&) {},
        [](const Error&) {},
        []() {}
    );

    auto chain = LiveProtocol::BuildChain(loop, sink, "test_key");
    ASSERT_NE(chain, nullptr);
}

TEST(LiveProtocolTest, BuildChainWithAllocator) {
    EpollEventLoop loop;
    StreamRecordSink sink(
        [](RecordBatch&&) {},
        [](const Error&) {},
        []() {}
    );

    SegmentAllocator alloc;
    auto chain = LiveProtocol::BuildChain(loop, sink, "test_key", &alloc);
    ASSERT_NE(chain, nullptr);
}

TEST(LiveProtocolTest, BuildChainWithCustomResource) {
    EpollEventLoop loop;
    StreamRecordSink sink(
        [](RecordBatch&&) {},
        [](const Error&) {},
        []() {}
    );

    // Create allocator with a custom PMR resource
    auto resource = std::make_shared<std::pmr::synchronized_pool_resource>();
    SegmentAllocator alloc(resource);
    auto chain = LiveProtocol::BuildChain(loop, sink, "test_key", &alloc);
    ASSERT_NE(chain, nullptr);
}
