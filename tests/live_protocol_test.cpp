// tests/live_protocol_test.cpp
#include <gtest/gtest.h>

#include "src/buffer_chain.hpp"
#include "src/live_protocol.hpp"
#include "src/pipeline_sink.hpp"
#include "src/protocol_driver.hpp"
#include "src/record_batch.hpp"

using namespace dbn_pipe;

using LiveSink = Sink<RecordRef>;

TEST(LiveProtocolTest, SatisfiesProtocolDriverConcept) {
    static_assert(ProtocolDriver<LiveProtocol, RecordRef>);
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
