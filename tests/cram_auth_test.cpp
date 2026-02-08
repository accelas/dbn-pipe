// SPDX-License-Identifier: MIT

// tests/cram_auth_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/cram_auth.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"

using namespace dbn_pipe;

// Mock downstream that satisfies Downstream concept
struct MockCramDownstream {
    std::vector<std::byte> received;
    Error last_error;
    bool error_called = false;
    bool done_called = false;

    void OnData(BufferChain& chain) {
        while (!chain.Empty()) {
            size_t chunk_size = chain.ContiguousSize();
            const std::byte* ptr = chain.DataAt(0);
            received.insert(received.end(), ptr, ptr + chunk_size);
            chain.Consume(chunk_size);
        }
    }
    void OnError(const Error& e) {
        last_error = e;
        error_called = true;
    }
    void OnDone() { done_called = true; }
};

// Verify MockCramDownstream satisfies Downstream concept
static_assert(Downstream<MockCramDownstream>);

// Helper to create BufferChain from string
BufferChain ToChain(const std::string& str) {
    BufferChain chain;
    auto seg = std::make_shared<Segment>();
    std::memcpy(seg->data.data(), str.data(), str.size());
    seg->size = str.size();
    chain.Append(std::move(seg));
    return chain;
}

// Helper to create BufferChain from byte vector
// Splits data across multiple segments if needed (each segment is 64KB max)
BufferChain ToChain(const std::pmr::vector<std::byte>& bytes) {
    BufferChain chain;
    size_t offset = 0;
    while (offset < bytes.size()) {
        auto seg = std::make_shared<Segment>();
        size_t to_copy = std::min(Segment::kSize, bytes.size() - offset);
        std::memcpy(seg->data.data(), bytes.data() + offset, to_copy);
        seg->size = to_copy;
        chain.Append(std::move(seg));
        offset += to_copy;
    }
    return chain;
}

// Helper to convert bytes to string for comparison
std::string ToString(const std::vector<std::byte>& data) {
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

class CramAuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        downstream_ = std::make_shared<MockCramDownstream>();
        handler_ = CramAuth<MockCramDownstream>::Create(
            downstream_, "test_api_key");
    }

    // Helper to send string data to handler
    void SendData(const std::string& str) {
        auto chain = ToChain(str);
        handler_->OnData(chain);
    }

    // Helper to send byte data to handler
    void SendBytes(std::pmr::vector<std::byte> bytes) {
        auto chain = ToChain(bytes);
        handler_->OnData(chain);
    }

    std::shared_ptr<MockCramDownstream> downstream_;
    std::shared_ptr<CramAuth<MockCramDownstream>> handler_;
    std::vector<std::byte> sent_data_;

    void SetupWriteCallback() {
        handler_->SetUpstreamWriteCallback([this](BufferChain chain) {
            while (!chain.Empty()) {
                size_t chunk_size = chain.ContiguousSize();
                const std::byte* ptr = chain.DataAt(0);
                sent_data_.insert(sent_data_.end(), ptr, ptr + chunk_size);
                chain.Consume(chunk_size);
            }
        });
    }
};

TEST_F(CramAuthTest, FactoryCreation) {
    ASSERT_NE(handler_, nullptr);
}

TEST_F(CramAuthTest, InitialStateIsWaitingGreeting) {
    EXPECT_EQ(handler_->GetState(), CramAuthState::WaitingGreeting);
}

TEST_F(CramAuthTest, ReceivesGreetingTransitionsToWaitingChallenge) {
    SendData("session123|v1.0\n");

    EXPECT_EQ(handler_->GetState(), CramAuthState::WaitingChallenge);
    EXPECT_EQ(handler_->GetGreeting().session_id, "session123");
    EXPECT_EQ(handler_->GetGreeting().version, "v1.0");
}

TEST_F(CramAuthTest, ReceivesGreetingWithCRLF) {
    SendData("session456|v2.0\r\n");

    EXPECT_EQ(handler_->GetState(), CramAuthState::WaitingChallenge);
    EXPECT_EQ(handler_->GetGreeting().session_id, "session456");
    EXPECT_EQ(handler_->GetGreeting().version, "v2.0");
}

