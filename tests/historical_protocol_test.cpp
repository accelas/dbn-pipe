// tests/historical_protocol_test.cpp
#include <gtest/gtest.h>
#include <sstream>

#include "src/historical_protocol.hpp"
#include "src/protocol_driver.hpp"
#include "src/pipeline_sink.hpp"
#include "src/record_batch.hpp"

using namespace dbn_pipe;

using HistoricalSink = Sink<RecordRef>;

TEST(HistoricalProtocolTest, SatisfiesProtocolDriverConcept) {
    static_assert(ProtocolDriver<HistoricalProtocol, RecordRef>);
    SUCCEED();
}

TEST(HistoricalProtocolTest, RequestHasTimeRange) {
    HistoricalProtocol::Request req;
    req.dataset = "GLBX.MDP3";
    req.symbols = "ESZ4";
    req.schema = "mbp-1";
    req.start = 1704067200000000000ULL;  // 2024-01-01
    req.end = 1704153600000000000ULL;    // 2024-01-02

    EXPECT_EQ(req.dataset, "GLBX.MDP3");
    EXPECT_GT(req.start, 0ULL);
    EXPECT_GT(req.end, req.start);
}

TEST(HistoricalProtocolTest, UrlEncodeHandlesSpecialChars) {
    // Test URL encoding helper
    std::string input = "ES Z4+test";
    std::ostringstream out;
    HistoricalProtocol::UrlEncode(out, input);
    EXPECT_EQ(out.str(), "ES%20Z4%2Btest");
}
