// tests/zstd_decompressor_test.cpp
#include <gtest/gtest.h>
#include <zstd.h>

#include <memory>
#include <string>
#include <vector>

#include "src/zstd_decompressor.hpp"
#include "src/pipeline.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

// Mock downstream that receives decompressed data
struct MockZstdDownstream {
    std::vector<std::byte> data;
    Error last_error;
    bool done = false;
    bool error_called = false;

    void Read(std::pmr::vector<std::byte> incoming) {
        data.insert(data.end(), incoming.begin(), incoming.end());
    }
    void OnError(const Error& e) {
        last_error = e;
        error_called = true;
    }
    void OnDone() { done = true; }
};

// Verify MockZstdDownstream satisfies Downstream concept
static_assert(Downstream<MockZstdDownstream>);

// Helper to compress data using ZSTD_compress
std::vector<std::byte> CompressData(const std::string& input) {
    size_t max_compressed_size = ZSTD_compressBound(input.size());
    std::vector<std::byte> compressed(max_compressed_size);

    size_t compressed_size = ZSTD_compress(
        compressed.data(), compressed.size(),
        input.data(), input.size(),
        1  // compression level
    );

    if (ZSTD_isError(compressed_size)) {
        throw std::runtime_error(
            std::string("ZSTD compression failed: ") + ZSTD_getErrorName(compressed_size));
    }

    compressed.resize(compressed_size);
    return compressed;
}

// Helper to convert decompressed bytes to string
std::string BytesToString(const std::vector<std::byte>& bytes) {
    std::string result;
    result.reserve(bytes.size());
    for (auto b : bytes) {
        result.push_back(static_cast<char>(b));
    }
    return result;
}

TEST(ZstdDecompressorTest, FactoryCreatesInstance) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();

    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);
    ASSERT_NE(decompressor, nullptr);
}

TEST(ZstdDecompressorTest, DecompressesSimpleData) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    std::string original = "Hello, World! This is a test of zstd decompression.";
    auto compressed = CompressData(original);

    // Feed compressed data
    std::pmr::vector<std::byte> input;
    input.assign(compressed.begin(), compressed.end());
    decompressor->Read(std::move(input));

    // Signal end of stream
    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_EQ(BytesToString(downstream->data), original);
}

TEST(ZstdDecompressorTest, DecompressesLargeData) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    // Create a large string (100KB of repeating pattern)
    std::string original;
    original.reserve(100 * 1024);
    for (int i = 0; i < 10000; ++i) {
        original += "Line " + std::to_string(i) + ": The quick brown fox jumps.\n";
    }

    auto compressed = CompressData(original);

    // Feed compressed data
    std::pmr::vector<std::byte> input;
    input.assign(compressed.begin(), compressed.end());
    decompressor->Read(std::move(input));

    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_EQ(BytesToString(downstream->data), original);
}

TEST(ZstdDecompressorTest, DecompressesChunkedInput) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    std::string original = "This is chunked input test data for zstd decompression.";
    auto compressed = CompressData(original);

    // Feed compressed data in small chunks
    size_t chunk_size = 10;
    for (size_t i = 0; i < compressed.size(); i += chunk_size) {
        size_t end = std::min(i + chunk_size, compressed.size());
        std::pmr::vector<std::byte> chunk;
        chunk.assign(compressed.begin() + static_cast<long>(i),
                     compressed.begin() + static_cast<long>(end));
        decompressor->Read(std::move(chunk));
    }

    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_EQ(BytesToString(downstream->data), original);
}

TEST(ZstdDecompressorTest, HandlesInvalidData) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    // Feed invalid compressed data
    std::pmr::vector<std::byte> invalid_data;
    invalid_data.push_back(std::byte{0xFF});
    invalid_data.push_back(std::byte{0xFE});
    invalid_data.push_back(std::byte{0xFD});
    invalid_data.push_back(std::byte{0xFC});

    decompressor->Read(std::move(invalid_data));

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::DecompressionError);
    EXPECT_TRUE(downstream->last_error.message.find("ZSTD") != std::string::npos);
}

TEST(ZstdDecompressorTest, SuspendAndResumeWork) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    // Initially not suspended
    EXPECT_FALSE(decompressor->IsSuspended());

    decompressor->Suspend();
    EXPECT_TRUE(decompressor->IsSuspended());

    decompressor->Resume();
    EXPECT_FALSE(decompressor->IsSuspended());
}

TEST(ZstdDecompressorTest, BuffersDataWhenSuspended) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    std::string original = "Data buffered while suspended.";
    auto compressed = CompressData(original);

    // Suspend before sending data
    decompressor->Suspend();

    // Feed compressed data - should be buffered
    std::pmr::vector<std::byte> input;
    input.assign(compressed.begin(), compressed.end());
    decompressor->Read(std::move(input));

    // Data should not be processed yet
    EXPECT_TRUE(downstream->data.empty());

    // Resume and data should be processed
    decompressor->Resume();

    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_EQ(BytesToString(downstream->data), original);
}

TEST(ZstdDecompressorTest, ForwardsUpstreamError) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    Error upstream_error{ErrorCode::ConnectionClosed, "Connection lost"};
    decompressor->OnError(upstream_error);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::ConnectionClosed);
    EXPECT_EQ(downstream->last_error.message, "Connection lost");
}

TEST(ZstdDecompressorTest, CloseCallsDoClose) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    decompressor->Close();
    EXPECT_TRUE(decompressor->IsClosed());
}

TEST(ZstdDecompressorTest, EmptyInput) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    // Send empty data
    std::pmr::vector<std::byte> empty;
    decompressor->Read(std::move(empty));

    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_TRUE(downstream->data.empty());
}

TEST(ZstdDecompressorTest, MultipleFrames) {
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    std::string original1 = "First frame data.";
    std::string original2 = "Second frame data.";

    auto compressed1 = CompressData(original1);
    auto compressed2 = CompressData(original2);

    // Send first frame
    std::pmr::vector<std::byte> input1;
    input1.assign(compressed1.begin(), compressed1.end());
    decompressor->Read(std::move(input1));

    // Send second frame
    std::pmr::vector<std::byte> input2;
    input2.assign(compressed2.begin(), compressed2.end());
    decompressor->Read(std::move(input2));

    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_EQ(BytesToString(downstream->data), original1 + original2);
}

TEST(ZstdDecompressorTest, ImplementsUpstreamConcept) {
    // ZstdDecompressor must satisfy Upstream concept for pipeline integration
    Reactor reactor;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(reactor, downstream);

    // Verify Upstream interface is available
    static_assert(Upstream<ZstdDecompressor<MockZstdDownstream>>);
    SUCCEED();
}
