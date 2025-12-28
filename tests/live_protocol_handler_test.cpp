// tests/live_protocol_handler_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "src/live_protocol_handler.hpp"
#include "src/pipeline.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

// Mock downstream that satisfies Downstream concept
struct MockDownstream {
    std::vector<std::byte> received;
    Error last_error;
    bool error_called = false;
    bool done_called = false;

    void Read(std::pmr::vector<std::byte> data) {
        received.insert(received.end(), data.begin(), data.end());
    }
    void OnError(const Error& e) {
        last_error = e;
        error_called = true;
    }
    void OnDone() { done_called = true; }
};

// Verify MockDownstream satisfies Downstream concept
static_assert(Downstream<MockDownstream>);

// Helper to create byte vector from string
std::pmr::vector<std::byte> ToBytes(const std::string& str) {
    std::pmr::vector<std::byte> result;
    result.resize(str.size());
    std::memcpy(result.data(), str.data(), str.size());
    return result;
}

// Helper to convert bytes to string for comparison
std::string ToString(const std::vector<std::byte>& data) {
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

class LiveProtocolHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        downstream_ = std::make_shared<MockDownstream>();
        handler_ = LiveProtocolHandler<MockDownstream>::Create(
            reactor_, downstream_, "test_api_key");
    }

    Reactor reactor_;
    std::shared_ptr<MockDownstream> downstream_;
    std::shared_ptr<LiveProtocolHandler<MockDownstream>> handler_;
    std::vector<std::byte> sent_data_;

    void SetupWriteCallback() {
        handler_->SetWriteCallback([this](std::pmr::vector<std::byte> data) {
            sent_data_.insert(sent_data_.end(), data.begin(), data.end());
        });
    }
};

TEST_F(LiveProtocolHandlerTest, FactoryCreation) {
    ASSERT_NE(handler_, nullptr);
}

TEST_F(LiveProtocolHandlerTest, InitialStateIsWaitingGreeting) {
    EXPECT_EQ(handler_->GetState(), LiveProtocolState::WaitingGreeting);
}

TEST_F(LiveProtocolHandlerTest, ReceivesGreetingTransitionsToWaitingChallenge) {
    handler_->Read(ToBytes("session123|v1.0\n"));

    EXPECT_EQ(handler_->GetState(), LiveProtocolState::WaitingChallenge);
    EXPECT_EQ(handler_->GetGreeting().session_id, "session123");
    EXPECT_EQ(handler_->GetGreeting().version, "v1.0");
}

TEST_F(LiveProtocolHandlerTest, ReceivesGreetingWithCRLF) {
    handler_->Read(ToBytes("session456|v2.0\r\n"));

    EXPECT_EQ(handler_->GetState(), LiveProtocolState::WaitingChallenge);
    EXPECT_EQ(handler_->GetGreeting().session_id, "session456");
    EXPECT_EQ(handler_->GetGreeting().version, "v2.0");
}

TEST_F(LiveProtocolHandlerTest, InvalidGreetingEmitsError) {
    handler_->Read(ToBytes("invalid_greeting_no_pipe\n"));

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::InvalidGreeting);
}

TEST_F(LiveProtocolHandlerTest, ReceivesChallengeAndSendsAuth) {
    SetupWriteCallback();

    // Send greeting first
    handler_->Read(ToBytes("session|v1\n"));
    EXPECT_EQ(handler_->GetState(), LiveProtocolState::WaitingChallenge);

    // Send challenge
    handler_->Read(ToBytes("cram=test_challenge\n"));

    EXPECT_EQ(handler_->GetState(), LiveProtocolState::Authenticating);

    // Check that auth was sent
    std::string sent = ToString(sent_data_);
    EXPECT_TRUE(sent.starts_with("auth="));
    EXPECT_TRUE(sent.find('|') != std::string::npos);  // Has bucket separator
    EXPECT_TRUE(sent.ends_with("\n"));
}

TEST_F(LiveProtocolHandlerTest, InvalidChallengeEmitsError) {
    // Send greeting first
    handler_->Read(ToBytes("session|v1\n"));

    // Send invalid challenge
    handler_->Read(ToBytes("invalid_challenge\n"));

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::InvalidChallenge);
}

TEST_F(LiveProtocolHandlerTest, AuthSuccessTransitionsToReady) {
    SetupWriteCallback();

    // Complete handshake
    handler_->Read(ToBytes("session|v1\n"));
    handler_->Read(ToBytes("cram=challenge\n"));
    handler_->Read(ToBytes("success\n"));  // Any non-error response

    EXPECT_EQ(handler_->GetState(), LiveProtocolState::Ready);
    EXPECT_FALSE(downstream_->error_called);
}

TEST_F(LiveProtocolHandlerTest, AuthFailureEmitsError) {
    SetupWriteCallback();

    // Complete handshake but fail auth
    handler_->Read(ToBytes("session|v1\n"));
    handler_->Read(ToBytes("cram=challenge\n"));
    handler_->Read(ToBytes("err=invalid_api_key\n"));

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::AuthFailed);
}

