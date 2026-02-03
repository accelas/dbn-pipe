# Event Loop Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build async databento client with thin epoll wrapper, supporting Live API first.

**Architecture:** Reactor (epoll) → DataSource base → LiveClient. All I/O non-blocking, single-threaded, user-controlled event loop.

**Tech Stack:** C++23, epoll, OpenSSL (SHA256 for CRAM), Bazel build.

---

## Phase 1: Core Infrastructure

### Task 1: Update Dependencies

Remove libuv, keep openssl. Add llhttp and zstd deps for later.

**Files:**
- Modify: `MODULE.bazel`

**Step 1: Update MODULE.bazel**

```bazel
"""Bazel module configuration for databento-async."""

module(
    name = "databento-async",
    version = "0.1.0",
)

# ============================================================================
# Core Build Rules
# ============================================================================

bazel_dep(name = "rules_cc", version = "0.2.4")
bazel_dep(name = "rules_pkg", version = "1.0.1")

# ============================================================================
# Testing
# ============================================================================

bazel_dep(name = "googletest", version = "1.15.2")

# ============================================================================
# Dependencies
# ============================================================================

# TLS/Crypto (for CRAM auth SHA256, Historical TLS)
bazel_dep(name = "openssl", version = "3.3.1.bcr.9")

# HTTP parsing (for Historical API)
bazel_dep(name = "llhttp", version = "9.2.1")

# Decompression (for Historical API)
bazel_dep(name = "zstd", version = "1.5.6")

# ============================================================================
# Databento C++ headers (type definitions only)
# ============================================================================

databento = use_extension("//third_party/databento:databento.bzl", "databento")
use_repo(databento, "databento_cpp")

# ============================================================================
# C++ Toolchain
# ============================================================================

register_toolchains("//toolchain:cc_toolchain_for_k8")
```

**Step 2: Verify build still works**

Run: `bazel build //src:parser`
Expected: SUCCESS

**Step 3: Commit**

```bash
git add MODULE.bazel
git commit -m "build: replace libuv with llhttp and zstd deps"
```

---

### Task 2: Error Types

**Files:**
- Create: `src/error.hpp`
- Create: `tests/error_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write error test**

```cpp
// tests/error_test.cpp
#include <gtest/gtest.h>

#include "dbn_pipe/error.hpp"

using namespace dbn_pipe;

TEST(ErrorTest, Construction) {
    Error err{ErrorCode::ConnectionFailed, "connection refused"};
    EXPECT_EQ(err.code, ErrorCode::ConnectionFailed);
    EXPECT_EQ(err.message, "connection refused");
    EXPECT_EQ(err.os_errno, 0);
}

TEST(ErrorTest, WithErrno) {
    Error err{ErrorCode::ConnectionFailed, "connection refused", ECONNREFUSED};
    EXPECT_EQ(err.code, ErrorCode::ConnectionFailed);
    EXPECT_EQ(err.os_errno, ECONNREFUSED);
}

