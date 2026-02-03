// SPDX-License-Identifier: MIT

// tests/live_protocol_test.cpp
#include <gtest/gtest.h>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/protocol.hpp"
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
