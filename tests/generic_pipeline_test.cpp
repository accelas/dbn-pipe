// SPDX-License-Identifier: MIT

// tests/generic_pipeline_test.cpp
#include <gtest/gtest.h>

#include "lib/stream/pipeline.hpp"
#include "lib/stream/epoll_event_loop.hpp"

using namespace dbn_pipe;

// Mock types from protocol_concept_test
struct MockRequest {};

struct MockChain {
    void Connect(const sockaddr_storage&) { connect_called = true; }
    void Close() { close_called = true; }
    void SetReadyCallback(std::function<void()> cb) { ready_cb = std::move(cb); }
    void SetDataset(const std::string&) {}
    void Suspend() { suspended = true; }
    void Resume() { suspended = false; }
    bool IsSuspended() const { return suspended; }

    bool connect_called = false;
    bool close_called = false;
    bool suspended = false;
    std::function<void()> ready_cb;
};

struct MockSink {
    void OnError(const Error&) {}
    void OnComplete() {}
    void Invalidate() { invalidated = true; }
    bool invalidated = false;
};

struct MockProtocol {
    using Request = MockRequest;
    using SinkType = MockSink;
    using ChainType = MockChain;

    static inline std::shared_ptr<MockChain> last_chain;
    static inline MockSink* last_sink;

    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop&, SinkType& sink, const std::string&) {
        last_chain = std::make_shared<ChainType>();
        last_sink = &sink;
        return last_chain;
    }

    static void SendRequest(std::shared_ptr<ChainType>&, const Request&) {}
    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) chain->Close();
    }
    static std::string GetHostname(const Request&) { return "test.com"; }
    static uint16_t GetPort(const Request&) { return 443; }
};

TEST(GenericPipelineTest, CreateReturnsPipeline) {
    EpollEventLoop loop;
    loop.Poll(0);

    auto sink = std::make_shared<MockSink>();
    auto chain = std::make_shared<MockChain>();

    auto pipeline = std::make_shared<Pipeline<MockProtocol>>(
        Pipeline<MockProtocol>::PrivateTag{},
        loop, chain, sink, MockRequest{});

    EXPECT_NE(pipeline, nullptr);
}

TEST(GenericPipelineTest, StopInvalidatesSink) {
    EpollEventLoop loop;
    loop.Poll(0);

    auto sink = std::make_shared<MockSink>();
    auto chain = std::make_shared<MockChain>();

    auto pipeline = std::make_shared<Pipeline<MockProtocol>>(
        Pipeline<MockProtocol>::PrivateTag{},
        loop, chain, sink, MockRequest{});

    pipeline->Stop();

    EXPECT_TRUE(sink->invalidated);
}