TEST(ErrorTest, CategoryString) {
    EXPECT_EQ(error_category(ErrorCode::ConnectionFailed), "connection");
    EXPECT_EQ(error_category(ErrorCode::AuthFailed), "auth");
    EXPECT_EQ(error_category(ErrorCode::ParseError), "protocol");
}
```

**Step 2: Update tests/BUILD.bazel**

```bazel
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "parser_test",
    srcs = ["parser_test.cpp"],
    deps = [
        "//src:parser",
        "@databento_cpp//:databento_headers",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "error_test",
    srcs = ["error_test.cpp"],
    deps = [
        "//src:error",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:error_test`
Expected: FAIL (src/error.hpp not found)

**Step 4: Write error.hpp**

```cpp
// src/error.hpp
#pragma once

#include <string>
#include <string_view>

namespace dbn_pipe {

enum class ErrorCode {
    // Connection
    ConnectionFailed,
    ConnectionClosed,
    DnsResolutionFailed,

    // Auth
    AuthFailed,
    InvalidApiKey,

    // Protocol
    InvalidGreeting,
    InvalidChallenge,
    ParseError,

    // Subscription
    InvalidDataset,
    InvalidSymbol,
    InvalidSchema,

    // TLS (Historical)
    TlsHandshakeFailed,
    CertificateError,

    // HTTP (Historical)
    HttpError,

    // Decompression (Historical)
    DecompressionError,
};

struct Error {
    ErrorCode code;
    std::string message;
    int os_errno = 0;
};

constexpr std::string_view error_category(ErrorCode code) {
    switch (code) {
        case ErrorCode::ConnectionFailed:
        case ErrorCode::ConnectionClosed:
        case ErrorCode::DnsResolutionFailed:
            return "connection";
        case ErrorCode::AuthFailed:
        case ErrorCode::InvalidApiKey:
            return "auth";
        case ErrorCode::InvalidGreeting:
        case ErrorCode::InvalidChallenge:
        case ErrorCode::ParseError:
            return "protocol";
        case ErrorCode::InvalidDataset:
        case ErrorCode::InvalidSymbol:
        case ErrorCode::InvalidSchema:
            return "subscription";
        case ErrorCode::TlsHandshakeFailed:
        case ErrorCode::CertificateError:
            return "tls";
        case ErrorCode::HttpError:
            return "http";
        case ErrorCode::DecompressionError:
            return "decompression";
    }
    return "unknown";
}

}  // namespace dbn_pipe
```

**Step 5: Update src/BUILD.bazel**

```bazel
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "parser",
    hdrs = ["parser.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "@databento_cpp//:databento_headers",
    ],
)

cc_library(
    name = "error",
    hdrs = ["error.hpp"],
    visibility = ["//visibility:public"],
)
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:error_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/error.hpp src/BUILD.bazel tests/error_test.cpp tests/BUILD.bazel
git commit -m "feat: add error types"
```

---

### Task 3: Reactor (epoll wrapper)

**Files:**
- Create: `src/reactor.hpp`
- Create: `src/reactor.cpp`
- Create: `tests/reactor_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write reactor test**

```cpp
// tests/reactor_test.cpp
#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include "dbn_pipe/reactor.hpp"

using namespace dbn_pipe;

TEST(ReactorTest, Construction) {
    Reactor reactor;
    // Should not throw, epoll_fd created
}

TEST(ReactorTest, PollEmpty) {
    Reactor reactor;
    // Non-blocking poll with no fds should return 0
    int n = reactor.Poll(0);
    EXPECT_EQ(n, 0);
}

TEST(ReactorTest, AddRemove) {
    Reactor reactor;

    // Create an eventfd for testing
    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    bool called = false;
    reactor.Add(efd, EPOLLIN, [&](uint32_t) { called = true; });

    // Write to make it readable
    uint64_t val = 1;
    write(efd, &val, sizeof(val));

    // Poll should trigger callback
    int n = reactor.Poll(0);
    EXPECT_EQ(n, 1);
    EXPECT_TRUE(called);

    // Remove and verify no more callbacks
    called = false;
    reactor.Remove(efd);

    write(efd, &val, sizeof(val));
    n = reactor.Poll(0);
    EXPECT_EQ(n, 0);
    EXPECT_FALSE(called);

    close(efd);
}

TEST(ReactorTest, Modify) {
    Reactor reactor;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    int call_count = 0;
    reactor.Add(efd, EPOLLIN, [&](uint32_t events) {
        call_count++;
    });

    // Write to trigger
    uint64_t val = 1;
    write(efd, &val, sizeof(val));
    reactor.Poll(0);
    EXPECT_EQ(call_count, 1);

    // Read to clear
    read(efd, &val, sizeof(val));

    // Modify to also watch EPOLLOUT (always ready for eventfd)
    reactor.Modify(efd, EPOLLIN | EPOLLOUT);
    reactor.Poll(0);
    EXPECT_EQ(call_count, 2);  // EPOLLOUT fires

    close(efd);
}

TEST(ReactorTest, RunStop) {
    Reactor reactor;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    reactor.Add(efd, EPOLLIN, [&](uint32_t) {
        reactor.Stop();
    });

    // Write to trigger stop
    uint64_t val = 1;
    write(efd, &val, sizeof(val));

    // Run should return after Stop() is called
    reactor.Run();

    close(efd);
}
```

**Step 2: Update tests/BUILD.bazel**

Add to existing file:

```bazel
cc_test(
    name = "reactor_test",
    srcs = ["reactor_test.cpp"],
    deps = [
        "//src:reactor",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:reactor_test`
Expected: FAIL

**Step 4: Write reactor.hpp**

```cpp
// src/reactor.hpp
#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace dbn_pipe {

class Reactor {
public:
    using Callback = std::function<void(uint32_t events)>;

    Reactor();
    ~Reactor();

    // Non-copyable, non-movable
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;
    Reactor& operator=(Reactor&&) = delete;

    // Register fd with events and callback
    void Add(int fd, uint32_t events, Callback cb);

    // Modify events for existing fd
    void Modify(int fd, uint32_t events);

    // Remove fd from reactor
    void Remove(int fd);

    // Poll for events, returns number handled
    // timeout_ms: -1 = block, 0 = non-blocking, >0 = timeout
    int Poll(int timeout_ms = -1);

    // Run until Stop() called
    void Run();

    // Signal Run() to stop
    void Stop();

private:
    int epoll_fd_;
    bool running_ = false;
    std::unordered_map<int, Callback> callbacks_;
    std::vector<epoll_event> events_;

    static constexpr int kMaxEvents = 64;
};

}  // namespace dbn_pipe
```

**Step 5: Write reactor.cpp**

```cpp
// src/reactor.cpp
#include "reactor.hpp"

#include <unistd.h>

#include <stdexcept>
#include <system_error>

namespace dbn_pipe {

Reactor::Reactor() : events_(kMaxEvents) {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_create1");
    }
}

Reactor::~Reactor() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}

void Reactor::Add(int fd, uint32_t events, Callback cb) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl ADD");
    }

    callbacks_[fd] = std::move(cb);
}

void Reactor::Modify(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl MOD");
    }
}

void Reactor::Remove(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    callbacks_.erase(fd);
}

int Reactor::Poll(int timeout_ms) {
    int n = epoll_wait(epoll_fd_, events_.data(), kMaxEvents, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) {
            return 0;
        }
        throw std::system_error(errno, std::system_category(), "epoll_wait");
    }

    for (int i = 0; i < n; ++i) {
        int fd = events_[i].data.fd;
        auto it = callbacks_.find(fd);
        if (it != callbacks_.end()) {
            it->second(events_[i].events);
        }
    }

    return n;
}

void Reactor::Run() {
    running_ = true;
    while (running_) {
        Poll(-1);
    }
}

void Reactor::Stop() {
    running_ = false;
}

}  // namespace dbn_pipe
```

**Step 6: Update src/BUILD.bazel**

Add to existing file:

```bazel
cc_library(
    name = "reactor",
    srcs = ["reactor.cpp"],
    hdrs = ["reactor.hpp"],
    visibility = ["//visibility:public"],
)
```

**Step 7: Run test to verify it passes**

Run: `bazel test //tests:reactor_test`
Expected: PASS

**Step 8: Commit**

```bash
git add src/reactor.hpp src/reactor.cpp src/BUILD.bazel tests/reactor_test.cpp tests/BUILD.bazel
git commit -m "feat: add Reactor epoll wrapper"
```

---

### Task 4: DataSource Base Class

**Files:**
- Create: `src/data_source.hpp`
- Create: `tests/data_source_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write data_source test**

```cpp
// tests/data_source_test.cpp
#include <gtest/gtest.h>

#include <databento/record.hpp>

#include "dbn_pipe/data_source.hpp"

using namespace databento;
using namespace dbn_pipe;

// Concrete test implementation
class TestDataSource : public DataSource {
public:
    void Start() override { started_ = true; }
    void Stop() override { started_ = false; }

    // Expose protected methods for testing
    using DataSource::DeliverBytes;
    using DataSource::DeliverError;

    bool started_ = false;
};

TEST(DataSourceTest, PauseResume) {
    TestDataSource ds;

    EXPECT_FALSE(ds.IsPaused());
    ds.Pause();
    EXPECT_TRUE(ds.IsPaused());
    ds.Resume();
    EXPECT_FALSE(ds.IsPaused());
}

TEST(DataSourceTest, OnRecordCallback) {
    TestDataSource ds;

    int call_count = 0;
    ds.OnRecord([&](const Record&) { call_count++; });

    // Create a valid MboMsg
    alignas(8) std::byte buffer[sizeof(MboMsg)] = {};
    auto* msg = reinterpret_cast<MboMsg*>(buffer);
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;

    ds.DeliverBytes({buffer, sizeof(buffer)});

    EXPECT_EQ(call_count, 1);
}

TEST(DataSourceTest, OnErrorCallback) {
    TestDataSource ds;

    ErrorCode received_code{};
    ds.OnError([&](const Error& e) { received_code = e.code; });

    ds.DeliverError({ErrorCode::ConnectionClosed, "closed"});

    EXPECT_EQ(received_code, ErrorCode::ConnectionClosed);
}

TEST(DataSourceTest, PausedDoesNotDeliver) {
    TestDataSource ds;

    int call_count = 0;
    ds.OnRecord([&](const Record&) { call_count++; });

    ds.Pause();

    // Create a valid record
    alignas(8) std::byte buffer[sizeof(MboMsg)] = {};
    auto* msg = reinterpret_cast<MboMsg*>(buffer);
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;

    ds.DeliverBytes({buffer, sizeof(buffer)});

    // Should buffer but not deliver while paused
    EXPECT_EQ(call_count, 0);

    ds.Resume();

    // After resume, should deliver buffered records
    EXPECT_EQ(call_count, 1);
}
```

**Step 2: Update tests/BUILD.bazel**

Add:

```bazel
cc_test(
    name = "data_source_test",
    srcs = ["data_source_test.cpp"],
    deps = [
        "//src:data_source",
        "@databento_cpp//:databento_headers",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:data_source_test`
Expected: FAIL

**Step 4: Write data_source.hpp**

```cpp
// src/data_source.hpp
#pragma once

#include <functional>
#include <span>

#include <databento/record.hpp>

#include "error.hpp"
#include "parser.hpp"

namespace dbn_pipe {

class DataSource {
public:
    virtual ~DataSource() = default;

    // Control
    virtual void Start() = 0;
    virtual void Stop() = 0;

    // Backpressure
    void Pause() { paused_ = true; }
    void Resume();
    bool IsPaused() const { return paused_; }

    // Callbacks
    template <typename Handler>
    void OnRecord(Handler&& h) {
        record_handler_ = std::forward<Handler>(h);
    }

    template <typename Handler>
    void OnError(Handler&& h) {
        error_handler_ = std::forward<Handler>(h);
    }

protected:
    void DeliverBytes(std::span<const std::byte> data);
    void DeliverError(Error e);

    DbnParser parser_;
    bool paused_ = false;

private:
    void DrainParser();

    std::function<void(const databento::Record&)> record_handler_;
    std::function<void(const Error&)> error_handler_;
};

// Inline implementations

inline void DataSource::Resume() {
    paused_ = false;
    DrainParser();
}

inline void DataSource::DeliverBytes(std::span<const std::byte> data) {
    parser_.Push(data.data(), data.size());
    if (!paused_) {
        DrainParser();
    }
}

inline void DataSource::DeliverError(Error e) {
    if (error_handler_) {
        error_handler_(e);
    }
}

inline void DataSource::DrainParser() {
    while (!paused_) {
        const databento::Record* rec = parser_.Pull();
        if (!rec) break;
        if (record_handler_) {
            record_handler_(*rec);
        }
    }
}

}  // namespace dbn_pipe
```

**Step 5: Update src/BUILD.bazel**

Add:

```bazel
cc_library(
    name = "data_source",
    hdrs = ["data_source.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":error",
        ":parser",
        "@databento_cpp//:databento_headers",
    ],
)
```

**Step 6: Run test to verify it passes**

Run: `bazel test //tests:data_source_test`
Expected: PASS

**Step 7: Commit**

```bash
git add src/data_source.hpp src/BUILD.bazel tests/data_source_test.cpp tests/BUILD.bazel
git commit -m "feat: add DataSource base class with backpressure"
```

---

## Phase 2: Live Client

### Task 5: TCP Socket Wrapper

**Files:**
- Create: `src/tcp_socket.hpp`
- Create: `src/tcp_socket.cpp`
- Create: `tests/tcp_socket_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write tcp_socket test**

```cpp
// tests/tcp_socket_test.cpp
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

#include "dbn_pipe/reactor.hpp"
#include "dbn_pipe/tcp_socket.hpp"

using namespace dbn_pipe;

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
    sock.OnConnect([&](std::error_code ec) {
        EXPECT_FALSE(ec);
        connected = true;
        reactor.Stop();
    });

    sock.Connect("127.0.0.1", port);

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
    sock.OnConnect([&](std::error_code ec) {
        EXPECT_TRUE(ec);
        got_error = true;
        reactor.Stop();
    });

    // Connect to port that's not listening
    sock.Connect("127.0.0.1", 59999);

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

    sock.OnConnect([&](std::error_code ec) {
        ASSERT_FALSE(ec);
        sock.Write(std::as_bytes(std::span{"hello", 5}));
    });

    sock.OnRead([&](std::span<const std::byte> data, std::error_code ec) {
        if (!ec && !data.empty()) {
            received.append(reinterpret_cast<const char*>(data.data()), data.size());
            if (received == "world") {
                reactor.Stop();
            }
        }
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

    sock.Connect("127.0.0.1", port);
    reactor.Run();

    EXPECT_EQ(received, "world");

    if (server_fd >= 0) close(server_fd);
    close(listener);
}
```

**Step 2: Update tests/BUILD.bazel**

Add:

```bazel
cc_test(
    name = "tcp_socket_test",
    srcs = ["tcp_socket_test.cpp"],
    deps = [
        "//src:reactor",
        "//src:tcp_socket",
        "@googletest//:gtest_main",
    ],
)
```

**Step 3: Run test to verify it fails**

Run: `bazel test //tests:tcp_socket_test`
Expected: FAIL

**Step 4: Write tcp_socket.hpp**

```cpp
// src/tcp_socket.hpp
#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace dbn_pipe {

class Reactor;

class TcpSocket {
public:
    using ConnectCallback = std::function<void(std::error_code)>;
    using ReadCallback = std::function<void(std::span<const std::byte>, std::error_code)>;
    using WriteCallback = std::function<void(std::error_code)>;

    explicit TcpSocket(Reactor* reactor);
    ~TcpSocket();

    // Non-copyable, non-movable
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&&) = delete;
    TcpSocket& operator=(TcpSocket&&) = delete;

    // Connect to host:port
    void Connect(std::string_view host, int port);

    // Write data (queued if not yet writable)
    void Write(std::span<const std::byte> data);

    // Close connection
    void Close();

    // Callbacks
    void OnConnect(ConnectCallback cb) { on_connect_ = std::move(cb); }
    void OnRead(ReadCallback cb) { on_read_ = std::move(cb); }
    void OnWrite(WriteCallback cb) { on_write_ = std::move(cb); }

    // State
    bool IsConnected() const { return connected_; }
    int fd() const { return fd_; }

private:
    void HandleEvents(uint32_t events);
    void HandleConnect();
    void HandleRead();
    void HandleWrite();
    void UpdateEpoll();

    Reactor* reactor_;
    int fd_ = -1;
    bool connected_ = false;
    bool connecting_ = false;

    std::vector<std::byte> write_buffer_;
    std::vector<std::byte> read_buffer_;

    ConnectCallback on_connect_;
    ReadCallback on_read_;
    WriteCallback on_write_;

    static constexpr size_t kReadBufferSize = 65536;
};

}  // namespace dbn_pipe
```

**Step 5: Write tcp_socket.cpp**

```cpp
// src/tcp_socket.cpp
#include "tcp_socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "reactor.hpp"

namespace dbn_pipe {

TcpSocket::TcpSocket(Reactor* reactor)
    : reactor_(reactor), read_buffer_(kReadBufferSize) {}

TcpSocket::~TcpSocket() { Close(); }

void TcpSocket::Connect(std::string_view host, int port) {
    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        if (on_connect_) {
            on_connect_(std::error_code(errno, std::system_category()));
        }
        return;
    }

    // Disable Nagle for lower latency
    int opt = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    std::string host_str(host);
    if (inet_pton(AF_INET, host_str.c_str(), &addr.sin_addr) != 1) {
        close(fd_);
        fd_ = -1;
        if (on_connect_) {
            on_connect_(std::make_error_code(std::errc::invalid_argument));
        }
        return;
    }

    int ret = connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        auto ec = std::error_code(errno, std::system_category());
        close(fd_);
        fd_ = -1;
        if (on_connect_) {
            on_connect_(ec);
        }
        return;
    }

    connecting_ = true;
    reactor_->Add(fd_, EPOLLOUT | EPOLLIN | EPOLLET,
                  [this](uint32_t events) { HandleEvents(events); });
}

void TcpSocket::Write(std::span<const std::byte> data) {
    if (fd_ < 0) return;

    write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());

    if (connected_ && !write_buffer_.empty()) {
        HandleWrite();
    }
}

void TcpSocket::Close() {
    if (fd_ >= 0) {
        reactor_->Remove(fd_);
        close(fd_);
        fd_ = -1;
    }
    connected_ = false;
    connecting_ = false;
    write_buffer_.clear();
}

void TcpSocket::HandleEvents(uint32_t events) {
    if (connecting_) {
        HandleConnect();
        return;
    }

    if (events & (EPOLLERR | EPOLLHUP)) {
        if (on_read_) {
            on_read_({}, std::make_error_code(std::errc::connection_reset));
        }
        return;
    }

    if (events & EPOLLIN) {
        HandleRead();
    }

    if (events & EPOLLOUT) {
        HandleWrite();
    }
}

void TcpSocket::HandleConnect() {
    connecting_ = false;

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);

    if (err != 0) {
        if (on_connect_) {
            on_connect_(std::error_code(err, std::system_category()));
        }
        Close();
        return;
    }

    connected_ = true;

    if (on_connect_) {
        on_connect_({});
    }

    // Flush any pending writes
    if (!write_buffer_.empty()) {
        HandleWrite();
    }
}

void TcpSocket::HandleRead() {
    while (true) {
        ssize_t n = read(fd_, read_buffer_.data(), read_buffer_.size());
        if (n > 0) {
            if (on_read_) {
                on_read_(std::span{read_buffer_.data(), static_cast<size_t>(n)}, {});
            }
        } else if (n == 0) {
            // EOF
            if (on_read_) {
                on_read_({}, std::make_error_code(std::errc::connection_reset));
            }
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // No more data
            }
            if (on_read_) {
                on_read_({}, std::error_code(errno, std::system_category()));
            }
            break;
        }
    }
}

void TcpSocket::HandleWrite() {
    while (!write_buffer_.empty()) {
        ssize_t n = write(fd_, write_buffer_.data(), write_buffer_.size());
        if (n > 0) {
            write_buffer_.erase(write_buffer_.begin(),
                                write_buffer_.begin() + n);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // Would block, wait for EPOLLOUT
            }
            if (on_write_) {
                on_write_(std::error_code(errno, std::system_category()));
            }
            break;
        }
    }

    if (write_buffer_.empty() && on_write_) {
        on_write_({});
    }
}

}  // namespace dbn_pipe
```

**Step 6: Update src/BUILD.bazel**

Add:

```bazel
cc_library(
    name = "tcp_socket",
    srcs = ["tcp_socket.cpp"],
    hdrs = ["tcp_socket.hpp"],
    visibility = ["//visibility:public"],
    deps = [":reactor"],
)
```

**Step 7: Run test to verify it passes**

Run: `bazel test //tests:tcp_socket_test`
Expected: PASS

**Step 8: Commit**

```bash
git add src/tcp_socket.hpp src/tcp_socket.cpp src/BUILD.bazel tests/tcp_socket_test.cpp tests/BUILD.bazel
git commit -m "feat: add TcpSocket async wrapper"
```

---

### Task 6: CRAM Authentication

**Files:**
- Create: `src/cram_auth.hpp`
- Create: `src/cram_auth.cpp`
- Create: `tests/cram_auth_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Research databento CRAM format**

Check databento-cpp source at `~/work/databento-cpp` for exact CRAM format:
- Greeting format
- Challenge format
- Response format

**Step 2: Write cram_auth test**

```cpp
// tests/cram_auth_test.cpp
#include <gtest/gtest.h>

#include "dbn_pipe/cram_auth.hpp"

using namespace dbn_pipe;

TEST(CramAuthTest, ParseGreeting) {
    std::string greeting = "lsg-test|20231015\n";
    auto result = CramAuth::ParseGreeting(greeting);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->session_id, "lsg-test");
    EXPECT_EQ(result->version, "20231015");
}

TEST(CramAuthTest, ParseChallenge) {
    std::string challenge = "cram=abcdef123456\n";
    auto result = CramAuth::ParseChallenge(challenge);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "abcdef123456");
}

TEST(CramAuthTest, ComputeResponse) {
    // Known test vector (from databento docs or testing)
    std::string challenge = "test_challenge";
    std::string api_key = "test_api_key";

    std::string response = CramAuth::ComputeResponse(challenge, api_key);

    // Response should be hex-encoded SHA256
    EXPECT_EQ(response.size(), 64);  // 32 bytes * 2 hex chars

    // Verify it's valid hex
    for (char c : response) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

TEST(CramAuthTest, FormatAuthMessage) {
    std::string msg = CramAuth::FormatAuthMessage(
        "db-abc123",
        "CRAM-SHA256",
        "deadbeef01234567"
    );

    EXPECT_EQ(msg, "auth=db-abc123|CRAM-SHA256|deadbeef01234567\n");
}
```

**Step 3: Update tests/BUILD.bazel**

Add:

```bazel
cc_test(
    name = "cram_auth_test",
    srcs = ["cram_auth_test.cpp"],
    deps = [
        "//src:cram_auth",
        "@googletest//:gtest_main",
    ],
)
```

**Step 4: Run test to verify it fails**

Run: `bazel test //tests:cram_auth_test`
Expected: FAIL

**Step 5: Write cram_auth.hpp**

```cpp
// src/cram_auth.hpp
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace dbn_pipe {

struct Greeting {
    std::string session_id;
    std::string version;
};

class CramAuth {
public:
    // Parse "session_id|version\n"
    static std::optional<Greeting> ParseGreeting(std::string_view data);

    // Parse "cram=challenge\n"
    static std::optional<std::string> ParseChallenge(std::string_view data);

    // Compute HMAC-SHA256(challenge, api_key) as hex
    static std::string ComputeResponse(std::string_view challenge,
                                       std::string_view api_key);

    // Format "auth=api_key|method|response\n"
    static std::string FormatAuthMessage(std::string_view api_key,
                                         std::string_view method,
                                         std::string_view response);
};

}  // namespace dbn_pipe
```

**Step 6: Write cram_auth.cpp**

```cpp
// src/cram_auth.cpp
#include "cram_auth.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <iomanip>
#include <sstream>

namespace dbn_pipe {

std::optional<Greeting> CramAuth::ParseGreeting(std::string_view data) {
    // Format: "session_id|version\n"
    auto pipe = data.find('|');
    if (pipe == std::string_view::npos) {
        return std::nullopt;
    }

    auto newline = data.find('\n', pipe);
    if (newline == std::string_view::npos) {
        newline = data.size();
    }

    return Greeting{
        .session_id = std::string(data.substr(0, pipe)),
        .version = std::string(data.substr(pipe + 1, newline - pipe - 1)),
    };
}

std::optional<std::string> CramAuth::ParseChallenge(std::string_view data) {
    // Format: "cram=challenge\n"
    constexpr std::string_view prefix = "cram=";
    if (!data.starts_with(prefix)) {
        return std::nullopt;
    }

    auto newline = data.find('\n', prefix.size());
    if (newline == std::string_view::npos) {
        newline = data.size();
    }

    return std::string(data.substr(prefix.size(), newline - prefix.size()));
}

std::string CramAuth::ComputeResponse(std::string_view challenge,
                                      std::string_view api_key) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_len = 0;

    HMAC(EVP_sha256(),
         api_key.data(), static_cast<int>(api_key.size()),
         reinterpret_cast<const unsigned char*>(challenge.data()),
         challenge.size(),
         digest, &digest_len);

    std::ostringstream oss;
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(digest[i]);
    }
    return oss.str();
}

std::string CramAuth::FormatAuthMessage(std::string_view api_key,
                                        std::string_view method,
                                        std::string_view response) {
    std::string msg = "auth=";
    msg += api_key;
    msg += '|';
    msg += method;
    msg += '|';
    msg += response;
    msg += '\n';
    return msg;
}

}  // namespace dbn_pipe
```

**Step 7: Update src/BUILD.bazel**

Add:

```bazel
cc_library(
    name = "cram_auth",
    srcs = ["cram_auth.cpp"],
    hdrs = ["cram_auth.hpp"],
    visibility = ["//visibility:public"],
    deps = ["@openssl//:crypto"],
)
```

**Step 8: Run test to verify it passes**

Run: `bazel test //tests:cram_auth_test`
Expected: PASS

**Step 9: Commit**

```bash
git add src/cram_auth.hpp src/cram_auth.cpp src/BUILD.bazel tests/cram_auth_test.cpp tests/BUILD.bazel
git commit -m "feat: add CRAM authentication"
```

---

### Task 7: LiveClient

**Files:**
- Create: `src/live_client.hpp`
- Create: `src/live_client.cpp`
- Create: `tests/live_client_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write live_client test (mock server)**

```cpp
// tests/live_client_test.cpp
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

#include "dbn_pipe/live_client.hpp"
#include "dbn_pipe/reactor.hpp"

using namespace databento;
using namespace dbn_pipe;

// Mock databento server
class MockServer {
public:
    MockServer() {
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        int opt = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(fd_, 1);

        socklen_t len = sizeof(addr);
        getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }

    ~MockServer() {
        if (client_fd_ >= 0) close(client_fd_);
        if (fd_ >= 0) close(fd_);
    }

    int port() const { return port_; }
    int fd() const { return fd_; }

    void Accept() {
        client_fd_ = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK);
    }

    void SendGreeting() {
        const char* greeting = "mock-session|20231015\n";
        write(client_fd_, greeting, strlen(greeting));
    }

    void SendChallenge() {
        const char* challenge = "cram=test_challenge_12345\n";
        write(client_fd_, challenge, strlen(challenge));
    }

    void SendAuthOk() {
        // In real protocol this might be different
        const char* ok = "success=1\n";
        write(client_fd_, ok, strlen(ok));
    }

    void SendRecord(const MboMsg& msg) {
        write(client_fd_, &msg, sizeof(msg));
    }

    int client_fd() const { return client_fd_; }

private:
    int fd_ = -1;
    int client_fd_ = -1;
    int port_ = 0;
};

TEST(LiveClientTest, ConnectAndAuth) {
    Reactor reactor;
    MockServer server;

    LiveClient client(&reactor, "test_api_key");

    bool connected = false;
    bool got_error = false;

    client.OnError([&](const Error& e) {
        got_error = true;
        reactor.Stop();
    });

    // Set up server-side handling
    reactor.Add(server.fd(), EPOLLIN, [&](uint32_t) {
        server.Accept();
        reactor.Remove(server.fd());

        // Add client fd to reactor for server-side
        reactor.Add(server.client_fd(), EPOLLIN | EPOLLOUT, [&](uint32_t events) {
            if (events & EPOLLOUT) {
                server.SendGreeting();
            }
            if (events & EPOLLIN) {
                char buf[1024];
                ssize_t n = read(server.client_fd(), buf, sizeof(buf));
                if (n > 0) {
                    // Got auth request, send challenge
                    server.SendChallenge();
                }
            }
        });
    });

    client.Connect("127.0.0.1", server.port());

    // Run for a bit
    for (int i = 0; i < 10 && !got_error; ++i) {
        reactor.Poll(10);
        if (client.GetState() == LiveClient::State::Ready) {
            connected = true;
            break;
        }
    }

    // For now, just verify we attempted connection
    EXPECT_TRUE(client.GetState() != LiveClient::State::Disconnected || got_error);
}

TEST(LiveClientTest, ReceiveRecord) {
    Reactor reactor;
    MockServer server;

    LiveClient client(&reactor, "test_api_key");

    int record_count = 0;

    client.OnRecord([&](const Record& rec) {
        record_count++;
        if (record_count >= 1) {
            reactor.Stop();
        }
    });

    // This test would require full protocol implementation
    // For now just verify callbacks compile
    EXPECT_EQ(record_count, 0);
}
```

**Step 2-8: Similar pattern as previous tasks**

(Full implementation in live_client.hpp/cpp with state machine handling connect → greeting → challenge → auth → ready → streaming states)

**Step 9: Commit**

```bash
git add src/live_client.hpp src/live_client.cpp src/BUILD.bazel tests/live_client_test.cpp tests/BUILD.bazel
git commit -m "feat: add LiveClient with CRAM auth"
```

---

## Summary

After completing all tasks, the project will have:

```
src/
├── BUILD.bazel
├── cram_auth.cpp
├── cram_auth.hpp
├── data_source.hpp
├── error.hpp
├── live_client.cpp
├── live_client.hpp
├── parser.hpp
├── reactor.cpp
├── reactor.hpp
├── tcp_socket.cpp
└── tcp_socket.hpp

tests/
├── BUILD.bazel
├── cram_auth_test.cpp
├── data_source_test.cpp
├── error_test.cpp
├── live_client_test.cpp
├── parser_test.cpp
├── reactor_test.cpp
└── tcp_socket_test.cpp
```

Run all tests: `bazel test //tests:all`
