// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "lib/stream/epoll_event_loop.hpp"

using namespace dbn_pipe;

class EpollEventLoopTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create socketpair for testing
        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);
        read_fd_ = fds[0];
        write_fd_ = fds[1];
    }

    void TearDown() override {
        if (read_fd_ >= 0) close(read_fd_);
        if (write_fd_ >= 0) close(write_fd_);
    }

    int read_fd_ = -1;
    int write_fd_ = -1;
};

TEST_F(EpollEventLoopTest, RegisterAndReceiveReadEvent) {
    EpollEventLoop loop;

    bool read_called = false;
    bool write_called = false;
    bool error_called = false;

    auto handle = loop.Register(
        read_fd_,
        true,   // want_read
        false,  // want_write
        [&]() { read_called = true; },
        [&]() { write_called = true; },
        [&](int) { error_called = true; }
    );

    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(handle->fd(), read_fd_);

    // Write data to make read_fd_ readable
    const char msg[] = "hello";
    ASSERT_EQ(write(write_fd_, msg, sizeof(msg)), static_cast<ssize_t>(sizeof(msg)));

    // Poll should trigger read callback
    loop.Poll(100);

    EXPECT_TRUE(read_called);
    EXPECT_FALSE(write_called);
    EXPECT_FALSE(error_called);
}

TEST_F(EpollEventLoopTest, RegisterAndReceiveWriteEvent) {
    EpollEventLoop loop;

    bool read_called = false;
    bool write_called = false;
    bool error_called = false;

    auto handle = loop.Register(
        read_fd_,
        false,  // want_read
        true,   // want_write
        [&]() { read_called = true; },
        [&]() { write_called = true; },
        [&](int) { error_called = true; }
    );

    // Socket should be immediately writable
    loop.Poll(100);

    EXPECT_FALSE(read_called);
    EXPECT_TRUE(write_called);
    EXPECT_FALSE(error_called);
}

TEST_F(EpollEventLoopTest, HandleUpdate) {
    EpollEventLoop loop;

    bool read_called = false;
    bool write_called = false;

    auto handle = loop.Register(
        read_fd_,
        true,   // want_read only
        false,
        [&]() { read_called = true; },
        [&]() { write_called = true; },
        [&](int) {}
    );

    // Initially no data, no write interest - nothing should happen
    loop.Poll(0);
    EXPECT_FALSE(read_called);
    EXPECT_FALSE(write_called);

    // Update to also watch for write
    handle->Update(true, true);

    // Now should get write event (socket always writable)
    loop.Poll(100);
    EXPECT_TRUE(write_called);
}

TEST_F(EpollEventLoopTest, UnregisterOnHandleDestruction) {
    EpollEventLoop loop;

    bool read_called = false;

    {
        auto handle = loop.Register(
            read_fd_,
            true,
            false,
            [&]() { read_called = true; },
            [&]() {},
            [&](int) {}
        );

        // Write data
        const char msg[] = "test";
        write(write_fd_, msg, sizeof(msg));

        // Handle goes out of scope here - should unregister
    }

    // Poll after handle destroyed - callback should NOT be called
    loop.Poll(100);
    EXPECT_FALSE(read_called);
}

TEST_F(EpollEventLoopTest, DeferExecutesOnNextPoll) {
    EpollEventLoop loop;

    bool deferred_called = false;
    int call_order = 0;
    int deferred_order = 0;
    int read_order = 0;

    auto handle = loop.Register(
        read_fd_,
        true,
        false,
        [&]() {
            read_order = ++call_order;
        },
        [&]() {},
        [&](int) {}
    );

    loop.Defer([&]() {
        deferred_called = true;
        deferred_order = ++call_order;
    });

    // Write data to trigger read event
    const char msg[] = "x";
    write(write_fd_, msg, sizeof(msg));

    loop.Poll(100);

    EXPECT_TRUE(deferred_called);
    // Deferred callbacks should run before epoll events
    EXPECT_EQ(deferred_order, 1);
    EXPECT_EQ(read_order, 2);
}

TEST_F(EpollEventLoopTest, MultipleDeferredCallbacks) {
    EpollEventLoop loop;

    std::vector<int> order;

    loop.Defer([&]() { order.push_back(1); });
    loop.Defer([&]() { order.push_back(2); });
    loop.Defer([&]() { order.push_back(3); });

    loop.Poll(0);

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST_F(EpollEventLoopTest, IsInEventLoopThreadReturnsFalseInitially) {
    EpollEventLoop loop;

    // Before any Poll/Run, should return false from any thread
    EXPECT_FALSE(loop.IsInEventLoopThread());
}

TEST_F(EpollEventLoopTest, IsInEventLoopThreadReturnsTrueDuringPoll) {
    EpollEventLoop loop;

    bool in_loop_thread = false;

    loop.Defer([&]() {
        in_loop_thread = loop.IsInEventLoopThread();
    });

    loop.Poll(0);

    EXPECT_TRUE(in_loop_thread);
}

TEST_F(EpollEventLoopTest, IsInEventLoopThreadReturnsFalseFromOtherThread) {
    EpollEventLoop loop;

    std::atomic<bool> loop_started{false};
    std::atomic<bool> in_loop_thread{true};

    // Start event loop in background
    std::thread loop_thread([&]() {
        loop.Defer([&]() {
            loop_started = true;
        });
        // Use a short timeout loop that stops after a few iterations
        for (int i = 0; i < 10 && !loop_started.load(); ++i) {
            loop.Poll(10);
        }
        // Now loop_started is true and thread_id is set
        // Wait for main thread to check
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });

    // Wait for loop thread to start and set thread_id
    while (!loop_started.load()) {
        std::this_thread::yield();
    }

    // Check from this thread - should be false
    in_loop_thread = loop.IsInEventLoopThread();

    loop_thread.join();

    EXPECT_FALSE(in_loop_thread);
}

TEST_F(EpollEventLoopTest, RunAndStop) {
    EpollEventLoop loop;

    int poll_count = 0;

    // Use a recursive lambda pattern to defer multiple times
    std::function<void()> increment_and_maybe_stop;
    increment_and_maybe_stop = [&]() {
        poll_count++;
        if (poll_count >= 3) {
            loop.Stop();
        } else {
            loop.Defer(increment_and_maybe_stop);
        }
    };

    loop.Defer(increment_and_maybe_stop);

    loop.Run();

    EXPECT_GE(poll_count, 3);
}

TEST_F(EpollEventLoopTest, ErrorCallbackOnPeerClose) {
    EpollEventLoop loop;

    bool error_called = false;
    int error_code = 0;

    auto handle = loop.Register(
        read_fd_,
        true,
        false,
        [&]() {},
        [&]() {},
        [&](int err) {
            error_called = true;
            error_code = err;
        }
    );

    // Close write end to trigger error/hangup on read end
    close(write_fd_);
    write_fd_ = -1;

    loop.Poll(100);

    // Should get either EPOLLHUP or EPOLLERR
    EXPECT_TRUE(error_called);
}

TEST(EpollEventLoopStandaloneTest, Construction) {
    // Should not throw
    EpollEventLoop loop;
}

TEST(EpollEventLoopStandaloneTest, PollWithNoEvents) {
    EpollEventLoop loop;

    // Should not block and return cleanly
    loop.Poll(0);
}
