// tests/dbn_parser_component_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include <databento/constants.hpp>
#include <databento/record.hpp>

#include "src/dbn_parser_component.hpp"
#include "src/pipeline.hpp"
#include "src/record_batch.hpp"

// Mock sink that receives RecordBatch
struct MockSink {
    std::vector<databento_async::RecordBatch> batches;
    databento_async::Error last_error;
    bool complete_called = false;
    bool error_called = false;

    void OnData(databento_async::RecordBatch&& batch) {
        batches.push_back(std::move(batch));
    }
    void OnError(const databento_async::Error& e) {
        last_error = e;
        error_called = true;
    }
    void OnComplete() { complete_called = true; }
};

// Verify MockSink satisfies RecordSink concept
static_assert(databento_async::RecordSink<MockSink>);

// Helper to create a minimal valid MboMsg record
alignas(8) static std::byte g_record_buffer[sizeof(databento::MboMsg)] = {};

databento::MboMsg* CreateMinimalRecord(uint32_t instrument_id = 100) {
    std::memset(g_record_buffer, 0, sizeof(g_record_buffer));
    auto* msg = reinterpret_cast<databento::MboMsg*>(g_record_buffer);
    msg->hd.length = sizeof(databento::MboMsg) / databento::kRecordHeaderLengthMultiplier;
    msg->hd.rtype = databento::RType::Mbo;
    msg->hd.publisher_id = 1;
    msg->hd.instrument_id = instrument_id;
    return msg;
}

// Helper to create a vector<byte> from MboMsg
std::vector<std::byte> MsgToBytes(const databento::MboMsg* msg) {
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    return std::vector<std::byte>(bytes, bytes + sizeof(databento::MboMsg));
}

TEST(DbnParserComponentTest, Construction) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);
    // Just verify construction works
    SUCCEED();
}

TEST(DbnParserComponentTest, ParsesSingleRecord) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    auto* msg = CreateMinimalRecord(100);
    auto data = MsgToBytes(msg);

    parser.OnData(std::move(data));

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    // Verify record data
    auto header = sink.batches[0].GetHeader(0);
    EXPECT_EQ(header.instrument_id, 100);
    EXPECT_EQ(header.rtype, databento::RType::Mbo);
}

TEST(DbnParserComponentTest, ParsesMultipleRecordsInSingleBuffer) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Create two records
    alignas(8) std::byte buffer1[sizeof(databento::MboMsg)] = {};
    alignas(8) std::byte buffer2[sizeof(databento::MboMsg)] = {};

    auto* msg1 = reinterpret_cast<databento::MboMsg*>(buffer1);
    msg1->hd.length = sizeof(databento::MboMsg) / databento::kRecordHeaderLengthMultiplier;
    msg1->hd.rtype = databento::RType::Mbo;
    msg1->hd.publisher_id = 1;
    msg1->hd.instrument_id = 100;

    auto* msg2 = reinterpret_cast<databento::MboMsg*>(buffer2);
    msg2->hd.length = sizeof(databento::MboMsg) / databento::kRecordHeaderLengthMultiplier;
    msg2->hd.rtype = databento::RType::Mbo;
    msg2->hd.publisher_id = 1;
    msg2->hd.instrument_id = 200;

    std::vector<std::byte> data;
    data.insert(data.end(), buffer1, buffer1 + sizeof(databento::MboMsg));
    data.insert(data.end(), buffer2, buffer2 + sizeof(databento::MboMsg));

    parser.OnData(std::move(data));

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 2);

    auto hdr1 = sink.batches[0].GetHeader(0);
    EXPECT_EQ(hdr1.instrument_id, 100);

    auto hdr2 = sink.batches[0].GetHeader(1);
    EXPECT_EQ(hdr2.instrument_id, 200);
}