TEST_F(LiveProtocolHandlerTest, StreamingModePassesBinaryDataToDownstream) {
    SetupWriteCallback();

    // Complete handshake
    handler_->Read(ToBytes("session|v1\n"));
    handler_->Read(ToBytes("cram=challenge\n"));
    handler_->Read(ToBytes("success\n"));
    EXPECT_EQ(handler_->GetState(), LiveProtocolState::Ready);

    // Subscribe and start streaming
    handler_->Subscribe("GLBX.MDP3", "ESZ4", "mbp-1");
    handler_->StartStreaming();
    EXPECT_EQ(handler_->GetState(), LiveProtocolState::Streaming);

    // Send binary data
    std::pmr::vector<std::byte> binary_data;
    binary_data.push_back(std::byte{0x01});
    binary_data.push_back(std::byte{0x02});
    binary_data.push_back(std::byte{0x03});
    handler_->Read(std::move(binary_data));

    ASSERT_EQ(downstream_->received.size(), 3);
    EXPECT_EQ(downstream_->received[0], std::byte{0x01});
    EXPECT_EQ(downstream_->received[1], std::byte{0x02});
    EXPECT_EQ(downstream_->received[2], std::byte{0x03});
}

TEST_F(LiveProtocolHandlerTest, SubscribeSendsSubscriptionWhenReady) {
    SetupWriteCallback();

    // Complete handshake
    handler_->Read(ToBytes("session|v1\n"));
    handler_->Read(ToBytes("cram=challenge\n"));
    sent_data_.clear();  // Clear auth message
    handler_->Read(ToBytes("success\n"));

    // Subscribe
    handler_->Subscribe("GLBX.MDP3", "ESZ4", "mbp-1");

    std::string sent = ToString(sent_data_);
    EXPECT_TRUE(sent.find("subscription=") != std::string::npos);
    EXPECT_TRUE(sent.find("GLBX.MDP3") != std::string::npos);
    EXPECT_TRUE(sent.find("ESZ4") != std::string::npos);
    EXPECT_TRUE(sent.find("mbp-1") != std::string::npos);
}

TEST_F(LiveProtocolHandlerTest, SubscribeQueuesIfNotReady) {
    SetupWriteCallback();

    // Subscribe before ready
    handler_->Subscribe("GLBX.MDP3", "ESZ4", "mbp-1");

    // Should not have sent anything yet
    EXPECT_TRUE(sent_data_.empty());

    // Complete handshake
    handler_->Read(ToBytes("session|v1\n"));
    handler_->Read(ToBytes("cram=challenge\n"));
    sent_data_.clear();
    handler_->Read(ToBytes("success\n"));

    // Now subscription should be sent
    std::string sent = ToString(sent_data_);
    EXPECT_TRUE(sent.find("subscription=") != std::string::npos);
}

TEST_F(LiveProtocolHandlerTest, StartStreamingSendsStartSession) {
    SetupWriteCallback();

    // Complete handshake and subscribe
    handler_->Read(ToBytes("session|v1\n"));
    handler_->Read(ToBytes("cram=challenge\n"));
    handler_->Read(ToBytes("success\n"));
    handler_->Subscribe("GLBX.MDP3", "ESZ4", "mbp-1");
    sent_data_.clear();

    // Start streaming
    handler_->StartStreaming();

    std::string sent = ToString(sent_data_);
    EXPECT_EQ(sent, "start_session\n");
    EXPECT_EQ(handler_->GetState(), LiveProtocolState::Streaming);
}

TEST_F(LiveProtocolHandlerTest, LineBufferOverflowEmitsError) {
    // Send data that exceeds kMaxLineLength (8KB) without newline
    std::string large_line(9 * 1024, 'x');  // 9KB
    handler_->Read(ToBytes(large_line));

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::BufferOverflow);
    EXPECT_TRUE(downstream_->last_error.message.find("Line buffer") != std::string::npos);
}

TEST_F(LiveProtocolHandlerTest, BinaryBufferOverflowEmitsError) {
    SetupWriteCallback();

    // Complete handshake and start streaming
    handler_->Read(ToBytes("session|v1\n"));
    handler_->Read(ToBytes("cram=challenge\n"));
    handler_->Read(ToBytes("success\n"));
    handler_->Subscribe("GLBX.MDP3", "ESZ4", "mbp-1");
    handler_->StartStreaming();

    // Suspend to cause buffering
    handler_->Suspend();

    // Send more than 16MB of data
    constexpr std::size_t kChunkSize = 1024 * 1024;  // 1MB chunks
    for (int i = 0; i < 17; ++i) {
        if (downstream_->error_called) break;

        std::pmr::vector<std::byte> large_data(kChunkSize, std::byte{0x42});
        handler_->Read(std::move(large_data));
    }

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::BufferOverflow);
    EXPECT_TRUE(downstream_->last_error.message.find("Binary buffer") != std::string::npos);
}

TEST_F(LiveProtocolHandlerTest, SuspendAndResumeWork) {
    EXPECT_FALSE(handler_->IsSuspended());

    handler_->Suspend();
    EXPECT_TRUE(handler_->IsSuspended());

    handler_->Resume();
    EXPECT_FALSE(handler_->IsSuspended());
}

