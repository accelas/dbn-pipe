// tests/live_client_test.cpp
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>

#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

#include <databento/record.hpp>

#include "src/live_client.hpp"
#include "src/reactor.hpp"

using namespace databento_async;

// Helper to create sockaddr_storage from IPv4 address and port
sockaddr_storage make_addr(const char* ip, int port) {
    sockaddr_storage storage{};
    auto* addr = reinterpret_cast<sockaddr_in*>(&storage);
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr->sin_addr);
    return storage;
}

// Ignore SIGPIPE at program startup to prevent crashes on socket writes
namespace {
struct SigpipeIgnorer {
    SigpipeIgnorer() {
        signal(SIGPIPE, SIG_IGN);
    }
} sigpipe_ignorer;
}

// Helper class that simulates the databento Live Subscription Gateway
// Runs in a separate thread to avoid reactor deadlocks
class MockLsgServer {
public:
    MockLsgServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(listener_, 0);

        int opt = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // Let OS pick port

        bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(listener_, 1);

        socklen_t len = sizeof(addr);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }

    ~MockLsgServer() {
        Stop();
        // Note: client_fd_ and listener_ may have been closed in Stop()
        // but close() on -1 is a no-op, and double-close is undefined but
        // we track the fd so it should be safe.
        if (client_fd_ >= 0) {
            close(client_fd_);
            client_fd_ = -1;
        }
        if (listener_ >= 0) {
            close(listener_);
            listener_ = -1;
        }
    }

    int port() const { return port_; }

    void Stop() {
        running_ = false;
        // Shutdown sockets to unblock any blocking calls
        if (client_fd_ >= 0) {
            shutdown(client_fd_, SHUT_RDWR);
        }
        if (listener_ >= 0) {
            shutdown(listener_, SHUT_RDWR);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Run standard auth flow in background thread
    void RunAuthFlow(bool auth_success = true, std::string error_msg = "") {
        running_ = true;
        thread_ = std::thread([this, auth_success, error_msg]() {
            // Accept connection
            client_fd_ = accept(listener_, nullptr, nullptr);
            if (client_fd_ < 0) return;

            // Small delay to let client set up
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // Send greeting
            Send("lsg-test|v1\n");

            // Send challenge
            Send("cram=testchallenge123\n");

            // Wait for and receive auth message
            std::string auth_msg = Receive();
            if (auth_msg.empty()) return;

            // Validate auth message format
            if (auth_msg.find("auth=") == std::string::npos) return;

            // Send auth response
            if (auth_success) {
                Send("success=1|session_id=42|\n");
            } else {
                Send("success=0|error=" + error_msg + "|\n");
            }

            // Wait for subscription if successful
            if (auth_success) {
                std::string sub_msg = Receive();
                // sub_msg should contain schema, symbols, etc.
            }
        });
    }

    // Run auth flow and then send a record after start_session
    void RunStreamingFlow() {
        running_ = true;
        thread_ = std::thread([this]() {
            // Accept connection
            client_fd_ = accept(listener_, nullptr, nullptr);
            if (client_fd_ < 0) return;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // Auth flow
            Send("lsg-test|v1\n");
            Send("cram=streamtest123\n");

            std::string auth_msg = Receive();
            if (auth_msg.empty()) return;

            Send("success=1|session_id=99|\n");

            // Wait for subscription
            std::string sub_msg = Receive();

            // Wait for start_session
            std::string start_msg = Receive();
            if (start_msg.find("start_session") == std::string::npos) return;

            // Send a test record
            SendTestRecord();
        });
    }

    void SendTestRecord() {
        // Create a minimal record
        std::vector<std::byte> record(64);
        auto* hdr = reinterpret_cast<databento::RecordHeader*>(record.data());
        hdr->length = 64 / databento::RecordHeader::kLengthMultiplier;
        hdr->rtype = databento::RType::Mbp0;
        hdr->publisher_id = 1;
        hdr->instrument_id = 12345;
        hdr->ts_event = databento::UnixNanos{std::chrono::nanoseconds{1609459200000000000ULL}};

        write(client_fd_, record.data(), record.size());
    }

private:
    void Send(const std::string& msg) {
        write(client_fd_, msg.data(), msg.size());
    }

    std::string Receive() {
        char buf[4096];
        std::string result;

        // Set a receive timeout
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(client_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ssize_t n = read(client_fd_, buf, sizeof(buf));
        if (n > 0) {
            result.assign(buf, n);
        }
        return result;
    }

    int listener_ = -1;
    int client_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

class LiveClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Nothing special
    }

    void TearDown() override {
        // Ensure server thread is stopped
    }
};

// ============================================================================
// Task 4: New orchestrator tests
// ============================================================================

TEST_F(LiveClientTest, ConstructsWithApiKey) {
    // Test: Creating a client should start in Disconnected state
    Reactor reactor;
    LiveClient client(reactor, "db-test-api-key");

    EXPECT_EQ(client.GetState(), LiveClient::State::Disconnected);
}

TEST_F(LiveClientTest, ConnectChangesState) {
    // Test: Connect() should transition to Connecting state
    Reactor reactor;
    MockLsgServer server;
    server.RunAuthFlow(true);

    LiveClient client(reactor, "db-test-api-key");
    EXPECT_EQ(client.GetState(), LiveClient::State::Disconnected);

    client.Connect(make_addr("127.0.0.1", server.port()));

    EXPECT_EQ(client.GetState(), LiveClient::State::Connecting);

    server.Stop();
}

TEST_F(LiveClientTest, StopResetsState) {
    // Test: Stop() should return to Disconnected state
    Reactor reactor;
    MockLsgServer server;
    server.RunAuthFlow(true);

    LiveClient client(reactor, "db-test-api-key");

    client.Connect(make_addr("127.0.0.1", server.port()));
    EXPECT_EQ(client.GetState(), LiveClient::State::Connecting);

    client.Stop();
    EXPECT_EQ(client.GetState(), LiveClient::State::Disconnected);

    server.Stop();
}

TEST_F(LiveClientTest, ConnectWhenNotDisconnectedErrors) {
    // Test: Calling Connect() when not disconnected should error with InvalidState
    Reactor reactor;
    MockLsgServer server;
    server.RunAuthFlow(true);

    LiveClient client(reactor, "db-test-api-key");

    bool got_error = false;
    ErrorCode received_code{};

    client.OnError([&](const Error& e) {
        got_error = true;
        received_code = e.code;
    });

    // First connect should succeed (move to Connecting)
    client.Connect(make_addr("127.0.0.1", server.port()));
    EXPECT_EQ(client.GetState(), LiveClient::State::Connecting);

    // Second connect while still connecting should error
    client.Connect(make_addr("127.0.0.1", server.port()));

    EXPECT_TRUE(got_error);
    EXPECT_EQ(received_code, ErrorCode::InvalidState);

    server.Stop();
}

// ============================================================================
// Original integration tests (may need adaptation for new pipeline architecture)
// ============================================================================

TEST_F(LiveClientTest, ConnectAndAuth) {
    Reactor reactor;
    MockLsgServer server;
    server.RunAuthFlow(true);

    LiveClient client(reactor, "db-test-api-key-12345");

    bool reached_ready = false;

    client.OnError([&](const Error& e) {
        ADD_FAILURE() << "Unexpected error: " << e.message;
        reactor.Stop();
    });

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    // Poll until we reach Ready state or timeout
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (client.GetState() == LiveClient::State::Ready) {
            reached_ready = true;
            break;
        }
    }

    server.Stop();
    EXPECT_TRUE(reached_ready);
    EXPECT_EQ(client.GetState(), LiveClient::State::Ready);
}