TEST_F(CramAuthTest, GreetingWithoutPipeIsAccepted) {
    // New behavior: greetings without pipe are accepted (fallback format)
    // This supports new LSG greeting formats like "lsg_version=0.7.2"
    SendData("greeting_without_pipe\n");

    EXPECT_FALSE(downstream_->error_called);
    EXPECT_EQ(handler_->GetState(), CramAuthState::WaitingChallenge);
    EXPECT_TRUE(handler_->GetGreeting().session_id.empty());  // No session_id
    EXPECT_EQ(handler_->GetGreeting().version, "greeting_without_pipe");
}

TEST_F(CramAuthTest, ReceivesChallengeAndSendsAuth) {
    SetupWriteCallback();

    // Send greeting first
    SendData("session|v1\n");
    EXPECT_EQ(handler_->GetState(), CramAuthState::WaitingChallenge);

    // Send challenge
    SendData("cram=test_challenge\n");

    EXPECT_EQ(handler_->GetState(), CramAuthState::Authenticating);

    // Check that auth was sent
    std::string sent = ToString(sent_data_);
    EXPECT_TRUE(sent.starts_with("auth="));
    EXPECT_TRUE(sent.find('|') != std::string::npos);  // Has bucket separator
    EXPECT_TRUE(sent.ends_with("\n"));
}

TEST_F(CramAuthTest, InvalidChallengeEmitsError) {
    // Send greeting first
    SendData("session|v1\n");

    // Send invalid challenge
    SendData("invalid_challenge\n");

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::InvalidChallenge);
}

TEST_F(CramAuthTest, AuthSuccessTransitionsToReady) {
    SetupWriteCallback();

    // Complete handshake
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    SendData("success\n");  // Any non-error response

    EXPECT_EQ(handler_->GetState(), CramAuthState::Ready);
    EXPECT_FALSE(downstream_->error_called);
}

TEST_F(CramAuthTest, AuthFailureEmitsError) {
    SetupWriteCallback();

    // Complete handshake but fail auth
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    SendData("err=invalid_api_key\n");

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::AuthFailed);
}

TEST_F(CramAuthTest, StreamingModePassesBinaryDataToDownstream) {
    SetupWriteCallback();

    // Complete handshake
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    SendData("success\n");
    EXPECT_EQ(handler_->GetState(), CramAuthState::Ready);

    // Subscribe and start streaming (new 2-arg signature: symbols, schema)
    handler_->Subscribe("ESZ4", "mbp-1");
    handler_->StartStreaming();
    EXPECT_EQ(handler_->GetState(), CramAuthState::Streaming);

    // Send binary data
    std::pmr::vector<std::byte> binary_data;
    binary_data.push_back(std::byte{0x01});
    binary_data.push_back(std::byte{0x02});
    binary_data.push_back(std::byte{0x03});
    SendBytes(std::move(binary_data));

    ASSERT_EQ(downstream_->received.size(), 3);
    EXPECT_EQ(downstream_->received[0], std::byte{0x01});
    EXPECT_EQ(downstream_->received[1], std::byte{0x02});
    EXPECT_EQ(downstream_->received[2], std::byte{0x03});
}

TEST_F(CramAuthTest, SubscribeSendsSubscriptionWhenReady) {
    SetupWriteCallback();

    // Complete handshake
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    sent_data_.clear();  // Clear auth message
    SendData("success\n");

    // Subscribe (new format: symbols, schema)
    handler_->Subscribe("ESZ4", "mbp-1");

    std::string sent = ToString(sent_data_);
    // New format: schema=<s>|stype_in=<t>|id=<n>|symbols=<syms>|snapshot=<b>|is_last=<b>
    EXPECT_TRUE(sent.find("schema=mbp-1") != std::string::npos);
    EXPECT_TRUE(sent.find("stype_in=raw_symbol") != std::string::npos);
    EXPECT_TRUE(sent.find("id=1") != std::string::npos);
    EXPECT_TRUE(sent.find("symbols=ESZ4") != std::string::npos);
    EXPECT_TRUE(sent.find("snapshot=0") != std::string::npos);
    EXPECT_TRUE(sent.find("is_last=1") != std::string::npos);
}