TEST(DbnParserComponentTest, HandlesCarryoverAcrossCalls) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);

    // Send record in two chunks
    size_t half = sizeof(databento::MboMsg) / 2;

    std::vector<std::byte> chunk1(bytes, bytes + half);
    parser.OnData(std::move(chunk1));

    // No batch yet (incomplete record)
    EXPECT_EQ(sink.batches.size(), 0);

    std::vector<std::byte> chunk2(bytes + half, bytes + sizeof(databento::MboMsg));
    parser.OnData(std::move(chunk2));

    // Now we should have the record
    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    auto header = sink.batches[0].GetHeader(0);
    EXPECT_EQ(header.instrument_id, 100);
}

TEST(DbnParserComponentTest, HandlesMultipleChunksWithCarryover) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Create two records
    alignas(8) std::byte buffer1[sizeof(databento::MboMsg)] = {};
    alignas(8) std::byte buffer2[sizeof(databento::MboMsg)] = {};

    auto* msg1 = reinterpret_cast<databento::MboMsg*>(buffer1);
    msg1->hd.length = sizeof(databento::MboMsg) / databento::kRecordHeaderLengthMultiplier;
    msg1->hd.rtype = databento::RType::Mbo;
    msg1->hd.instrument_id = 100;

    auto* msg2 = reinterpret_cast<databento::MboMsg*>(buffer2);
    msg2->hd.length = sizeof(databento::MboMsg) / databento::kRecordHeaderLengthMultiplier;
    msg2->hd.rtype = databento::RType::Mbo;
    msg2->hd.instrument_id = 200;

    // Combine and send with split in middle of second record
    std::vector<std::byte> combined;
    combined.insert(combined.end(), buffer1, buffer1 + sizeof(databento::MboMsg));
    combined.insert(combined.end(), buffer2, buffer2 + sizeof(databento::MboMsg));

    size_t split = sizeof(databento::MboMsg) + sizeof(databento::MboMsg) / 2;

    std::vector<std::byte> chunk1(combined.begin(), combined.begin() + split);
    parser.OnData(std::move(chunk1));

    // First record should be delivered
    ASSERT_EQ(sink.batches.size(), 1);
    EXPECT_EQ(sink.batches[0].size(), 1);

    std::vector<std::byte> chunk2(combined.begin() + split, combined.end());
    parser.OnData(std::move(chunk2));

    // Second record should now be delivered
    ASSERT_EQ(sink.batches.size(), 2);
    EXPECT_EQ(sink.batches[1].size(), 1);

    auto hdr1 = sink.batches[0].GetHeader(0);
    EXPECT_EQ(hdr1.instrument_id, 100);

    auto hdr2 = sink.batches[1].GetHeader(0);
    EXPECT_EQ(hdr2.instrument_id, 200);
}

TEST(DbnParserComponentTest, ForwardsOnComplete) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    parser.OnComplete();

    EXPECT_TRUE(sink.complete_called);
    EXPECT_FALSE(sink.error_called);
}

TEST(DbnParserComponentTest, ForwardsOnError) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    databento_async::Error err{databento_async::ErrorCode::ConnectionFailed, "test error"};
    parser.OnError(err);

    EXPECT_TRUE(sink.error_called);
    EXPECT_EQ(sink.last_error.code, databento_async::ErrorCode::ConnectionFailed);
    EXPECT_EQ(sink.last_error.message, "test error");
}

TEST(DbnParserComponentTest, InvalidRecordSizeEmitsError) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Create a header with size smaller than header (invalid)
    alignas(8) std::byte buffer[sizeof(databento::RecordHeader)] = {};
    auto* hdr = reinterpret_cast<databento::RecordHeader*>(buffer);
    // length=1 means size = 1 * 4 = 4 bytes, which is less than sizeof(RecordHeader)
    hdr->length = 1;
    hdr->rtype = databento::RType::Mbo;
    hdr->publisher_id = 1;
    hdr->instrument_id = 100;

    std::vector<std::byte> data(buffer, buffer + sizeof(buffer));
    parser.OnData(std::move(data));

    EXPECT_TRUE(sink.error_called);
    EXPECT_EQ(sink.last_error.code, databento_async::ErrorCode::ParseError);
    EXPECT_TRUE(sink.last_error.message.find("smaller than header") != std::string::npos);
}

