// SPDX-License-Identifier: MIT

// tests/dbn_parser_component_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include <databento/constants.hpp>
#include <databento/record.hpp>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/dbn_parser_component.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"
#include "dbn_pipe/record_batch.hpp"

// Mock sink that receives RecordBatch
struct MockSink {
    std::vector<dbn_pipe::RecordBatch> batches;
    dbn_pipe::Error last_error;
    bool complete_called = false;
    bool error_called = false;

    void OnData(dbn_pipe::RecordBatch&& batch) {
        batches.push_back(std::move(batch));
    }
    void OnError(const dbn_pipe::Error& e) {
        last_error = e;
        error_called = true;
    }
    void OnComplete() { complete_called = true; }
};

// Verify MockSink satisfies RecordSink concept
static_assert(dbn_pipe::RecordSink<MockSink>);

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

// Helper to create a segment containing data from a record
std::shared_ptr<dbn_pipe::Segment> MakeSegmentFromRecord(
    const databento::MboMsg* msg) {
    auto seg = std::make_shared<dbn_pipe::Segment>();
    std::memcpy(seg->data.data(), msg, sizeof(databento::MboMsg));
    seg->size = sizeof(databento::MboMsg);
    return seg;
}

// Helper to create a segment from raw bytes
std::shared_ptr<dbn_pipe::Segment> MakeSegmentFromBytes(
    const std::byte* data, size_t len) {
    auto seg = std::make_shared<dbn_pipe::Segment>();
    std::memcpy(seg->data.data(), data, len);
    seg->size = len;
    return seg;
}

// Helper to create a BufferChain with a single record
dbn_pipe::BufferChain MakeChainWithRecord(const databento::MboMsg* msg) {
    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromRecord(msg));
    return chain;
}

TEST(DbnParserComponentTest, Construction) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);
    // Just verify construction works
    SUCCEED();
}

TEST(DbnParserComponentTest, ParsesSingleRecord) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    auto* msg = CreateMinimalRecord(100);
    auto chain = MakeChainWithRecord(msg);

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    // Verify record data using RecordRef API
    const auto& ref = sink.batches[0][0];
    EXPECT_EQ(ref.Header().instrument_id, 100);
    EXPECT_EQ(ref.Header().rtype, databento::RType::Mbo);

    // Verify chain was consumed
    EXPECT_TRUE(chain.Empty());
}

TEST(DbnParserComponentTest, ParsesMultipleRecordsInSingleSegment) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Create segment with two records
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

    auto seg = std::make_shared<dbn_pipe::Segment>();
    std::memcpy(seg->data.data(), buffer1, sizeof(databento::MboMsg));
    std::memcpy(seg->data.data() + sizeof(databento::MboMsg), buffer2, sizeof(databento::MboMsg));
    seg->size = 2 * sizeof(databento::MboMsg);

    dbn_pipe::BufferChain chain;
    chain.Append(seg);

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 2);

    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
    EXPECT_EQ(sink.batches[0][1].Header().instrument_id, 200);
    EXPECT_TRUE(chain.Empty());
}

TEST(DbnParserComponentTest, HandlesPartialRecordAcrossCalls) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t half = sizeof(databento::MboMsg) / 2;

    // First call: partial record
    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(bytes, half));

    parser.OnData(chain);

    // No batch yet (incomplete record)
    EXPECT_EQ(sink.batches.size(), 0);
    // Partial data should remain in chain
    EXPECT_EQ(chain.Size(), half);

    // Second call: rest of record
    chain.Append(MakeSegmentFromBytes(bytes + half, sizeof(databento::MboMsg) - half));

    parser.OnData(chain);

    // Now we should have the record
    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
    EXPECT_TRUE(chain.Empty());
}