TEST_F(CramAuthTest, SubscribeQueuesIfNotReady) {
    SetupWriteCallback();

    // Subscribe before ready (new format: symbols, schema)
    handler_->Subscribe("ESZ4", "mbp-1");

    // Should not have sent anything yet
    EXPECT_TRUE(sent_data_.empty());

    // Complete handshake
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    sent_data_.clear();
    SendData("success\n");

    // Now subscription should be sent with new format
    std::string sent = ToString(sent_data_);
    EXPECT_TRUE(sent.find("schema=") != std::string::npos);
}

TEST_F(CramAuthTest, StartStreamingSendsStartSession) {
    SetupWriteCallback();

    // Complete handshake and subscribe
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    SendData("success\n");
    handler_->Subscribe("ESZ4", "mbp-1");
    sent_data_.clear();

    // Start streaming
    handler_->StartStreaming();

    std::string sent = ToString(sent_data_);
    EXPECT_EQ(sent, "start_session\n");
    EXPECT_EQ(handler_->GetState(), CramAuthState::Streaming);
}

TEST_F(CramAuthTest, LineBufferOverflowEmitsError) {
    // Send data that exceeds kMaxLineLength (8KB) without newline
    std::string large_line(9 * 1024, 'x');  // 9KB
    SendData(large_line);

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::BufferOverflow);
    EXPECT_TRUE(downstream_->last_error.message.find("Line buffer") != std::string::npos);
}

TEST_F(CramAuthTest, BinaryBufferOverflowEmitsError) {
    SetupWriteCallback();

    // Complete handshake and start streaming
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    SendData("success\n");
    handler_->Subscribe("ESZ4", "mbp-1");
    handler_->StartStreaming();

    // Suspend to cause buffering
    handler_->Suspend();

    // Send more than 16MB of data
    constexpr std::size_t kChunkSize = 1024 * 1024;  // 1MB chunks
    for (int i = 0; i < 17; ++i) {
        if (downstream_->error_called) break;

        std::pmr::vector<std::byte> large_data(kChunkSize, std::byte{0x42});
        SendBytes(std::move(large_data));
    }

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::BufferOverflow);
    EXPECT_TRUE(downstream_->last_error.message.find("Binary buffer") != std::string::npos);
}

TEST_F(CramAuthTest, SuspendAndResumeWork) {
    EXPECT_FALSE(handler_->IsSuspended());

    handler_->Suspend();
    EXPECT_TRUE(handler_->IsSuspended());

    handler_->Resume();
    EXPECT_FALSE(handler_->IsSuspended());
}

TEST_F(CramAuthTest, SuspendedBuffersDataAndResumeDrains) {
    SetupWriteCallback();

    // Complete handshake and start streaming
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    SendData("success\n");
    handler_->Subscribe("ESZ4", "mbp-1");
    handler_->StartStreaming();

    // Suspend
    handler_->Suspend();

    // Send data while suspended
    std::pmr::vector<std::byte> data1;
    data1.push_back(std::byte{0xAA});
    data1.push_back(std::byte{0xBB});
    SendBytes(std::move(data1));

    // Data should be buffered, not forwarded
    EXPECT_TRUE(downstream_->received.empty());

    // Resume
    handler_->Resume();

    // Data should now be forwarded
    ASSERT_EQ(downstream_->received.size(), 2);
    EXPECT_EQ(downstream_->received[0], std::byte{0xAA});
    EXPECT_EQ(downstream_->received[1], std::byte{0xBB});
}

TEST_F(CramAuthTest, CloseCallsDoClose) {
    handler_->Close();
    EXPECT_TRUE(handler_->IsClosed());
}

TEST_F(CramAuthTest, OnDoneForwardsToDownstream) {
    SetupWriteCallback();

    // Complete handshake and start streaming
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    SendData("success\n");
    handler_->Subscribe("ESZ4", "mbp-1");
    handler_->StartStreaming();

    handler_->OnDone();

    EXPECT_TRUE(downstream_->done_called);
    EXPECT_FALSE(downstream_->error_called);
}

