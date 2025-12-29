#include <gtest/gtest.h>

#include <cstring>

#include <databento/constants.hpp>
#include <databento/enums.hpp>
#include <databento/record.hpp>

#include "src/record_batch.hpp"

using namespace databento;
using namespace databento_async;

TEST(RecordBatchTest, EmptyBatch) {
    RecordBatch batch;
    EXPECT_EQ(batch.size(), 0);
    EXPECT_TRUE(batch.empty());
}

TEST(RecordBatchTest, SingleRecord) {
    // Create a valid MboMsg
    alignas(8) std::byte record_buffer[sizeof(MboMsg)] = {};
    auto* msg = reinterpret_cast<MboMsg*>(record_buffer);
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;
    msg->hd.publisher_id = 1;
    msg->hd.instrument_id = 12345;
    msg->order_id = 999;
    msg->price = 100 * kFixedPriceScale;
    msg->size = 10;
    msg->side = Side::Bid;
    msg->action = Action::Add;

    // Build RecordBatch
    RecordBatch batch;
    batch.buffer.resize(sizeof(MboMsg));
    std::memcpy(batch.buffer.data(), record_buffer, sizeof(MboMsg));
    batch.offsets.push_back(0);

    EXPECT_EQ(batch.size(), 1);
    EXPECT_FALSE(batch.empty());

    // Test GetHeader
    RecordHeader header = batch.GetHeader(0);
    EXPECT_EQ(header.rtype, RType::Mbo);
    EXPECT_EQ(header.publisher_id, 1);
    EXPECT_EQ(header.instrument_id, 12345);

    // Test GetRecordData
    const std::byte* data = batch.GetRecordData(0);
    EXPECT_EQ(data, batch.buffer.data());

    // Verify we can copy out the full record
    MboMsg copied;
    std::memcpy(&copied, data, sizeof(MboMsg));
    EXPECT_EQ(copied.order_id, 999);
    EXPECT_EQ(copied.price, 100 * kFixedPriceScale);
    EXPECT_EQ(copied.size, 10);
    EXPECT_EQ(copied.side, Side::Bid);
    EXPECT_EQ(copied.action, Action::Add);
}

TEST(RecordBatchTest, MultipleRecords) {
    // Create two MboMsg records
    alignas(8) std::byte msg1_buffer[sizeof(MboMsg)] = {};
    alignas(8) std::byte msg2_buffer[sizeof(MboMsg)] = {};

    auto* msg1 = reinterpret_cast<MboMsg*>(msg1_buffer);
    msg1->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg1->hd.rtype = RType::Mbo;
    msg1->hd.instrument_id = 111;
    msg1->order_id = 1001;

    auto* msg2 = reinterpret_cast<MboMsg*>(msg2_buffer);
    msg2->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg2->hd.rtype = RType::Mbo;
    msg2->hd.instrument_id = 222;
    msg2->order_id = 2002;

    // Build RecordBatch with both records
    RecordBatch batch;
    batch.buffer.resize(2 * sizeof(MboMsg));
    std::memcpy(batch.buffer.data(), msg1_buffer, sizeof(MboMsg));
    std::memcpy(batch.buffer.data() + sizeof(MboMsg), msg2_buffer, sizeof(MboMsg));
    batch.offsets.push_back(0);
    batch.offsets.push_back(sizeof(MboMsg));

    EXPECT_EQ(batch.size(), 2);
    EXPECT_FALSE(batch.empty());

    // Verify first record
    RecordHeader hdr1 = batch.GetHeader(0);
    EXPECT_EQ(hdr1.instrument_id, 111);

    MboMsg copied1;
    std::memcpy(&copied1, batch.GetRecordData(0), sizeof(MboMsg));
    EXPECT_EQ(copied1.order_id, 1001);

    // Verify second record
    RecordHeader hdr2 = batch.GetHeader(1);
    EXPECT_EQ(hdr2.instrument_id, 222);

    MboMsg copied2;
    std::memcpy(&copied2, batch.GetRecordData(1), sizeof(MboMsg));
    EXPECT_EQ(copied2.order_id, 2002);
}

