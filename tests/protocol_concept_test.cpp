// tests/protocol_concept_test.cpp
#include <gtest/gtest.h>

#include "lib/stream/protocol.hpp"
#include "lib/stream/sink.hpp"
#include "lib/stream/error.hpp"
#include "lib/stream/event_loop.hpp"

using namespace dbn_pipe;

// Mock types for testing
struct MockRequest {};
struct MockChain {
    void Connect(const sockaddr_storage&) {}
    void Close() {}
    void SetReadyCallback(std::function<void()>) {}
    void Suspend() {}
    void Resume() {}
    bool IsSuspended() const { return false; }
};

struct MockSink {
    void OnError(const Error&) {}
    void OnComplete() {}
    void Invalidate() {}
};

// A minimal protocol that should satisfy the concept
struct MockProtocol {
    using Request = MockRequest;
    using SinkType = MockSink;
    using ChainType = MockChain;

    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop&, SinkType&, const std::string&) {
        return std::make_shared<ChainType>();
    }

    static void SendRequest(std::shared_ptr<ChainType>&, const Request&) {}
    static void Teardown(std::shared_ptr<ChainType>&) {}
    static std::string GetHostname(const Request&) { return "test.com"; }
    static uint16_t GetPort(const Request&) { return 443; }
};

static_assert(Protocol<MockProtocol>, "MockProtocol must satisfy Protocol concept");

TEST(ProtocolConceptTest, MockProtocolSatisfiesConcept) {
    SUCCEED();
}