TEST(DbnParserComponentTest, HandlesMultipleCallsWithPartialRecords) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

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

    // First call: first record complete + half of second
    size_t half = sizeof(databento::MboMsg) / 2;
    auto seg1 = std::make_shared<dbn_pipe::Segment>();
    std::memcpy(seg1->data.data(), buffer1, sizeof(databento::MboMsg));
    std::memcpy(seg1->data.data() + sizeof(databento::MboMsg), buffer2, half);
    seg1->size = sizeof(databento::MboMsg) + half;

    dbn_pipe::BufferChain chain;
    chain.Append(seg1);

    parser.OnData(chain);

    // First record should be delivered
    ASSERT_EQ(sink.batches.size(), 1);
    EXPECT_EQ(sink.batches[0].size(), 1);
    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);

    // Partial second record remains
    EXPECT_EQ(chain.Size(), half);

    // Second call: rest of second record
    chain.Append(MakeSegmentFromBytes(
        reinterpret_cast<const std::byte*>(buffer2) + half,
        sizeof(databento::MboMsg) - half));

    parser.OnData(chain);

    // Second record should now be delivered
    ASSERT_EQ(sink.batches.size(), 2);
    EXPECT_EQ(sink.batches[1].size(), 1);
    EXPECT_EQ(sink.batches[1][0].Header().instrument_id, 200);
    EXPECT_TRUE(chain.Empty());
}

TEST(DbnParserComponentTest, ForwardsOnComplete) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    parser.OnComplete();

    EXPECT_TRUE(sink.complete_called);
    EXPECT_FALSE(sink.error_called);
}

TEST(DbnParserComponentTest, ForwardsOnError) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    dbn_pipe::Error err{dbn_pipe::ErrorCode::ConnectionFailed, "test error"};
    parser.OnError(err);

    EXPECT_TRUE(sink.error_called);
    EXPECT_EQ(sink.last_error.code, dbn_pipe::ErrorCode::ConnectionFailed);
    EXPECT_EQ(sink.last_error.message, "test error");
}

TEST(DbnParserComponentTest, InvalidRecordSizeEmitsError) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Create a header with size smaller than header (invalid)
    alignas(8) std::byte buffer[sizeof(databento::RecordHeader)] = {};
    auto* hdr = reinterpret_cast<databento::RecordHeader*>(buffer);
    // length=1 means size = 1 * 4 = 4 bytes, which is less than sizeof(RecordHeader)
    hdr->length = 1;
    hdr->rtype = databento::RType::Mbo;
    hdr->publisher_id = 1;
    hdr->instrument_id = 100;

    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(buffer, sizeof(buffer)));

    parser.OnData(chain);

    EXPECT_TRUE(sink.error_called);
    EXPECT_EQ(sink.last_error.code, dbn_pipe::ErrorCode::ParseError);
    EXPECT_TRUE(sink.last_error.message.find("smaller than header") != std::string::npos);
}

TEST(DbnParserComponentTest, MaxRecordSizeIsReasonable) {
    // The RecordHeader.length field is uint8_t, so max record size via header is 255 * 4 = 1020 bytes.
    // kMaxRecordSize at 64KB provides headroom for future format changes and ensures
    // the validation is in place even though current DBN format can't exceed it.

    // Verify kMaxRecordSize is larger than any valid DBN record
    constexpr size_t max_dbn_record = 255 * databento::kRecordHeaderLengthMultiplier;
    EXPECT_GT(dbn_pipe::kMaxRecordSize, max_dbn_record);

    // Verify constant has the expected value (64KB)
    EXPECT_EQ(dbn_pipe::kMaxRecordSize, 64 * 1024);
}

TEST(DbnParserComponentTest, IncompleteRecordAtEndEmitsError) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Add partial data to chain
    std::byte partial[] = {std::byte{0x01}, std::byte{0x02}};
    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(partial, sizeof(partial)));

    parser.OnData(chain);

    // Signal end of stream with incomplete data
    parser.OnComplete(chain);

    EXPECT_TRUE(sink.error_called);
    EXPECT_EQ(sink.last_error.code, dbn_pipe::ErrorCode::ParseError);
    EXPECT_TRUE(sink.last_error.message.find("Incomplete record") != std::string::npos);
    EXPECT_FALSE(sink.complete_called);
}

