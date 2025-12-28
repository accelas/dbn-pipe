#include <gtest/gtest.h>

#include <cstring>

#include <databento/constants.hpp>
#include <databento/enums.hpp>
#include <databento/record.hpp>

#include "src/parser.hpp"

using namespace databento;
using namespace databento_async;

TEST(DbnParserTest, EmptyBuffer) {
    DbnParser parser;
    EXPECT_EQ(parser.Pull(), nullptr);
    EXPECT_FALSE(parser.HasRecord());
    EXPECT_EQ(parser.Available(), 0);
}

TEST(DbnParserTest, PartialHeader) {
    DbnParser parser;

    // Push less than a full header
    std::byte data[8] = {};
    parser.Push(data, sizeof(data));

    EXPECT_EQ(parser.Pull(), nullptr);
    EXPECT_FALSE(parser.HasRecord());
    EXPECT_EQ(parser.Available(), 8);
}

TEST(DbnParserTest, ParseMboMsg) {
    DbnParser parser;

    // Create a valid MboMsg in a buffer
    alignas(8) std::byte buffer[sizeof(MboMsg)] = {};
    auto* msg = reinterpret_cast<MboMsg*>(buffer);

    // Set up the header
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;
    msg->hd.publisher_id = 1;
    msg->hd.instrument_id = 12345;

    // Set some fields
    msg->order_id = 999;
    msg->price = 100 * kFixedPriceScale;
    msg->size = 10;
    msg->side = Side::Bid;
    msg->action = Action::Add;

    // Push to parser
    parser.Push(buffer, sizeof(buffer));

    EXPECT_TRUE(parser.HasRecord());
    EXPECT_EQ(parser.PeekSize(), sizeof(MboMsg));

    const Record* rec = parser.Pull();
    ASSERT_NE(rec, nullptr);

    EXPECT_EQ(rec->RType(), RType::Mbo);
    EXPECT_TRUE(rec->Holds<MboMsg>());

    const MboMsg& parsed = rec->Get<MboMsg>();
    EXPECT_EQ(parsed.order_id, 999);
    EXPECT_EQ(parsed.price, 100 * kFixedPriceScale);
    EXPECT_EQ(parsed.size, 10);
    EXPECT_EQ(parsed.side, Side::Bid);
    EXPECT_EQ(parsed.action, Action::Add);
}

TEST(DbnParserTest, StreamingPush) {
    DbnParser parser;

    // Create a record
    alignas(8) std::byte buffer[sizeof(MboMsg)] = {};
    auto* msg = reinterpret_cast<MboMsg*>(buffer);
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;
    msg->order_id = 42;

    // Push in chunks (simulating network reads)
    parser.Push(buffer, 10);           // Partial header
    EXPECT_EQ(parser.Pull(), nullptr);

    parser.Push(buffer + 10, 10);      // Rest of header, partial body
    EXPECT_EQ(parser.Pull(), nullptr);

    parser.Push(buffer + 20, sizeof(MboMsg) - 20);  // Rest of record

    const Record* rec = parser.Pull();
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->Get<MboMsg>().order_id, 42);
}
