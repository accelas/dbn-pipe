// tests/dbn_parser_component_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include <databento/constants.hpp>
#include <databento/record.hpp>

#include "src/dbn_parser_component.hpp"
#include "src/pipeline.hpp"
#include "src/reactor.hpp"

// Mock sink that receives parsed records
struct MockSink {
    std::vector<databento::RecordHeader> headers;
    databento_async::Error last_error;
    bool done = false;
    bool error_called = false;

    void OnRecord(const databento::Record& rec) {
        headers.push_back(rec.Header());
    }
    void OnError(const databento_async::Error& e) {
        last_error = e;
        error_called = true;
    }
    void OnDone() { done = true; }
};

// Verify MockSink satisfies RecordDownstream concept
static_assert(databento_async::RecordDownstream<MockSink>);

// Helper to create a minimal valid MboMsg record
// Using MboMsg as it's the smallest concrete record type
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

TEST(DbnParserComponentTest, FactoryCreation) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);
    ASSERT_NE(parser, nullptr);
}

TEST(DbnParserComponentTest, ParsesSingleRecord) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    auto* msg = CreateMinimalRecord(100);

    std::pmr::vector<std::byte> data;
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    data.assign(bytes, bytes + sizeof(databento::MboMsg));

    parser->Read(std::move(data));

    ASSERT_EQ(downstream->headers.size(), 1);
    EXPECT_EQ(downstream->headers[0].instrument_id, 100);
}

TEST(DbnParserComponentTest, ParsesMultipleRecords) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    // Create two records in separate buffers
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

    std::pmr::vector<std::byte> data;
    data.insert(data.end(), buffer1, buffer1 + sizeof(databento::MboMsg));
    data.insert(data.end(), buffer2, buffer2 + sizeof(databento::MboMsg));

    parser->Read(std::move(data));

    ASSERT_EQ(downstream->headers.size(), 2);
    EXPECT_EQ(downstream->headers[0].instrument_id, 100);
    EXPECT_EQ(downstream->headers[1].instrument_id, 200);
}

TEST(DbnParserComponentTest, ParsesChunkedRecords) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    auto* msg = CreateMinimalRecord(100);
    auto* bytes = reinterpret_cast<const std::byte*>(msg);

    // Send record in two chunks
    size_t half = sizeof(databento::MboMsg) / 2;

    std::pmr::vector<std::byte> chunk1;
    chunk1.assign(bytes, bytes + half);
    parser->Read(std::move(chunk1));

    // No record yet (incomplete)
    EXPECT_EQ(downstream->headers.size(), 0);

    std::pmr::vector<std::byte> chunk2;
    chunk2.assign(bytes + half, bytes + sizeof(databento::MboMsg));
    parser->Read(std::move(chunk2));

    // Now we should have the record
    ASSERT_EQ(downstream->headers.size(), 1);
    EXPECT_EQ(downstream->headers[0].instrument_id, 100);
}

TEST(DbnParserComponentTest, ForwardsOnDone) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    parser->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
}

TEST(DbnParserComponentTest, ForwardsOnError) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    databento_async::Error err{databento_async::ErrorCode::ConnectionFailed, "test error"};
    parser->OnError(err);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, databento_async::ErrorCode::ConnectionFailed);
    EXPECT_EQ(downstream->last_error.message, "test error");
}

TEST(DbnParserComponentTest, InvalidRecordSizeEmitsError) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    // Create a header with invalid size (0)
    alignas(8) std::byte buffer[sizeof(databento::RecordHeader)] = {};
    auto* hdr = reinterpret_cast<databento::RecordHeader*>(buffer);
    hdr->length = 0;  // Invalid - size will be 0
    hdr->rtype = databento::RType::Mbo;
    hdr->publisher_id = 1;
    hdr->instrument_id = 100;

    std::pmr::vector<std::byte> data;
    data.assign(buffer, buffer + sizeof(buffer));

    parser->Read(std::move(data));

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, databento_async::ErrorCode::ParseError);
    EXPECT_TRUE(downstream->last_error.message.find("Invalid record size") != std::string::npos);
}

TEST(DbnParserComponentTest, BufferOverflowEmitsError) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    // Suspend the parser so data accumulates
    parser->Suspend();

    // Try to send more than 16MB of data
    constexpr size_t kChunkSize = 1024 * 1024;  // 1MB chunks
    for (int i = 0; i < 17; ++i) {
        if (downstream->error_called) break;

        std::pmr::vector<std::byte> large_data(kChunkSize, std::byte{0x42});
        parser->Read(std::move(large_data));
    }

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, databento_async::ErrorCode::BufferOverflow);
}

TEST(DbnParserComponentTest, IncompleteRecordAtEndEmitsError) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    // Send partial header
    std::pmr::vector<std::byte> partial;
    partial.push_back(std::byte{0x01});
    partial.push_back(std::byte{0x02});
    parser->Read(std::move(partial));

    // Signal end of stream with incomplete data
    parser->OnDone();

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, databento_async::ErrorCode::ParseError);
    EXPECT_TRUE(downstream->last_error.message.find("Incomplete record") != std::string::npos);
    EXPECT_FALSE(downstream->done);
}

TEST(DbnParserComponentTest, SuspendAndResumeWork) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    // Initially not suspended
    EXPECT_FALSE(parser->IsSuspended());

    parser->Suspend();
    EXPECT_TRUE(parser->IsSuspended());

    parser->Resume();
    EXPECT_FALSE(parser->IsSuspended());
}

TEST(DbnParserComponentTest, BuffersDataWhenSuspended) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    auto* msg = CreateMinimalRecord(100);

    // Suspend before sending data
    parser->Suspend();

    std::pmr::vector<std::byte> data;
    auto* bytes = reinterpret_cast<const std::byte*>(msg);
    data.assign(bytes, bytes + sizeof(databento::MboMsg));
    parser->Read(std::move(data));

    // Data should be buffered, not processed yet
    EXPECT_EQ(downstream->headers.size(), 0);

    // Resume and data should be processed
    parser->Resume();

    ASSERT_EQ(downstream->headers.size(), 1);
    EXPECT_EQ(downstream->headers[0].instrument_id, 100);
}

TEST(DbnParserComponentTest, CloseCallsDoClose) {
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    parser->Close();
    EXPECT_TRUE(parser->IsClosed());
}

TEST(DbnParserComponentTest, ImplementsSuspendableInterface) {
    // DbnParserComponent must inherit from Suspendable
    databento_async::Reactor reactor;
    auto downstream = std::make_shared<MockSink>();
    auto parser = databento_async::DbnParserComponent<MockSink>::Create(reactor, downstream);

    // Should be able to cast to Suspendable
    databento_async::Suspendable* suspendable = parser.get();
    ASSERT_NE(suspendable, nullptr);

    suspendable->Suspend();
    EXPECT_TRUE(suspendable->IsSuspended());

    suspendable->Resume();
    EXPECT_FALSE(suspendable->IsSuspended());
}