TEST(DbnParserComponentTest, OneShotErrorGuard) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    dbn_pipe::Error err1{dbn_pipe::ErrorCode::ConnectionFailed, "first error"};
    dbn_pipe::Error err2{dbn_pipe::ErrorCode::ParseError, "second error"};

    parser.OnError(err1);
    parser.OnError(err2);

    // Only first error should be forwarded
    EXPECT_TRUE(sink.error_called);
    EXPECT_EQ(sink.last_error.code, dbn_pipe::ErrorCode::ConnectionFailed);
    EXPECT_EQ(sink.last_error.message, "first error");
}

TEST(DbnParserComponentTest, IgnoresDataAfterError) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Trigger error
    dbn_pipe::Error err{dbn_pipe::ErrorCode::ConnectionFailed, "error"};
    parser.OnError(err);

    // Send more data - should be ignored
    auto* msg = CreateMinimalRecord(100);
    auto chain = MakeChainWithRecord(msg);
    parser.OnData(chain);

    // No batches should be delivered
    EXPECT_EQ(sink.batches.size(), 0);
}

TEST(DbnParserComponentTest, IgnoresCompleteAfterError) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Trigger error
    dbn_pipe::Error err{dbn_pipe::ErrorCode::ConnectionFailed, "error"};
    parser.OnError(err);

    // Send complete - should be ignored
    parser.OnComplete();

    EXPECT_TRUE(sink.error_called);
    EXPECT_FALSE(sink.complete_called);
}

TEST(DbnParserComponentTest, EmptyChainDoesNothing) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    dbn_pipe::BufferChain chain;
    parser.OnData(chain);

    EXPECT_EQ(sink.batches.size(), 0);
    EXPECT_FALSE(sink.error_called);

    // OnComplete should work fine with empty chain
    parser.OnComplete(chain);
    EXPECT_TRUE(sink.complete_called);
}

TEST(DbnParserComponentTest, SkipsDbMetadataHeader) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

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
    auto seg = std::make_shared<dbn_pipe::Segment>();
    size_t offset = 0;
    std::memcpy(seg->data.data() + offset, &header, sizeof(header));
    offset += sizeof(header);
    std::memset(seg->data.data() + offset, 0, 16);  // Metadata content
    offset += 16;
    std::memcpy(seg->data.data() + offset, msg, sizeof(databento::MboMsg));
    offset += sizeof(databento::MboMsg);
    seg->size = offset;

    dbn_pipe::BufferChain chain;
    chain.Append(seg);

    parser.OnData(chain);

    // Should have parsed the record after metadata
    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
    EXPECT_TRUE(chain.Empty());
}

TEST(DbnParserComponentTest, HandlesChunkedMetadata) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

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

    // Combine everything into a single buffer first
    std::vector<std::byte> all_data;
    auto* header_bytes = reinterpret_cast<const std::byte*>(&header);
    all_data.insert(all_data.end(), header_bytes, header_bytes + sizeof(header));
    all_data.insert(all_data.end(), 16, std::byte{0x00});
    auto* msg_bytes = reinterpret_cast<const std::byte*>(msg);
    all_data.insert(all_data.end(), msg_bytes, msg_bytes + sizeof(databento::MboMsg));

    // Send in small segments (4 bytes each)
    dbn_pipe::BufferChain chain;
    size_t chunk_size = 4;
    for (size_t i = 0; i < all_data.size(); i += chunk_size) {
        size_t len = std::min(chunk_size, all_data.size() - i);
        chain.Append(MakeSegmentFromBytes(all_data.data() + i, len));
        parser.OnData(chain);
    }

    // Should eventually get the record
    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
}