TEST_F(LiveProtocolHandlerTest, SuspendedBuffersDataAndResumeDrains) {
    SetupWriteCallback();

    // Complete handshake and start streaming
    handler_->Read(ToBytes("session|v1\n"));
    handler_->Read(ToBytes("cram=challenge\n"));
    handler_->Read(ToBytes("success\n"));
    handler_->Subscribe("GLBX.MDP3", "ESZ4", "mbp-1");
    handler_->StartStreaming();

    // Suspend
    handler_->Suspend();

    // Send data while suspended
    std::pmr::vector<std::byte> data1;
    data1.push_back(std::byte{0xAA});
    data1.push_back(std::byte{0xBB});
    handler_->Read(std::move(data1));

    // Data should be buffered, not forwarded
    EXPECT_TRUE(downstream_->received.empty());

    // Resume
    handler_->Resume();

    // Data should now be forwarded
    ASSERT_EQ(downstream_->received.size(), 2);
    EXPECT_EQ(downstream_->received[0], std::byte{0xAA});
    EXPECT_EQ(downstream_->received[1], std::byte{0xBB});
}

TEST_F(LiveProtocolHandlerTest, CloseCallsDoClose) {
    handler_->Close();
    EXPECT_TRUE(handler_->IsClosed());
}

TEST_F(LiveProtocolHandlerTest, OnDoneForwardsToDownstream) {
    SetupWriteCallback();

    // Complete handshake and start streaming
    handler_->Read(ToBytes("session|v1\n"));
    handler_->Read(ToBytes("cram=challenge\n"));
    handler_->Read(ToBytes("success\n"));
    handler_->Subscribe("GLBX.MDP3", "ESZ4", "mbp-1");
    handler_->StartStreaming();

    handler_->OnDone();

    EXPECT_TRUE(downstream_->done_called);
    EXPECT_FALSE(downstream_->error_called);
}

TEST_F(LiveProtocolHandlerTest, OnDoneBeforeStreamingEmitsError) {
    // Simulate connection close during authentication
    handler_->Read(ToBytes("session|v1\n"));
    handler_->OnDone();

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::ConnectionClosed);
}

TEST_F(LiveProtocolHandlerTest, OnErrorForwardsToDownstream) {
    Error err{ErrorCode::ConnectionFailed, "test error"};
    handler_->OnError(err);

    EXPECT_TRUE(downstream_->error_called);
    EXPECT_EQ(downstream_->last_error.code, ErrorCode::ConnectionFailed);
    EXPECT_EQ(downstream_->last_error.message, "test error");
}

TEST_F(LiveProtocolHandlerTest, ImplementsSuspendableInterface) {
    // Should be able to cast to Suspendable
    Suspendable* suspendable = handler_.get();
    ASSERT_NE(suspendable, nullptr);

    suspendable->Suspend();
    EXPECT_TRUE(suspendable->IsSuspended());

    suspendable->Resume();
    EXPECT_FALSE(suspendable->IsSuspended());
}

TEST_F(LiveProtocolHandlerTest, ChunkedGreetingParsed) {
    // Send greeting in multiple chunks
    handler_->Read(ToBytes("sess"));
    EXPECT_EQ(handler_->GetState(), LiveProtocolState::WaitingGreeting);

    handler_->Read(ToBytes("ion|v"));
    EXPECT_EQ(handler_->GetState(), LiveProtocolState::WaitingGreeting);

    handler_->Read(ToBytes("1\n"));
    EXPECT_EQ(handler_->GetState(), LiveProtocolState::WaitingChallenge);
    EXPECT_EQ(handler_->GetGreeting().session_id, "session");
}

TEST_F(LiveProtocolHandlerTest, MultipleMessagesInSingleRead) {
    SetupWriteCallback();

    // Send greeting and challenge in single packet
    handler_->Read(ToBytes("session|v1\ncram=test\n"));

    EXPECT_EQ(handler_->GetState(), LiveProtocolState::Authenticating);
    EXPECT_FALSE(sent_data_.empty());  // Auth was sent
}

TEST_F(LiveProtocolHandlerTest, RemainingDataAfterStreamingTransitionForwarded) {
    SetupWriteCallback();

    // Complete handshake
    handler_->Read(ToBytes("session|v1\n"));
    handler_->Read(ToBytes("cram=challenge\n"));
    handler_->Read(ToBytes("success\n"));
    handler_->Subscribe("GLBX.MDP3", "ESZ4", "mbp-1");
    handler_->StartStreaming();

    // Clear any received data
    downstream_->received.clear();

    // After start_session response, server immediately sends binary data
    // This simulates the case where start confirmation and binary data
    // arrive in the same packet
    handler_->Read(ToBytes("\x01\x02\x03"));

    // Binary data should be forwarded
    ASSERT_EQ(downstream_->received.size(), 3);
    EXPECT_EQ(downstream_->received[0], std::byte{0x01});
}
