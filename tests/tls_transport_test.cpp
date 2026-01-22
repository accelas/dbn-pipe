// tests/tls_transport_test.cpp
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/epoll_event_loop.hpp"
#include "lib/stream/component.hpp"
#include "lib/stream/tls_transport.hpp"

using namespace dbn_pipe;

// Mock downstream that receives decrypted data
struct MockTlsDownstream {
    std::vector<std::byte> received;
    Error last_error;
    bool done = false;

    void OnData(BufferChain& chain) {
        while (!chain.Empty()) {
            size_t chunk_size = chain.ContiguousSize();
            const std::byte* ptr = chain.DataAt(0);
            received.insert(received.end(), ptr, ptr + chunk_size);
            chain.Consume(chunk_size);
        }
    }
    void OnError(const Error& e) { last_error = e; }
    void OnDone() { done = true; }
};

// Verify MockTlsDownstream satisfies Downstream concept
static_assert(Downstream<MockTlsDownstream>);

TEST(TlsTransportTest, FactoryCreatesInstance) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockTlsDownstream>();

    auto tls = TlsTransport<MockTlsDownstream>::Create(loop, downstream);
    ASSERT_NE(tls, nullptr);
}

TEST(TlsTransportTest, ImplementsUpstreamConcept) {
    // TlsTransport must satisfy Upstream concept for pipeline integration
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockTlsDownstream>();
    auto tls = TlsTransport<MockTlsDownstream>::Create(loop, downstream);

    // Verify Upstream interface is available
    static_assert(Upstream<TlsTransport<MockTlsDownstream>>);
    SUCCEED();
}

TEST(TlsTransportTest, ImplementsDownstreamConcept) {
    // TlsTransport must satisfy Downstream concept so TcpSocket can use it
    static_assert(Downstream<TlsTransport<MockTlsDownstream>>);
    SUCCEED();
}

TEST(TlsTransportTest, SuspendAndResumeWork) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockTlsDownstream>();
    auto tls = TlsTransport<MockTlsDownstream>::Create(loop, downstream);

    // Initialize event loop thread ID for Suspend/Resume assertions
    loop.Poll(0);

    // Initially not suspended
    EXPECT_FALSE(tls->IsSuspended());

    tls->Suspend();
    EXPECT_TRUE(tls->IsSuspended());

    tls->Resume();
    EXPECT_FALSE(tls->IsSuspended());
}

TEST(TlsTransportTest, CloseCallsDoClose) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockTlsDownstream>();
    auto tls = TlsTransport<MockTlsDownstream>::Create(loop, downstream);

    tls->Close();
    EXPECT_TRUE(tls->IsClosed());
}

TEST(TlsTransportTest, HandshakeNotCompleteInitially) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockTlsDownstream>();
    auto tls = TlsTransport<MockTlsDownstream>::Create(loop, downstream);

    EXPECT_FALSE(tls->IsHandshakeComplete());
}

TEST(TlsTransportTest, CanSetHostnameForSNI) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockTlsDownstream>();
    auto tls = TlsTransport<MockTlsDownstream>::Create(loop, downstream);

    // Setting hostname should not throw
    tls->SetHostname("example.com");
    SUCCEED();
}

// Integration test: verify the TLS objects are properly initialized
TEST(TlsTransportTest, OpenSSLObjectsInitialized) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockTlsDownstream>();
    auto tls = TlsTransport<MockTlsDownstream>::Create(loop, downstream);

    // The TLS socket should have valid SSL context and connection
    // We can't directly check internals, but construction should succeed
    ASSERT_NE(tls, nullptr);
}

// Test that StartHandshake initiates TLS negotiation
TEST(TlsTransportTest, StartHandshakeProducesData) {
    EpollEventLoop loop;
    auto downstream = std::make_shared<MockTlsDownstream>();
    auto tls = TlsTransport<MockTlsDownstream>::Create(loop, downstream);

    tls->SetHostname("test.example.com");

    // Track if upstream write callback was called
    std::vector<std::byte> handshake_data;
    tls->SetUpstreamWriteCallback([&](BufferChain chain) {
        while (!chain.Empty()) {
            size_t chunk_size = chain.ContiguousSize();
            const std::byte* ptr = chain.DataAt(0);
            handshake_data.insert(handshake_data.end(), ptr, ptr + chunk_size);
            chain.Consume(chunk_size);
        }
    });

    tls->StartHandshake();

    // After starting handshake, client hello should be produced
    EXPECT_FALSE(handshake_data.empty());
}
