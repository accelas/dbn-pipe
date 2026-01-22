// tests/live_protocol_test.cpp
#include <gtest/gtest.h>

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/protocol.hpp"
#include "src/live_protocol.hpp"

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
