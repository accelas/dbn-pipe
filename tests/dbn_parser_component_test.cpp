// tests/dbn_parser_component_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include <databento/constants.hpp>
#include <databento/record.hpp>

#include "src/buffer_chain.hpp"
#include "src/dbn_parser_component.hpp"
#include "src/pipeline_component.hpp"
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

// Helper to create a segment containing data from a record
std::shared_ptr<databento_async::Segment> MakeSegmentFromRecord(
    const databento::MboMsg* msg) {
    auto seg = std::make_shared<databento_async::Segment>();
    std::memcpy(seg->data.data(), msg, sizeof(databento::MboMsg));
    seg->size = sizeof(databento::MboMsg);
    return seg;
}

// Helper to create a segment from raw bytes
std::shared_ptr<databento_async::Segment> MakeSegmentFromBytes(
    const std::byte* data, size_t len) {
    auto seg = std::make_shared<databento_async::Segment>();
    std::memcpy(seg->data.data(), data, len);
    seg->size = len;
    return seg;
}

// Helper to create a BufferChain with a single record
databento_async::BufferChain MakeChainWithRecord(const databento::MboMsg* msg) {
    databento_async::BufferChain chain;
    chain.Append(MakeSegmentFromRecord(msg));
    return chain;
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
    databento_async::DbnParserComponent<MockSink> parser(sink);

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

    auto seg = std::make_shared<databento_async::Segment>();
    std::memcpy(seg->data.data(), buffer1, sizeof(databento::MboMsg));
    std::memcpy(seg->data.data() + sizeof(databento::MboMsg), buffer2, sizeof(databento::MboMsg));
    seg->size = 2 * sizeof(databento::MboMsg);

    databento_async::BufferChain chain;
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
    databento_async::DbnParserComponent<MockSink> parser(sink);

    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t half = sizeof(databento::MboMsg) / 2;

    // First call: partial record
    databento_async::BufferChain chain;
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

    // First call: first record complete + half of second
    size_t half = sizeof(databento::MboMsg) / 2;
    auto seg1 = std::make_shared<databento_async::Segment>();
    std::memcpy(seg1->data.data(), buffer1, sizeof(databento::MboMsg));
    std::memcpy(seg1->data.data() + sizeof(databento::MboMsg), buffer2, half);
    seg1->size = sizeof(databento::MboMsg) + half;

    databento_async::BufferChain chain;
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

    databento_async::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(buffer, sizeof(buffer)));

    parser.OnData(chain);

    EXPECT_TRUE(sink.error_called);
    EXPECT_EQ(sink.last_error.code, databento_async::ErrorCode::ParseError);
    EXPECT_TRUE(sink.last_error.message.find("smaller than header") != std::string::npos);
}

TEST(DbnParserComponentTest, MaxRecordSizeIsReasonable) {
    // The RecordHeader.length field is uint8_t, so max record size via header is 255 * 4 = 1020 bytes.
    // kMaxRecordSize at 64KB provides headroom for future format changes and ensures
    // the validation is in place even though current DBN format can't exceed it.

    // Verify kMaxRecordSize is larger than any valid DBN record
    constexpr size_t max_dbn_record = 255 * databento::kRecordHeaderLengthMultiplier;
    EXPECT_GT(databento_async::kMaxRecordSize, max_dbn_record);

    // Verify constant has the expected value (64KB)
    EXPECT_EQ(databento_async::kMaxRecordSize, 64 * 1024);
}

TEST(DbnParserComponentTest, IncompleteRecordAtEndEmitsError) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Add partial data to chain
    std::byte partial[] = {std::byte{0x01}, std::byte{0x02}};
    databento_async::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(partial, sizeof(partial)));

    parser.OnData(chain);

    // Signal end of stream with incomplete data
    parser.OnComplete(chain);

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
    auto chain = MakeChainWithRecord(msg);
    parser.OnData(chain);

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

