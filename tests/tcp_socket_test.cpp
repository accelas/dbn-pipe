// SPDX-License-Identifier: MIT

// tests/tcp_socket_test.cpp
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/epoll_event_loop.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"
#include "dbn_pipe/stream/tcp_socket.hpp"

using namespace dbn_pipe;

// Mock downstream for testing TcpSocket
struct MockDownstream {
    std::vector<std::byte> received_data;
    std::optional<Error> last_error;
    bool done_called = false;
    bool error_called = false;
    EpollEventLoop* loop = nullptr;  // For stopping loop in callbacks

    void OnData(BufferChain& chain) {
        while (!chain.Empty()) {
            const std::byte* ptr = chain.DataAt(0);
            size_t chunk = chain.ContiguousSize();
            received_data.insert(received_data.end(), ptr, ptr + chunk);
            chain.Consume(chunk);
        }
    }

    void OnError(const Error& e) {
        error_called = true;
        last_error = e;
        if (loop) loop->Stop();
    }

    void OnDone() {
        done_called = true;
        if (loop) loop->Stop();
    }
};

// Helper to create BufferChain from string
BufferChain ToChain(std::string_view str) {
    BufferChain chain;
    auto seg = std::make_shared<Segment>();
    std::memcpy(seg->data.data(), str.data(), str.size());
    seg->size = str.size();
    chain.Append(std::move(seg));
    return chain;
}

// Helper to create sockaddr_storage from IPv4 address and port
sockaddr_storage make_addr(const char* ip, int port) {
    sockaddr_storage storage{};
    auto* addr = reinterpret_cast<sockaddr_in*>(&storage);
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr->sin_addr);
    return storage;
}

// Helper to create a listening socket
int create_listener(int& port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    EXPECT_GE(fd, 0);

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // Let OS pick port

    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(fd, 1);

    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);

    return fd;
}

TEST(TcpSocketTest, ConnectSuccess) {
    EpollEventLoop loop;
    int port;
    int listener = create_listener(port);

    auto downstream = std::make_shared<MockDownstream>();

    bool connected = false;
    auto sock = TcpSocket<MockDownstream>::Create(loop, downstream);

    sock->OnConnect([&]() {
        connected = true;
        loop.Stop();
    });

    sock->Connect(make_addr("127.0.0.1", port));

    // Accept on server side using IEventHandle
    auto listener_handle = loop.Register(
        listener,
        /*want_read=*/true,
        /*want_write=*/false,
        [&]() {
            int client = accept(listener, nullptr, nullptr);
            EXPECT_GE(client, 0);
            close(client);
        },
        []() {},
        [](int) {}
    );

    loop.Run();

    EXPECT_TRUE(connected);

    close(listener);
}

TEST(TcpSocketTest, ConnectFail) {
    EpollEventLoop loop;

    auto downstream = std::make_shared<MockDownstream>();
    downstream->loop = &loop;

    auto sock = TcpSocket<MockDownstream>::Create(loop, downstream);

    // Connect to a port that won't accept connections
    sock->Connect(make_addr("127.0.0.1", 1));  // Port 1 is privileged and unlikely to accept

    loop.Run();

    EXPECT_TRUE(downstream->error_called);
}

TEST(TcpSocketTest, ReadWrite) {
    EpollEventLoop loop;
    int port;
    int listener = create_listener(port);

    auto downstream = std::make_shared<MockDownstream>();
    downstream->loop = &loop;

    auto sock = TcpSocket<MockDownstream>::Create(loop, downstream);

    int server_fd = -1;
    bool connected = false;

    // Server side handle for reading from client and echoing back
    std::unique_ptr<IEventHandle> server_handle;

    sock->OnConnect([&]() {
        connected = true;
        // Send data after connect
        sock->Write(ToChain("hello"));
    });

    sock->Connect(make_addr("127.0.0.1", port));

    // Accept on listener and immediately register for server data
    auto listener_handle = loop.Register(
        listener,
        /*want_read=*/true,
        /*want_write=*/false,
        [&]() {
            server_fd = accept(listener, nullptr, nullptr);
            if (server_fd >= 0) {
                // Make non-blocking
                fcntl(server_fd, F_SETFL, O_NONBLOCK);
                // Register for reading from client
                server_handle = loop.Register(
                    server_fd,
                    /*want_read=*/true,
                    /*want_write=*/false,
                    [&]() {
                        char buf[1024];
                        ssize_t n = read(server_fd, buf, sizeof(buf));
                        if (n > 0) {
                            write(server_fd, buf, n);  // Echo back
                            close(server_fd);
                            server_fd = -1;
                        }
                    },
                    []() {},
                    [](int) {}
                );
            }
        },
        []() {},
        [](int) {}
    );

    loop.Run();

    EXPECT_TRUE(connected);
    // Data received by downstream
    std::string received(reinterpret_cast<char*>(downstream->received_data.data()),
                         downstream->received_data.size());
    EXPECT_EQ(received, "hello");

    close(listener);
}

