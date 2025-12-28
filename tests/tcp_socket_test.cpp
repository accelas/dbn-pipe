// tests/tcp_socket_test.cpp
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

#include "src/reactor.hpp"
#include "src/tcp_socket.hpp"

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
    Reactor reactor;
    int port;
    int listener = create_listener(port);

    TcpSocket sock(&reactor);

    bool connected = false;
    sock.OnConnect([&]() {
        connected = true;
        reactor.Stop();
    });

    sock.OnError([&](std::error_code ec) {
        ADD_FAILURE() << "Unexpected error: " << ec.message();
        reactor.Stop();
    });

    sock.Connect(make_addr("127.0.0.1", port));

    // Accept on server side
    reactor.Add(listener, EPOLLIN, [&](uint32_t) {
        int client = accept(listener, nullptr, nullptr);
        EXPECT_GE(client, 0);
        close(client);
    });

    reactor.Run();

    EXPECT_TRUE(connected);

    close(listener);
}

TEST(TcpSocketTest, ConnectFail) {
    Reactor reactor;

    TcpSocket sock(&reactor);

    bool got_error = false;
    sock.OnConnect([&]() {
        ADD_FAILURE() << "Connect should have failed";
        reactor.Stop();
    });

    sock.OnError([&](std::error_code ec) {
        EXPECT_TRUE(ec);
        got_error = true;
        reactor.Stop();
    });

    // Connect to port that's not listening
    sock.Connect(make_addr("127.0.0.1", 59999));

    reactor.Run();

    EXPECT_TRUE(got_error);
}

TEST(TcpSocketTest, ReadWrite) {
    Reactor reactor;
    int port;
    int listener = create_listener(port);

    TcpSocket sock(&reactor);
    int server_fd = -1;

    std::string received;

    sock.OnConnect([&]() {
        sock.Write(std::as_bytes(std::span{"hello", 5}));
    });

    sock.OnRead([&](std::span<const std::byte> data) {
        received.append(reinterpret_cast<const char*>(data.data()), data.size());
        if (received == "world") {
            reactor.Stop();
        }
    });

    sock.OnError([&](std::error_code ec) {
        ADD_FAILURE() << "Unexpected error: " << ec.message();
        reactor.Stop();
    });

    reactor.Add(listener, EPOLLIN, [&](uint32_t) {
        server_fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK);
        reactor.Remove(listener);

        // Echo server: read "hello", write "world"
        reactor.Add(server_fd, EPOLLIN, [&](uint32_t) {
            char buf[16];
            ssize_t n = read(server_fd, buf, sizeof(buf));
            if (n > 0) {
                write(server_fd, "world", 5);
            }
        });
    });

    sock.Connect(make_addr("127.0.0.1", port));
    reactor.Run();

    EXPECT_EQ(received, "world");

    if (server_fd >= 0) close(server_fd);
    close(listener);
}