TEST_F(CramAuthTest, OnDoneBeforeStreamingEmitsError) {
    // Simulate connection close during authentication
    SendData("session|v1\n");
    handler_->OnDone();

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::ConnectionClosed);
}

TEST_F(CramAuthTest, OnErrorForwardsToDownstream) {
    Error err{ErrorCode::ConnectionFailed, "test error"};
    handler_->OnError(err);

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::ConnectionFailed);
    EXPECT_EQ(downstream_->last_error.message, "test error");
}

TEST_F(CramAuthTest, ImplementsSuspendableInterface) {
    // Should be able to cast to Suspendable
    Suspendable* suspendable = handler_.get();
    ASSERT_NE(suspendable, nullptr);

    suspendable->Suspend();
    EXPECT_TRUE(suspendable->IsSuspended());

    suspendable->Resume();
    EXPECT_FALSE(suspendable->IsSuspended());
}

TEST_F(CramAuthTest, ChunkedGreetingParsed) {
    // Send greeting in multiple chunks
    SendData("sess");
    EXPECT_EQ(handler_->GetState(), CramAuthState::WaitingGreeting);

    SendData("ion|v");
    EXPECT_EQ(handler_->GetState(), CramAuthState::WaitingGreeting);

    SendData("1\n");
    EXPECT_EQ(handler_->GetState(), CramAuthState::WaitingChallenge);
    EXPECT_EQ(handler_->GetGreeting().session_id, "session");
}

TEST_F(CramAuthTest, MultipleMessagesInSingleRead) {
    SetupWriteCallback();

    // Send greeting and challenge in single packet
    SendData("session|v1\ncram=test\n");

    EXPECT_EQ(handler_->GetState(), CramAuthState::Authenticating);
    EXPECT_FALSE(sent_data_.empty());  // Auth was sent
}

TEST_F(CramAuthTest, RemainingDataAfterStreamingTransitionForwarded) {
    SetupWriteCallback();

    // Complete handshake
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    SendData("success\n");
    handler_->Subscribe("ESZ4", "mbp-1");
    handler_->StartStreaming();

    // Clear any received data
    downstream_->received.clear();

    // After start_session response, server immediately sends binary data
    // This simulates the case where start confirmation and binary data
    // arrive in the same packet
    SendData("\x01\x02\x03");

    // Binary data should be forwarded
    ASSERT_EQ(downstream_->received.size(), 3);
    EXPECT_EQ(downstream_->received[0], std::byte{0x01});
}

TEST_F(CramAuthTest, SetAllocatorUsesExternalAllocator) {
    SetupWriteCallback();

    // Create an external SegmentAllocator and wire it to CramAuth
    SegmentAllocator external_alloc;
    handler_->SetAllocator(&external_alloc);

    // Verify GetAllocator returns the external allocator
    EXPECT_EQ(&handler_->GetAllocator(), &external_alloc);

    // Complete handshake and start streaming to exercise AppendBytes paths
    SendData("session|v1\n");
    SendData("cram=challenge\n");
    SendData("success\n");
    handler_->Subscribe("ESZ4", "mbp-1");
    handler_->StartStreaming();
    EXPECT_EQ(handler_->GetState(), CramAuthState::Streaming);

    // Send binary data - exercises the allocator-backed AppendBytes path
    std::pmr::vector<std::byte> binary_data;
    binary_data.push_back(std::byte{0xDE});
    binary_data.push_back(std::byte{0xAD});
    SendBytes(std::move(binary_data));

    ASSERT_EQ(downstream_->received.size(), 2);
    EXPECT_EQ(downstream_->received[0], std::byte{0xDE});
    EXPECT_EQ(downstream_->received[1], std::byte{0xAD});
}

TEST_F(CramAuthTest, DefaultAllocatorUsedWhenNoExternalSet) {
    // Without calling SetAllocator, GetAllocator should return default
    SegmentAllocator& alloc = handler_->GetAllocator();
    // Should be valid - allocate a segment to verify
    auto seg = alloc.Allocate();
    ASSERT_NE(seg, nullptr);
    EXPECT_EQ(seg->size, 0u);
}
