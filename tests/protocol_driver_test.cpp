// tests/protocol_driver_test.cpp
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <functional>
#include <memory>
#include <string>

#include "lib/stream/event_loop.hpp"
#include "src/pipeline_sink.hpp"
#include "src/protocol_driver.hpp"

using namespace dbn_pipe;

// Mock sink for testing
struct MockRecord {};
using TestSink = Sink<MockRecord>;

// Mock chain type - satisfies ChainType interface requirements
struct MockChain {
    void Connect(const sockaddr_storage&) {}
    void SetReadyCallback(std::function<void()>) {}
    void Suspend() {}
    void Resume() {}
    void Close() {}
};

// Valid protocol driver for testing
struct ValidProtocol {
    struct Request {
        std::string data;
    };

    using ChainType = MockChain;

    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop&, TestSink&, const std::string&, const std::string& /*dataset*/ = {}
    ) {
        return std::make_shared<ChainType>();
    }

    static void SendRequest(std::shared_ptr<ChainType>&, const Request&) {}

    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) chain->Close();
    }
};

TEST(ProtocolDriverTest, ValidProtocolSatisfiesConcept) {
    static_assert(ProtocolDriver<ValidProtocol, MockRecord>);
    SUCCEED();
}

// Invalid protocol missing BuildChain
struct InvalidProtocol {
    struct Request {};
    using ChainType = MockChain;
    // Missing BuildChain and other methods
};

TEST(ProtocolDriverTest, InvalidProtocolDoesNotSatisfyConcept) {
    static_assert(!ProtocolDriver<InvalidProtocol, MockRecord>);
    SUCCEED();
}
