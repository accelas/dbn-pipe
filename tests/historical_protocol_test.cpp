// SPDX-License-Identifier: MIT

// tests/historical_protocol_test.cpp
#include <gtest/gtest.h>
#include <iterator>
#include <string>

#include "dbn_pipe/stream/protocol.hpp"
#include "dbn_pipe/stream/url_encode.hpp"
#include "dbn_pipe/historical_protocol.hpp"
#include "dbn_pipe/record_batch.hpp"

using namespace dbn_pipe;

TEST(HistoricalProtocolTest, SatisfiesProtocolConcept) {
    static_assert(Protocol<HistoricalProtocol>);
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
    // Test URL encoding helper (now uses free function from api/url_encode.hpp)
    std::string input = "ES Z4+test";
    std::string out;
    UrlEncode(std::back_inserter(out), input);
    EXPECT_EQ(out, "ES%20Z4%2Btest");
}
