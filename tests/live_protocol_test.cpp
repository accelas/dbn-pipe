// tests/live_protocol_test.cpp
#include <gtest/gtest.h>

#include "src/live_protocol.hpp"
#include "src/protocol_driver.hpp"
#include "src/pipeline_base.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

// Forward declare the record type used by live protocol
struct DbnRecord {};
using LiveSink = Sink<DbnRecord>;

TEST(LiveProtocolTest, SatisfiesProtocolDriverConcept) {
    static_assert(ProtocolDriver<LiveProtocol, DbnRecord>);
    SUCCEED();
}

TEST(LiveProtocolTest, OnConnectReturnsTrue) {
    // Live protocol is ready immediately on connect
    std::shared_ptr<LiveProtocol::ChainType> chain;
    EXPECT_TRUE(LiveProtocol::OnConnect(chain));
}

TEST(LiveProtocolTest, OnReadReturnsTrue) {
    // Live protocol is always ready after connect
    std::shared_ptr<LiveProtocol::ChainType> chain;
    std::pmr::vector<std::byte> data;
    EXPECT_TRUE(LiveProtocol::OnRead(chain, std::move(data)));
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