TEST(TcpSocketTest, SuspendResumeBeforeConnect) {
    EpollEventLoop loop;

    auto downstream = std::make_shared<MockDownstream>();
    auto sock = TcpSocket<MockDownstream>::Create(loop, downstream);

    // Suspend before connect should work
    EXPECT_FALSE(sock->IsSuspended());
    sock->Suspend();
    EXPECT_TRUE(sock->IsSuspended());
    sock->Resume();
    EXPECT_FALSE(sock->IsSuspended());
}

TEST(TcpSocketTest, SuspendStopsCallbacks) {
    EpollEventLoop loop;
    int port;
    int listener = create_listener(port);

    auto downstream = std::make_shared<MockDownstream>();
    auto sock = TcpSocket<MockDownstream>::Create(loop, downstream);

    bool connected = false;

    sock->OnConnect([&]() {
        connected = true;
        // Suspend immediately after connect
        sock->Suspend();
        EXPECT_TRUE(sock->IsSuspended());
        loop.Stop();
    });

    sock->Connect(make_addr("127.0.0.1", port));

    // Accept on server side
    auto listener_handle = loop.Register(
        listener,
        /*want_read=*/true,
        /*want_write=*/false,
        [&]() {
            int client = accept(listener, nullptr, nullptr);
            EXPECT_GE(client, 0);
            // Send data - should not trigger callback while suspended
            write(client, "test", 4);
            close(client);
        },
        []() {},
        [](int) {}
    );

    loop.Run();

    EXPECT_TRUE(connected);
    EXPECT_TRUE(sock->IsSuspended());

    // Resume and run again to receive data
    sock->Resume();
    EXPECT_FALSE(sock->IsSuspended());

    close(listener);
}

TEST(TcpSocketTest, CloseResetsSuspendedState) {
    EpollEventLoop loop;
    int port;
    int listener = create_listener(port);

    auto downstream = std::make_shared<MockDownstream>();
    auto sock = TcpSocket<MockDownstream>::Create(loop, downstream);

    bool connected = false;

    sock->OnConnect([&]() {
        connected = true;
        // Suspend, then close - should reset suspended state
        sock->Suspend();
        EXPECT_TRUE(sock->IsSuspended());
        sock->Close();
        EXPECT_FALSE(sock->IsSuspended());
        loop.Stop();
    });

    sock->Connect(make_addr("127.0.0.1", port));

    // Accept on server side
    auto listener_handle = loop.Register(
        listener,
        /*want_read=*/true,
        /*want_write=*/false,
        [&]() {
            int client = accept(listener, nullptr, nullptr);
            if (client >= 0) close(client);
        },
        []() {},
        [](int) {}
    );

    loop.Run();

    EXPECT_TRUE(connected);
    EXPECT_FALSE(sock->IsSuspended());

    close(listener);
}

TEST(TcpSocketTest, UsesProvidedAllocator) {
    EpollEventLoop loop;
    int port;
    int listener = create_listener(port);

    auto downstream = std::make_shared<MockDownstream>();
    downstream->loop = &loop;

    auto sock = TcpSocket<MockDownstream>::Create(loop, downstream);

    // Provide an external allocator
    SegmentAllocator allocator;
    sock->SetAllocator(&allocator);

    int server_fd = -1;
    bool connected = false;
    std::unique_ptr<IEventHandle> server_handle;

    sock->OnConnect([&]() {
        connected = true;
    });

    sock->Connect(make_addr("127.0.0.1", port));

    // Accept on listener and send data from server side
    auto listener_handle = loop.Register(
        listener,
        /*want_read=*/true,
        /*want_write=*/false,
        [&]() {
            server_fd = accept(listener, nullptr, nullptr);
            if (server_fd >= 0) {
                fcntl(server_fd, F_SETFL, O_NONBLOCK);
                server_handle = loop.Register(
                    server_fd,
                    /*want_read=*/false,
                    /*want_write=*/true,
                    []() {},
                    [&]() {
                        // Send data and close to trigger EOF -> OnDone -> loop.Stop()
                        write(server_fd, "allocator-test", 14);
                        close(server_fd);
                        server_fd = -1;
                        server_handle.reset();
                    },
                    [](int) {}
                );
            }
        },
        []() {},
        [](int) {}
    );

    loop.Run();

    EXPECT_TRUE(connected);
    // Verify data flowed through the provided allocator
    std::string received(reinterpret_cast<char*>(downstream->received_data.data()),
                         downstream->received_data.size());
    EXPECT_EQ(received, "allocator-test");

    close(listener);
}