TEST(DbnParserComponentTest, EmptyChainDoesNothing) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    databento_async::BufferChain chain;
    parser.OnData(chain);

    EXPECT_EQ(sink.batches.size(), 0);
    EXPECT_FALSE(sink.error_called);

    // OnComplete should work fine with empty chain
    parser.OnComplete(chain);
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
    auto seg = std::make_shared<databento_async::Segment>();
    size_t offset = 0;
    std::memcpy(seg->data.data() + offset, &header, sizeof(header));
    offset += sizeof(header);
    std::memset(seg->data.data() + offset, 0, 16);  // Metadata content
    offset += 16;
    std::memcpy(seg->data.data() + offset, msg, sizeof(databento::MboMsg));
    offset += sizeof(databento::MboMsg);
    seg->size = offset;

    databento_async::BufferChain chain;
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

    // Combine everything into a single buffer first
    std::vector<std::byte> all_data;
    auto* header_bytes = reinterpret_cast<const std::byte*>(&header);
    all_data.insert(all_data.end(), header_bytes, header_bytes + sizeof(header));
    all_data.insert(all_data.end(), 16, std::byte{0x00});
    auto* msg_bytes = reinterpret_cast<const std::byte*>(msg);
    all_data.insert(all_data.end(), msg_bytes, msg_bytes + sizeof(databento::MboMsg));

    // Send in small segments (4 bytes each)
    databento_async::BufferChain chain;
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
    databento_async::DbnParserComponent<MockSink> parser(sink);

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

    databento_async::BufferChain chain;
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
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Create a record and split it across two segments
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t split = sizeof(databento::MboMsg) / 2;

    databento_async::BufferChain chain;
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
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Create a record and split it so header spans two segments
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);

    // Split in the middle of the header (header is 8 bytes)
    size_t split = sizeof(databento::RecordHeader) / 2;

    databento_async::BufferChain chain;
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
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Create a record and split it across three segments
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t record_size = sizeof(databento::MboMsg);
    size_t third = record_size / 3;

    databento_async::BufferChain chain;
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
    databento_async::DbnParserComponent<MockSink> parser(sink);

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
    auto seg1 = std::make_shared<databento_async::Segment>();
    std::memcpy(seg1->data.data(), rec1, record_size);
    std::memcpy(seg1->data.data() + record_size, rec2, record_size / 2);
    seg1->size = record_size + record_size / 2;

    // Segment 2: second half of record2 + record3 complete
    auto seg2 = std::make_shared<databento_async::Segment>();
    std::memcpy(seg2->data.data(), rec2 + record_size / 2, record_size - record_size / 2);
    std::memcpy(seg2->data.data() + record_size - record_size / 2, rec3, record_size);
    seg2->size = record_size - record_size / 2 + record_size;

    databento_async::BufferChain chain;
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
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Create a single record in one segment (contiguous case)
    auto* msg = CreateMinimalRecord(100);
    auto seg = MakeSegmentFromRecord(msg);
    const std::byte* original_ptr = seg->data.data();

    databento_async::BufferChain chain;
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
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Create a record spanning two segments to force scratch buffer allocation
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t split = 8;  // Split early to maximize data in second segment

    databento_async::BufferChain chain;
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
    databento_async::DbnParserComponent<MockSink> parser(sink);

    auto* msg = CreateMinimalRecord(100);
    auto chain = MakeChainWithRecord(msg);

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);

    // Take a copy of the RecordRef
    databento_async::RecordRef ref = sink.batches[0][0];

    // Clear batches (but ref still holds keepalive)
    sink.batches.clear();

    // Data should still be valid because ref.keepalive holds the segment
    EXPECT_EQ(ref.Header().instrument_id, 100);
}

TEST(DbnParserComponentTest, BoundaryCrossingKeepaliveKeepsDataValid) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Create boundary-crossing record
    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t split = sizeof(databento::MboMsg) / 2;

    databento_async::BufferChain chain;
    chain.Append(MakeSegmentFromBytes(bytes, split));
    chain.Append(MakeSegmentFromBytes(bytes + split, sizeof(databento::MboMsg) - split));

    parser.OnData(chain);

    ASSERT_EQ(sink.batches.size(), 1);

    // Take a copy of the RecordRef
    databento_async::RecordRef ref = sink.batches[0][0];

    // Clear batches
    sink.batches.clear();

    // Data should still be valid because ref.keepalive holds the scratch buffer
    EXPECT_EQ(ref.Header().instrument_id, 100);
}