TEST(RecordBatchTest, UnalignedBuffer) {
    // Test that RecordBatch works correctly even when buffer is not aligned
    // This is the key use case - avoiding alignment UB

    alignas(8) std::byte msg_buffer[sizeof(MboMsg)] = {};
    auto* msg = reinterpret_cast<MboMsg*>(msg_buffer);
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;
    msg->hd.instrument_id = 42;
    msg->order_id = 12345;
    msg->price = 50 * kFixedPriceScale;

    // Create batch with 1-byte offset prefix to simulate unaligned data
    RecordBatch batch;
    batch.buffer.resize(1 + sizeof(MboMsg));  // 1 byte padding + record
    batch.buffer[0] = std::byte{0xFF};        // Padding byte
    std::memcpy(batch.buffer.data() + 1, msg_buffer, sizeof(MboMsg));
    batch.offsets.push_back(1);  // Record starts at offset 1 (unaligned)

    EXPECT_EQ(batch.size(), 1);

    // GetHeader should work via memcpy even with unaligned source
    RecordHeader header = batch.GetHeader(0);
    EXPECT_EQ(header.rtype, RType::Mbo);
    EXPECT_EQ(header.instrument_id, 42);

    // GetRecordData returns unaligned pointer - caller must use memcpy
    const std::byte* data = batch.GetRecordData(0);
    EXPECT_EQ(data, batch.buffer.data() + 1);

    // Safe access via memcpy
    MboMsg copied;
    std::memcpy(&copied, data, sizeof(MboMsg));
    EXPECT_EQ(copied.order_id, 12345);
    EXPECT_EQ(copied.price, 50 * kFixedPriceScale);
}

TEST(RecordBatchTest, DifferentRecordTypes) {
    // Test batch with different record types (MboMsg and TradeMsg)
    alignas(8) std::byte mbo_buffer[sizeof(MboMsg)] = {};
    alignas(8) std::byte trade_buffer[sizeof(TradeMsg)] = {};

    auto* mbo = reinterpret_cast<MboMsg*>(mbo_buffer);
    mbo->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    mbo->hd.rtype = RType::Mbo;
    mbo->hd.instrument_id = 100;
    mbo->order_id = 5555;

    auto* trade = reinterpret_cast<TradeMsg*>(trade_buffer);
    trade->hd.length = sizeof(TradeMsg) / kRecordHeaderLengthMultiplier;
    trade->hd.rtype = RType::Mbp0;  // TradeMsg uses Mbp0 rtype
    trade->hd.instrument_id = 200;
    trade->price = 75 * kFixedPriceScale;
    trade->size = 100;

    // Build batch
    RecordBatch batch;
    batch.buffer.resize(sizeof(MboMsg) + sizeof(TradeMsg));
    std::memcpy(batch.buffer.data(), mbo_buffer, sizeof(MboMsg));
    std::memcpy(batch.buffer.data() + sizeof(MboMsg), trade_buffer, sizeof(TradeMsg));
    batch.offsets.push_back(0);
    batch.offsets.push_back(sizeof(MboMsg));

    EXPECT_EQ(batch.size(), 2);

    // Check first record (MboMsg)
    RecordHeader hdr1 = batch.GetHeader(0);
    EXPECT_EQ(hdr1.rtype, RType::Mbo);
    EXPECT_EQ(hdr1.instrument_id, 100);

    MboMsg mbo_copy;
    std::memcpy(&mbo_copy, batch.GetRecordData(0), sizeof(MboMsg));
    EXPECT_EQ(mbo_copy.order_id, 5555);

    // Check second record (TradeMsg)
    RecordHeader hdr2 = batch.GetHeader(1);
    EXPECT_EQ(hdr2.rtype, RType::Mbp0);
    EXPECT_EQ(hdr2.instrument_id, 200);

    TradeMsg trade_copy;
    std::memcpy(&trade_copy, batch.GetRecordData(1), sizeof(TradeMsg));
    EXPECT_EQ(trade_copy.price, 75 * kFixedPriceScale);
    EXPECT_EQ(trade_copy.size, 100);
}

TEST(RecordBatchTest, GetRecordDataReturnsCorrectPointer) {
    // Verify GetRecordData returns exactly buffer.data() + offset
    RecordBatch batch;
    batch.buffer.resize(100);
    batch.offsets.push_back(0);
    batch.offsets.push_back(30);
    batch.offsets.push_back(70);

    EXPECT_EQ(batch.GetRecordData(0), batch.buffer.data());
    EXPECT_EQ(batch.GetRecordData(1), batch.buffer.data() + 30);
    EXPECT_EQ(batch.GetRecordData(2), batch.buffer.data() + 70);
}