TEST_F(LiveClientTest, ReceiveRecord) {
    Reactor reactor;
    MockLsgServer server;
    server.RunStreamingFlow();

    LiveClient client(reactor, "db-test-api-key-12345");

    bool record_received = false;
    std::uint32_t received_instrument_id = 0;

    client.OnRecord([&](const databento::Record& rec) {
        record_received = true;
        received_instrument_id = rec.Header().instrument_id;
        reactor.Stop();
    });

    client.OnError([&](const Error& e) {
        ADD_FAILURE() << "Unexpected error: " << e.message;
        reactor.Stop();
    });

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    // Wait for Ready state
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (client.GetState() == LiveClient::State::Ready) {
            break;
        }
    }

    ASSERT_EQ(client.GetState(), LiveClient::State::Ready);

    // Start streaming
    client.Start();
    EXPECT_EQ(client.GetState(), LiveClient::State::Streaming);

    // Wait for record
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        if (record_received) break;
        reactor.Poll(100);
    }

    server.Stop();
    EXPECT_TRUE(record_received);
    EXPECT_EQ(received_instrument_id, 12345u);
}

TEST_F(LiveClientTest, ConnectionError) {
    Reactor reactor;
    LiveClient client(reactor, "db-test-api-key");

    bool got_error = false;

    client.OnError([&](const Error& e) {
        got_error = true;
        EXPECT_EQ(e.code, ErrorCode::ConnectionFailed);
        reactor.Stop();
    });

    // Connect to port that's not listening
    client.Connect(make_addr("127.0.0.1", 59999));

    // Poll for result with timeout
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (got_error) break;
    }

    EXPECT_TRUE(got_error);
    EXPECT_EQ(client.GetState(), LiveClient::State::Error);
}