TEST(DbnParserComponentTest, NoMetadataParsesDirect) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Send record directly without DBN header (live streaming case)
    auto* msg = CreateMinimalRecord(100);
    auto chain = MakeChainWithRecord(msg);

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
}

TEST(DbnParserComponentTest, RecordRefContainsCorrectData) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

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

    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(buffer, sizeof(databento::MboMsg)));

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    // Use RecordRef::As<T>() for zero-copy typed access
    const auto& ref = sink.batches[0][0];
    const auto& record = ref.As<databento::MboMsg>();
    EXPECT_EQ(record.hd.publisher_id, 42);
    EXPECT_EQ(record.hd.instrument_id, 12345);
    EXPECT_EQ(record.order_id, 999);
    EXPECT_EQ(record.price, 50 * databento::kFixedPriceScale);
    EXPECT_EQ(record.size, 100);
}

// ============================================================================
// Boundary-crossing tests (records spanning segment boundaries)
// ============================================================================

TEST(DbnParserComponentTest, RecordSpanningTwoSegments) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Create a record and split it across two segments
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t split = sizeof(databento::MboMsg) / 2;

    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(bytes, split));
    chain.Append(MakeSegmentFromBytes(bytes + split, sizeof(databento::MboMsg) - split));

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    // Verify data is correct (should have been copied to scratch buffer)
    const auto& ref = sink.batches[0][0];
    EXPECT_EQ(ref.Header().instrument_id, 100);
    EXPECT_EQ(ref.size, sizeof(databento::MboMsg));

    // Verify keepalive is set (scratch buffer, not segment)
    EXPECT_NE(ref.keepalive, nullptr);
    EXPECT_TRUE(chain.Empty());
}

TEST(DbnParserComponentTest, HeaderSpanningTwoSegments) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Create a record and split it so header spans two segments
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);

    // Split in the middle of the header (header is 8 bytes)
    size_t split = sizeof(databento::RecordHeader) / 2;

    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(bytes, split));
    chain.Append(MakeSegmentFromBytes(bytes + split, sizeof(databento::MboMsg) - split));

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    const auto& ref = sink.batches[0][0];
    EXPECT_EQ(ref.Header().instrument_id, 100);
    EXPECT_TRUE(chain.Empty());
}

TEST(DbnParserComponentTest, RecordSpanningThreeSegments) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Create a record and split it across three segments
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t record_size = sizeof(databento::MboMsg);
    size_t third = record_size / 3;

    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(bytes, third));
    chain.Append(MakeSegmentFromBytes(bytes + third, third));
    chain.Append(MakeSegmentFromBytes(bytes + 2 * third, record_size - 2 * third));

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    const auto& ref = sink.batches[0][0];
    EXPECT_EQ(ref.Header().instrument_id, 100);
    EXPECT_TRUE(chain.Empty());
}

