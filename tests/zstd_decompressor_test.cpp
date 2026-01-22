// tests/zstd_decompressor_test.cpp
#include <gtest/gtest.h>
#include <zstd.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/epoll_event_loop.hpp"
#include "lib/stream/component.hpp"
#include "lib/stream/zstd_decompressor.hpp"

using namespace dbn_pipe;

// Mock downstream that receives decompressed data
struct MockZstdDownstream {
    std::vector<std::byte> data;
    Error last_error;
    bool done = false;
    bool error_called = false;

    void OnData(BufferChain& chain) {
        while (!chain.Empty()) {
            size_t chunk_size = chain.ContiguousSize();
            const std::byte* ptr = chain.DataAt(0);
            data.insert(data.end(), ptr, ptr + chunk_size);
            chain.Consume(chunk_size);
        }
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

// Helper to create BufferChain from byte vector
BufferChain ToChain(const std::vector<std::byte>& bytes) {
    BufferChain chain;
    auto seg = std::make_shared<Segment>();
    std::memcpy(seg->data.data(), bytes.data(), bytes.size());
    seg->size = bytes.size();
    chain.Append(std::move(seg));
    return chain;
}

// Helper to create BufferChain from partial range
BufferChain ToChain(const std::vector<std::byte>& bytes, size_t offset, size_t len) {
    BufferChain chain;
    auto seg = std::make_shared<Segment>();
    std::memcpy(seg->data.data(), bytes.data() + offset, len);
    seg->size = len;
    chain.Append(std::move(seg));
    return chain;
}

TEST(ZstdDecompressorTest, FactoryCreatesInstance) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();

    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);
    ASSERT_NE(decompressor, nullptr);
}

TEST(ZstdDecompressorTest, DecompressesSimpleData) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    std::string original = "Hello, World! This is a test of zstd decompression.";
    auto compressed = CompressData(original);

    // Feed compressed data
    auto chain = ToChain(compressed);
    decompressor->OnData(chain);

    // Signal end of stream
    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_EQ(BytesToString(downstream->data), original);
}

TEST(ZstdDecompressorTest, DecompressesLargeData) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    // Create a large string (100KB of repeating pattern)
    std::string original;
    original.reserve(100 * 1024);
    for (int i = 0; i < 10000; ++i) {
        original += "Line " + std::to_string(i) + ": The quick brown fox jumps.\n";
    }

    auto compressed = CompressData(original);

    // Feed compressed data
    auto chain = ToChain(compressed);
    decompressor->OnData(chain);

    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_EQ(BytesToString(downstream->data), original);
}

TEST(ZstdDecompressorTest, DecompressesChunkedInput) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    std::string original = "This is chunked input test data for zstd decompression.";
    auto compressed = CompressData(original);

    // Feed compressed data in small chunks
    size_t chunk_size = 10;
    for (size_t i = 0; i < compressed.size(); i += chunk_size) {
        size_t len = std::min(chunk_size, compressed.size() - i);
        auto chain = ToChain(compressed, i, len);
        decompressor->OnData(chain);
    }

    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_EQ(BytesToString(downstream->data), original);
}

TEST(ZstdDecompressorTest, HandlesInvalidData) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    // Feed invalid compressed data
    std::vector<std::byte> invalid_bytes = {
        std::byte{0xFF}, std::byte{0xFE}, std::byte{0xFD}, std::byte{0xFC}
    };
    auto chain = ToChain(invalid_bytes);
    decompressor->OnData(chain);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::DecompressionError);
    EXPECT_TRUE(downstream->last_error.message.find("ZSTD") != std::string::npos);
}

TEST(ZstdDecompressorTest, SuspendAndResumeWork) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    // Initialize event loop thread ID for Suspend/Resume assertions
    loop.Poll(0);

    // Initially not suspended
    EXPECT_FALSE(decompressor->IsSuspended());

    decompressor->Suspend();
    EXPECT_TRUE(decompressor->IsSuspended());

    decompressor->Resume();
    EXPECT_FALSE(decompressor->IsSuspended());
}

TEST(ZstdDecompressorTest, BuffersDataWhenSuspended) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    // Initialize event loop thread ID for Suspend/Resume assertions
    loop.Poll(0);

    std::string original = "Data buffered while suspended.";
    auto compressed = CompressData(original);

    // Suspend before sending data
    decompressor->Suspend();

    // Feed compressed data - should be buffered
    auto chain = ToChain(compressed);
    decompressor->OnData(chain);

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
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    Error upstream_error{ErrorCode::ConnectionClosed, "Connection lost"};
    decompressor->OnError(upstream_error);

    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::ConnectionClosed);
    EXPECT_EQ(downstream->last_error.message, "Connection lost");
}

TEST(ZstdDecompressorTest, CloseCallsDoClose) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    decompressor->Close();
    EXPECT_TRUE(decompressor->IsClosed());
}

TEST(ZstdDecompressorTest, EmptyInput) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    // Send empty data - create empty chain
    BufferChain empty_chain;
    decompressor->OnData(empty_chain);

    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_TRUE(downstream->data.empty());
}

TEST(ZstdDecompressorTest, MultipleFrames) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    std::string original1 = "First frame data.";
    std::string original2 = "Second frame data.";

    auto compressed1 = CompressData(original1);
    auto compressed2 = CompressData(original2);

    // Send first frame
    auto chain1 = ToChain(compressed1);
    decompressor->OnData(chain1);

    // Send second frame
    auto chain2 = ToChain(compressed2);
    decompressor->OnData(chain2);

    decompressor->OnDone();

    EXPECT_TRUE(downstream->done);
    EXPECT_FALSE(downstream->error_called);
    EXPECT_EQ(BytesToString(downstream->data), original1 + original2);
}

TEST(ZstdDecompressorTest, ImplementsUpstreamConcept) {
    // ZstdDecompressor must satisfy Upstream concept for pipeline integration
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    // Verify Upstream interface is available
    static_assert(Upstream<ZstdDecompressor<MockZstdDownstream>>);
    SUCCEED();
}

TEST(ZstdDecompressorTest, IncompleteZstdFrameEmitsError) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockZstdDownstream>();
    auto decompressor = ZstdDecompressor<MockZstdDownstream>::Create(loop, downstream);

    std::string original = "Test data for incomplete frame detection.";
    auto compressed = CompressData(original);

    // Truncate the compressed data to simulate an incomplete frame
    // Remove the last few bytes so the frame is incomplete
    if (compressed.size() > 10) {
        compressed.resize(compressed.size() - 5);
    }

    // Feed truncated compressed data
    auto chain = ToChain(compressed);
    decompressor->OnData(chain);

    // Signal end of stream - should detect incomplete frame
    decompressor->OnDone();

    // Should emit an error, not call OnDone
    EXPECT_TRUE(downstream->error_called);
    EXPECT_EQ(downstream->last_error.code, ErrorCode::DecompressionError);
    EXPECT_TRUE(downstream->last_error.message.find("Incomplete zstd frame") != std::string::npos ||
                downstream->last_error.message.find("ZSTD") != std::string::npos);
    EXPECT_FALSE(downstream->done);
}