TEST_F(LiveClientTest, AuthenticationFailure) {
    Reactor reactor;
    MockLsgServer server;
    server.RunAuthFlow(false, "Invalid API key");

    LiveClient client(reactor, "db-invalid-api-key");

    bool got_error = false;

    client.OnError([&](const Error& e) {
        got_error = true;
        EXPECT_EQ(e.code, ErrorCode::AuthFailed);
        reactor.Stop();
    });

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    // Poll for result with timeout
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (got_error) break;
    }

    server.Stop();
    EXPECT_TRUE(got_error);
    EXPECT_EQ(client.GetState(), LiveClient::State::Error);
}

TEST_F(LiveClientTest, CloseWhileConnected) {
    Reactor reactor;
    MockLsgServer server;
    server.RunAuthFlow(true);

    LiveClient client(reactor, "db-test-api-key-12345");

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    // Poll until past Connecting
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        reactor.Poll(100);
        if (client.GetState() != LiveClient::State::Connecting &&
            client.GetState() != LiveClient::State::Disconnected) {
            break;
        }
    }

    // If we're still connecting or already disconnected (connection failed quickly),
    // that's okay for this test - we just want to test that Close() works
    if (client.GetState() != LiveClient::State::Disconnected) {
        client.Close();
        EXPECT_EQ(client.GetState(), LiveClient::State::Disconnected);
    }

    server.Stop();
}

TEST_F(LiveClientTest, StateTransitions) {
    Reactor reactor;
    MockLsgServer server;
    server.RunAuthFlow(true);

    LiveClient client(reactor, "db-test-api-key-12345");

    EXPECT_EQ(client.GetState(), LiveClient::State::Disconnected);

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    EXPECT_EQ(client.GetState(), LiveClient::State::Connecting);

    // Poll until Ready
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (client.GetState() == LiveClient::State::Ready) {
            break;
        }
    }

    server.Stop();
    EXPECT_EQ(client.GetState(), LiveClient::State::Ready);
}

// ============================================================================
// Task 4: Suspendable interface tests
// ============================================================================

TEST_F(LiveClientTest, SuspendableInterfaceBasic) {
    // Test: Suspendable interface methods work correctly
    Reactor reactor;
    MockLsgServer server;
    server.RunStreamingFlow();

    LiveClient client(reactor, "db-test-api-key-12345");

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    // Poll until Ready state
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (client.GetState() == LiveClient::State::Ready) {
            break;
        }
    }

    ASSERT_EQ(client.GetState(), LiveClient::State::Ready);

    // Start streaming
    client.Start();
    EXPECT_EQ(client.GetState(), LiveClient::State::Streaming);

    // Poll once to ensure reactor thread ID is set
    reactor.Poll(0);

    // Test IsSuspended initial state
    EXPECT_FALSE(client.IsSuspended());

    // Test Suspend
    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    // Test Resume
    client.Resume();
    EXPECT_FALSE(client.IsSuspended());

    server.Stop();
}

TEST_F(LiveClientTest, SuspendIsIdempotent) {
    // Test: Multiple Suspend() calls don't crash and state remains suspended
    Reactor reactor;
    MockLsgServer server;
    server.RunStreamingFlow();

    LiveClient client(reactor, "db-test-api-key-12345");

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    // Poll until Ready state
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (client.GetState() == LiveClient::State::Ready) {
            break;
        }
    }

    ASSERT_EQ(client.GetState(), LiveClient::State::Ready);

    client.Start();
    reactor.Poll(0);  // Set reactor thread ID

    // Call Suspend multiple times - should be idempotent
    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    client.Suspend();  // Second call - should be safe
    EXPECT_TRUE(client.IsSuspended());

    client.Suspend();  // Third call - should be safe
    EXPECT_TRUE(client.IsSuspended());

    server.Stop();
}