TEST(DbnParserComponentTest, MaxRecordSizeIsReasonable) {
    // The RecordHeader.length field is uint8_t, so max record size via header is 255 * 4 = 1020 bytes.
    // kMaxRecordSize at 64KB provides headroom for future format changes and ensures
    // the validation is in place even though current DBN format can't exceed it.
    //
    // Note: Since length is uint8_t, we can't actually create a header that
    // exceeds kMaxRecordSize. This test verifies the constant is reasonable.

    // Verify kMaxRecordSize is larger than any valid DBN record
    constexpr size_t max_dbn_record = 255 * databento::kRecordHeaderLengthMultiplier;
    EXPECT_GT(databento_async::kMaxRecordSize, max_dbn_record);

    // Verify constant has the expected value (64KB)
    EXPECT_EQ(databento_async::kMaxRecordSize, 64 * 1024);
}

TEST(DbnParserComponentTest, IncompleteRecordAtEndEmitsError) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Send partial header
    std::vector<std::byte> partial;
    partial.push_back(std::byte{0x01});
    partial.push_back(std::byte{0x02});
    parser.OnData(std::move(partial));

    // Signal end of stream with incomplete data
    parser.OnComplete();

    EXPECT_TRUE(sink.error_called);
    EXPECT_EQ(sink.last_error.code, databento_async::ErrorCode::ParseError);
    EXPECT_TRUE(sink.last_error.message.find("Incomplete record") != std::string::npos);
    EXPECT_FALSE(sink.complete_called);
}

TEST(DbnParserComponentTest, OneShotErrorGuard) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    databento_async::Error err1{databento_async::ErrorCode::ConnectionFailed, "first error"};
    databento_async::Error err2{databento_async::ErrorCode::ParseError, "second error"};

    parser.OnError(err1);
    parser.OnError(err2);

    // Only first error should be forwarded
    EXPECT_TRUE(sink.error_called);
    EXPECT_EQ(sink.last_error.code, databento_async::ErrorCode::ConnectionFailed);
    EXPECT_EQ(sink.last_error.message, "first error");
}

TEST(DbnParserComponentTest, IgnoresDataAfterError) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Trigger error
    databento_async::Error err{databento_async::ErrorCode::ConnectionFailed, "error"};
    parser.OnError(err);

    // Send more data - should be ignored
    auto* msg = CreateMinimalRecord(100);
    auto data = MsgToBytes(msg);
    parser.OnData(std::move(data));

    // No batches should be delivered
    EXPECT_EQ(sink.batches.size(), 0);
}

TEST(DbnParserComponentTest, IgnoresCompleteAfterError) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Trigger error
    databento_async::Error err{databento_async::ErrorCode::ConnectionFailed, "error"};
    parser.OnError(err);

    // Send complete - should be ignored
    parser.OnComplete();

    EXPECT_TRUE(sink.error_called);
    EXPECT_FALSE(sink.complete_called);
}

TEST(DbnParserComponentTest, EmptyBufferDoesNothing) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    std::vector<std::byte> empty;
    parser.OnData(std::move(empty));

    EXPECT_EQ(sink.batches.size(), 0);
    EXPECT_FALSE(sink.error_called);

    // OnComplete should work fine
    parser.OnComplete();
    EXPECT_TRUE(sink.complete_called);
}