TEST(DbnParserComponentTest, MetadataSpanningSegments) {
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

    // Split metadata header across two segments
    auto* header_bytes = reinterpret_cast<const std::byte*>(&header);

    databento_async::BufferChain chain;

    // First segment: half of DBN header
    chain.Append(MakeSegmentFromBytes(header_bytes, 4));

    // Second segment: rest of header + metadata content + record
    auto seg2 = std::make_shared<databento_async::Segment>();
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
// Legacy Read() adapter tests (byte vector -> BufferChain conversion)
// ============================================================================

// Helper to create pmr::vector from record
std::pmr::vector<std::byte> MsgToPmrBytes(const databento::MboMsg* msg) {
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    return std::pmr::vector<std::byte>(bytes, bytes + sizeof(databento::MboMsg));
}

TEST(DbnParserComponentTest, ReadAdapterParsesSingleRecord) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    auto* msg = CreateMinimalRecord(100);
    auto data = MsgToPmrBytes(msg);

    parser.Read(std::move(data));

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);
    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
}

TEST(DbnParserComponentTest, ReadAdapterHandlesChunkedData) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    size_t half = sizeof(databento::MboMsg) / 2;

    // First chunk: partial record
    std::pmr::vector<std::byte> chunk1(bytes, bytes + half);
    parser.Read(std::move(chunk1));

    EXPECT_EQ(sink.batches.size(), 0);  // No complete record yet

    // Second chunk: rest of record
    std::pmr::vector<std::byte> chunk2(bytes + half, bytes + sizeof(databento::MboMsg));
    parser.Read(std::move(chunk2));

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);
    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
}

TEST(DbnParserComponentTest, ReadAdapterHandlesMultipleRecords) {
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

    // Combine into single pmr::vector
    std::pmr::vector<std::byte> data;
    data.insert(data.end(), buffer1, buffer1 + sizeof(databento::MboMsg));
    data.insert(data.end(), buffer2, buffer2 + sizeof(databento::MboMsg));

    parser.Read(std::move(data));

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 2);
    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
    EXPECT_EQ(sink.batches[0][1].Header().instrument_id, 200);
}

TEST(DbnParserComponentTest, ReadAdapterIncompleteAtEndEmitsError) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Send partial data
    std::pmr::vector<std::byte> partial{std::byte{0x01}, std::byte{0x02}};
    parser.Read(std::move(partial));

    // Signal end of stream
    parser.OnComplete();

    EXPECT_TRUE(sink.error_called);
    EXPECT_EQ(sink.last_error.code, databento_async::ErrorCode::ParseError);
    EXPECT_TRUE(sink.last_error.message.find("Incomplete record") != std::string::npos);
}

TEST(DbnParserComponentTest, ReadAdapterWithMetadata) {
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

    // Combine metadata + record into pmr::vector
    std::pmr::vector<std::byte> data;
    auto* header_bytes = reinterpret_cast<const std::byte*>(&header);
    data.insert(data.end(), header_bytes, header_bytes + sizeof(header));
    data.insert(data.end(), 16, std::byte{0x00});  // Metadata content
    auto* msg_bytes = reinterpret_cast<const std::byte*>(msg);
    data.insert(data.end(), msg_bytes, msg_bytes + sizeof(databento::MboMsg));

    parser.Read(std::move(data));

    ASSERT_EQ(sink.batches.size(), 1);
    ASSERT_EQ(sink.batches[0].size(), 1);
    EXPECT_EQ(sink.batches[0][0].Header().instrument_id, 100);
}

TEST(DbnParserComponentTest, ReadAdapterEmptyDataDoesNothing) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    std::pmr::vector<std::byte> empty;
    parser.Read(std::move(empty));

    EXPECT_EQ(sink.batches.size(), 0);
    EXPECT_FALSE(sink.error_called);

    parser.OnComplete();
    EXPECT_TRUE(sink.complete_called);
}

TEST(DbnParserComponentTest, ReadAdapterIgnoresDataAfterError) {
    MockSink sink;
    databento_async::DbnParserComponent<MockSink> parser(sink);

    // Trigger error
    databento_async::Error err{databento_async::ErrorCode::ConnectionFailed, "error"};
    parser.OnError(err);

    // Try to send more data via Read
    auto* msg = CreateMinimalRecord(100);
    parser.Read(MsgToPmrBytes(msg));

    // No batches should be delivered
    EXPECT_EQ(sink.batches.size(), 0);
}