TEST_F(LiveClientTest, ResumeIsIdempotent) {
    // Test: Multiple Resume() calls don't crash and state remains not suspended
    Reactor reactor;
    MockLsgServer server;
    server.RunStreamingFlow();

    LiveClient client(reactor, "db-test-api-key-12345");

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    // Poll until Ready state
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (client.GetState() == LiveClient::State::Ready) {
            break;
        }
    }

    ASSERT_EQ(client.GetState(), LiveClient::State::Ready);

    client.Start();
    reactor.Poll(0);  // Set reactor thread ID

    // Start from suspended state
    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    // Call Resume multiple times - should be idempotent
    client.Resume();
    EXPECT_FALSE(client.IsSuspended());

    client.Resume();  // Second call - should be safe
    EXPECT_FALSE(client.IsSuspended());

    client.Resume();  // Third call - should be safe
    EXPECT_FALSE(client.IsSuspended());

    server.Stop();
}

TEST_F(LiveClientTest, SuspendResumeSequence) {
    // Test: Suspend/Resume sequence works correctly
    Reactor reactor;
    MockLsgServer server;
    server.RunStreamingFlow();

    LiveClient client(reactor, "db-test-api-key-12345");

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    // Poll until Ready state
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (client.GetState() == LiveClient::State::Ready) {
            break;
        }
    }

    ASSERT_EQ(client.GetState(), LiveClient::State::Ready);

    client.Start();
    reactor.Poll(0);  // Set reactor thread ID

    // Test sequence: suspend -> resume -> suspend -> resume
    EXPECT_FALSE(client.IsSuspended());

    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    client.Resume();
    EXPECT_FALSE(client.IsSuspended());

    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    client.Resume();
    EXPECT_FALSE(client.IsSuspended());

    server.Stop();
}

TEST_F(LiveClientTest, CloseResetsSuspendState) {
    // Test: Close() resets the suspended state
    Reactor reactor;
    MockLsgServer server;
    server.RunStreamingFlow();

    LiveClient client(reactor, "db-test-api-key-12345");

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    // Poll until Ready state
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (client.GetState() == LiveClient::State::Ready) {
            break;
        }
    }

    ASSERT_EQ(client.GetState(), LiveClient::State::Ready);

    client.Start();
    reactor.Poll(0);  // Set reactor thread ID

    // Suspend first
    client.Suspend();
    EXPECT_TRUE(client.IsSuspended());

    // Close should reset suspend state
    client.Close();
    EXPECT_FALSE(client.IsSuspended());
    EXPECT_EQ(client.GetState(), LiveClient::State::Disconnected);

    server.Stop();
}

TEST_F(LiveClientTest, IsSuspendedIsThreadSafe) {
    // Test: IsSuspended() can be called from any thread
    Reactor reactor;
    MockLsgServer server;
    server.RunStreamingFlow();

    LiveClient client(reactor, "db-test-api-key-12345");

    client.Subscribe("test.dbn", "AAPL", "trades");
    client.Connect(make_addr("127.0.0.1", server.port()));

    // Poll until Ready state
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        reactor.Poll(100);
        if (client.GetState() == LiveClient::State::Ready) {
            break;
        }
    }

    ASSERT_EQ(client.GetState(), LiveClient::State::Ready);

    client.Start();
    reactor.Poll(0);  // Set reactor thread ID

    // Start a thread that queries IsSuspended while we modify state
    std::atomic<bool> stop_thread{false};
    std::atomic<int> query_count{0};

    std::thread query_thread([&]() {
        while (!stop_thread.load()) {
            // This should be safe to call from any thread
            [[maybe_unused]] bool suspended = client.IsSuspended();
            query_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Toggle suspend state while the other thread queries
    for (int i = 0; i < 100; ++i) {
        client.Suspend();
        client.Resume();
    }

    stop_thread.store(true);
    query_thread.join();

    // Verify thread was actually doing queries
    EXPECT_GT(query_count.load(), 0);

    server.Stop();
}