TEST(DbnParserComponentTest, MultipleRecordsSomeBoundaryCrossing) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Create three records
    alignas(8) std::byte rec1[sizeof(databento::MboMsg)] = {};
    alignas(8) std::byte rec2[sizeof(databento::MboMsg)] = {};
    alignas(8) std::byte rec3[sizeof(databento::MboMsg)] = {};

    auto* msg1 = reinterpret_cast<databento::MboMsg*>(rec1);
    msg1->hd.length = sizeof(databento::MboMsg) / databento::kRecordHeaderLengthMultiplier;
    msg1->hd.rtype = databento::RType::Mbo;
    msg1->hd.instrument_id = 100;

    auto* msg2 = reinterpret_cast<databento::MboMsg*>(rec2);
    msg2->hd.length = sizeof(databento::MboMsg) / databento::kRecordHeaderLengthMultiplier;
    msg2->hd.rtype = databento::RType::Mbo;
    msg2->hd.instrument_id = 200;

    auto* msg3 = reinterpret_cast<databento::MboMsg*>(rec3);
    msg3->hd.length = sizeof(databento::MboMsg) / databento::kRecordHeaderLengthMultiplier;
    msg3->hd.rtype = databento::RType::Mbo;
    msg3->hd.instrument_id = 300;

    size_t record_size = sizeof(databento::MboMsg);

    // Segment 1: record1 complete + first half of record2
    auto seg1 = std::make_shared<dbn_pipe::Segment>();
    std::memcpy(seg1->data.data(), rec1, record_size);
    std::memcpy(seg1->data.data() + record_size, rec2, record_size / 2);
    seg1->size = record_size + record_size / 2;

    // Segment 2: second half of record2 + record3 complete
    auto seg2 = std::make_shared<dbn_pipe::Segment>();
    std::memcpy(seg2->data.data(), rec2 + record_size / 2, record_size - record_size / 2);
    std::memcpy(seg2->data.data() + record_size - record_size / 2, rec3, record_size);
    seg2->size = record_size - record_size / 2 + record_size;

    dbn_pipe::BufferChain chain;
    chain.Append(seg1);
    chain.Append(seg2);

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 3);

    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
    EXPECT_EQ(sink.batches[0][1].Header().instrument_id, 200);
    EXPECT_EQ(sink.batches[0][2].Header().instrument_id, 300);
    EXPECT_TRUE(chain.Empty());
}

TEST(DbnParserComponentTest, ZeroCopyWhenContiguous) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Create a single record in one segment (contiguous case)
    auto* msg = CreateMinimalRecord(100);
    auto seg = MakeSegmentFromRecord(msg);
    const std::byte* original_ptr = seg->data.data();

    dbn_pipe::BufferChain chain;
    chain.Append(seg);

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    // Verify zero-copy: data pointer should point into the segment
    const auto& ref = sink.batches[0][0];
    EXPECT_EQ(ref.data, original_ptr);

    // Verify keepalive holds the segment
    EXPECT_NE(ref.keepalive, nullptr);
}

TEST(DbnParserComponentTest, ScratchBufferIsAligned) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Create a record spanning two segments to force scratch buffer allocation
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t split = 8;  // Split early to maximize data in second segment

    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(bytes, split));
    chain.Append(MakeSegmentFromBytes(bytes + split, sizeof(databento::MboMsg) - split));

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    // Verify alignment (8-byte aligned for DBN records)
    const auto& ref = sink.batches[0][0];
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ref.data) % 8, 0);
}

TEST(DbnParserComponentTest, KeepaliveKeepsDataValid) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    auto* msg = CreateMinimalRecord(100);
    auto chain = MakeChainWithRecord(msg);

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    // Take a copy of the RecordRef
    dbn_pipe::RecordRef ref = sink.batches[0][0];

    // Clear batches (but ref still holds keepalive)
    sink.batches.clear();

    // Data should still be valid because ref.keepalive holds the segment
    EXPECT_EQ(ref.Header().instrument_id, 100);
}

TEST(DbnParserComponentTest, BoundaryCrossingKeepaliveKeepsDataValid) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Create boundary-crossing record
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t split = sizeof(databento::MboMsg) / 2;

    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(bytes, split));
    chain.Append(MakeSegmentFromBytes(bytes + split, sizeof(databento::MboMsg) - split));

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);

    // Take a copy of the RecordRef
    dbn_pipe::RecordRef ref = sink.batches[0][0];

    // Clear batches
    sink.batches.clear();

    // Data should still be valid because ref.keepalive holds the scratch buffer
    EXPECT_EQ(ref.Header().instrument_id, 100);
}