TEST(DbnParserComponentTest, SkipsDbMetadataHeader) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Build DBN metadata header
    struct DbnHeader {
        char magic[3];
        std::uint8_t version;
        std::uint32_t frame_size;
    } __attribute__((packed));

    DbnHeader header;
    header.magic[0] = 'D';
    header.magic[1] = 'B';
    header.magic[2] = 'N';
    header.version = 2;
    header.frame_size = 16;  // 16 bytes of metadata content

    auto* msg = CreateMinimalRecord(100);

    // Combine: header + metadata content + record
    std::vector<std::byte> data;
    auto* header_bytes = reinterpret_cast<const std::byte*>(&header);
    data.insert(data.end(), header_bytes, header_bytes + sizeof(header));
    data.insert(data.end(), 16, std::byte{0x00});  // Metadata content
    auto* msg_bytes = reinterpret_cast<const std::byte*>(msg);
    data.insert(data.end(), msg_bytes, msg_bytes + sizeof(databento::MboMsg));

    parser.OnData(std::move(data));

    // Should have parsed the record after metadata
    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    auto hdr = sink.batches[0].GetHeader(0);
    EXPECT_EQ(hdr.instrument_id, 100);
}

TEST(DbnParserComponentTest, HandlesChunkedMetadata) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Build DBN metadata header
    struct DbnHeader {
        char magic[3];
        std::uint8_t version;
        std::uint32_t frame_size;
    } __attribute__((packed));

    DbnHeader header;
    header.magic[0] = 'D';
    header.magic[1] = 'B';
    header.magic[2] = 'N';
    header.version = 2;
    header.frame_size = 16;

    auto* msg = CreateMinimalRecord(100);

    // Combine everything
    std::vector<std::byte> data;
    auto* header_bytes = reinterpret_cast<const std::byte*>(&header);
    data.insert(data.end(), header_bytes, header_bytes + sizeof(header));
    data.insert(data.end(), 16, std::byte{0x00});
    auto* msg_bytes = reinterpret_cast<const std::byte*>(msg);
    data.insert(data.end(), msg_bytes, msg_bytes + sizeof(databento::MboMsg));

    // Send in small chunks
    size_t chunk_size = 4;
    for (size_t i = 0; i < data.size(); i += chunk_size) {
        size_t end = std::min(i + chunk_size, data.size());
        std::vector<std::byte> chunk(data.begin() + i, data.begin() + end);
        parser.OnData(std::move(chunk));
    }

    // Should eventually get the record
    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    auto hdr = sink.batches[0].GetHeader(0);
    EXPECT_EQ(hdr.instrument_id, 100);
}

TEST(DbnParserComponentTest, NoMetadataParsesDirect) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Send record directly without DBN header (live streaming case)
    auto* msg = CreateMinimalRecord(100);
    auto data = MsgToBytes(msg);

    parser.OnData(std::move(data));

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    auto header = sink.batches[0].GetHeader(0);
    EXPECT_EQ(header.instrument_id, 100);
}

TEST(DbnParserComponentTest, RecordBatchContainsCorrectData) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Create a record with specific values
    alignas(8) std::byte buffer[sizeof(databento::MboMsg)] = {};
    auto* msg = reinterpret_cast<databento::MboMsg*>(buffer);
    msg->hd.length = sizeof(databento::MboMsg) / databento::kRecordHeaderLengthMultiplier;
    msg->hd.rtype = databento::RType::Mbo;
    msg->hd.publisher_id = 42;
    msg->hd.instrument_id = 12345;
    msg->order_id = 999;
    msg->price = 50 * databento::kFixedPriceScale;
    msg->size = 100;

    std::vector<std::byte> data(buffer, buffer + sizeof(databento::MboMsg));
    parser.OnData(std::move(data));

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    // Copy out full record and verify
    databento::MboMsg copied;
    std::memcpy(&copied, sink.batches[0].GetRecordData(0), sizeof(databento::MboMsg));
    EXPECT_EQ(copied.hd.publisher_id, 42);
    EXPECT_EQ(copied.hd.instrument_id, 12345);
    EXPECT_EQ(copied.order_id, 999);
    EXPECT_EQ(copied.price, 50 * databento::kFixedPriceScale);
    EXPECT_EQ(copied.size, 100);
}

// Note: kMaxRecordSize value verification is in MaxRecordSizeIsReasonable test