TEST(DbnParserComponentTest, MetadataSpanningSegments) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

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

    // Split metadata header across two segments
    auto* header_bytes = reinterpret_cast<const std::byte*>(&header);

    dbn_pipe::BufferChain chain;

    // First segment: half of DBN header
    chain.Append(MakeSegmentFromBytes(header_bytes, 4));

    // Second segment: rest of header + metadata content + record
    auto seg2 = std::make_shared<dbn_pipe::Segment>();
    size_t offset = 0;
    std::memcpy(seg2->data.data() + offset, header_bytes + 4, 4);
    offset += 4;
    std::memset(seg2->data.data() + offset, 0, 16);  // Metadata content
    offset += 16;
    std::memcpy(seg2->data.data() + offset, msg, sizeof(databento::MboMsg));
    offset += sizeof(databento::MboMsg);
    seg2->size = offset;
    chain.Append(seg2);

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);
    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
}

// ============================================================================
// SegmentAllocator integration tests
// ============================================================================

// Tracking memory resource that counts allocations
class TrackingMemoryResource : public std::pmr::memory_resource {
public:
    std::atomic<size_t> allocate_count{0};
    std::atomic<size_t> deallocate_count{0};
    std::atomic<size_t> total_bytes_allocated{0};

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        allocate_count.fetch_add(1, std::memory_order_relaxed);
        total_bytes_allocated.fetch_add(bytes, std::memory_order_relaxed);
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        deallocate_count.fetch_add(1, std::memory_order_relaxed);
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

TEST(DbnParserComponentTest, ScratchBufferUsesInjectedAllocator) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // Create a tracking allocator
    auto tracking_resource = std::make_shared<TrackingMemoryResource>();
    dbn_pipe::SegmentAllocator allocator(tracking_resource);
    parser.SetAllocator(&allocator);

    // Create a record spanning two segments to force scratch buffer allocation
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t split = sizeof(databento::MboMsg) / 2;

    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(bytes, split));
    chain.Append(MakeSegmentFromBytes(bytes + split, sizeof(databento::MboMsg) - split));

    parser.OnData(chain);

    // Verify record was parsed correctly
    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);
    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);

    // Verify the tracking resource was used for scratch allocation
    EXPECT_GE(tracking_resource->allocate_count.load(), 1);
    EXPECT_GE(tracking_resource->total_bytes_allocated.load(), sizeof(databento::MboMsg));
}

TEST(DbnParserComponentTest, ScratchBufferDeallocatesViaAllocator) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    auto tracking_resource = std::make_shared<TrackingMemoryResource>();
    dbn_pipe::SegmentAllocator allocator(tracking_resource);
    parser.SetAllocator(&allocator);

    // Force scratch path with boundary-crossing record
    auto* msg = CreateMinimalRecord(200);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t split = sizeof(databento::MboMsg) / 2;

    {
        dbn_pipe::BufferChain chain;
        chain.Append(MakeSegmentFromBytes(bytes, split));
        chain.Append(MakeSegmentFromBytes(bytes + split, sizeof(databento::MboMsg) - split));

        parser.OnData(chain);

        ASSERT_EQ(sink.batches.size(), 1);

        // Clear batches - this drops the keepalive shared_ptr, triggering deallocation
        sink.batches.clear();
    }

    // Verify deallocation went through the tracking resource
    EXPECT_GE(tracking_resource->deallocate_count.load(), 1);
    EXPECT_EQ(tracking_resource->allocate_count.load(),
              tracking_resource->deallocate_count.load());
}

TEST(DbnParserComponentTest, DefaultAllocatorUsedWhenNoneInjected) {
    MockSink sink;
    dbn_pipe::DbnParserComponent<MockSink> parser(sink);

    // No SetAllocator call - should use default allocator

    // Force scratch path
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t split = sizeof(databento::MboMsg) / 2;

    dbn_pipe::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(bytes, split));
    chain.Append(MakeSegmentFromBytes(bytes + split, sizeof(databento::MboMsg) - split));

    parser.OnData(chain);

    // Should still work correctly with default allocator
    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);
    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(sink.batches[0][0].data) % 8, 0);
}
