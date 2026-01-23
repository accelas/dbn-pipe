// SPDX-License-Identifier: MIT

// tests/reactor_test.cpp
#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include "lib/stream/reactor.hpp"

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

TEST(ReactorTest, EventAddRemove) {
    Reactor reactor;

    // Create an eventfd for testing
    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    bool called = false;
    {
        Event event(reactor, efd, EPOLLIN);
        event.OnEvent([&](uint32_t) { called = true; });

        // Write to make it readable
        uint64_t val = 1;
        write(efd, &val, sizeof(val));

        // Poll should trigger callback
        int n = reactor.Poll(0);
        EXPECT_EQ(n, 1);
        EXPECT_TRUE(called);

        // Event goes out of scope, removes itself
    }

    // Verify no more callbacks after Event is gone
    called = false;
    uint64_t val = 1;
    write(efd, &val, sizeof(val));
    int n = reactor.Poll(0);
    EXPECT_EQ(n, 0);
    EXPECT_FALSE(called);

    close(efd);
}

TEST(ReactorTest, EventModify) {
    Reactor reactor;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    int call_count = 0;
    Event event(reactor, efd, EPOLLIN);
    event.OnEvent([&](uint32_t /*events*/) {
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
    event.Modify(EPOLLIN | EPOLLOUT);
    reactor.Poll(0);
    EXPECT_EQ(call_count, 2);  // EPOLLOUT fires

    close(efd);
}

TEST(ReactorTest, RunStop) {
    Reactor reactor;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    Event event(reactor, efd, EPOLLIN);
    event.OnEvent([&](uint32_t) {
        reactor.Stop();
    });

    // Write to trigger stop
    uint64_t val = 1;
    write(efd, &val, sizeof(val));

    // Run should return after Stop() is called
    reactor.Run();

    close(efd);
}

TEST(ReactorTest, Timer) {
    Reactor reactor;

    Timer timer(reactor);

    int call_count = 0;
    timer.OnTimer([&]() {
        call_count++;
        if (call_count >= 3) {
            reactor.Stop();
        }
    });

    // Start repeating timer at 10ms intervals
    timer.Start(10, 10);
    EXPECT_TRUE(timer.IsArmed());

    reactor.Run();

    EXPECT_GE(call_count, 3);
    timer.Stop();
    EXPECT_FALSE(timer.IsArmed());
}

TEST(ReactorTest, TimerOneShot) {
    Reactor reactor;

    Timer timer(reactor);

    bool fired = false;
    timer.OnTimer([&]() {
        fired = true;
        reactor.Stop();
    });

    // One-shot timer at 10ms
    timer.Start(10);

    reactor.Run();

    EXPECT_TRUE(fired);
}

// Tests for IEventLoop interface implementation
TEST(ReactorTest, ImplementsIEventLoop) {
    Reactor reactor;
    // Reactor should be usable as IEventLoop&
    IEventLoop& loop = reactor;
    (void)loop;  // Suppress unused warning
}

TEST(ReactorTest, RegisterReadEvent) {
    Reactor reactor;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    bool read_called = false;
    auto handle = reactor.Register(
        efd, true, false,
        [&]() { read_called = true; },
        []() {},
        [](int) {});

    // Write to make readable
    uint64_t val = 1;
    write(efd, &val, sizeof(val));

    reactor.Poll(0);
    EXPECT_TRUE(read_called);

    close(efd);
}

TEST(ReactorTest, RegisterWriteEvent) {
    Reactor reactor;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    bool write_called = false;
    auto handle = reactor.Register(
        efd, false, true,
        []() {},
        [&]() { write_called = true; },
        [](int) {});

    // eventfd is always writable
    reactor.Poll(0);
    EXPECT_TRUE(write_called);

    close(efd);
}

TEST(ReactorTest, RegisterHandleUpdate) {
    Reactor reactor;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    int read_count = 0;
    int write_count = 0;
    auto handle = reactor.Register(
        efd, true, false,
        [&]() { read_count++; },
        [&]() { write_count++; },
        [](int) {});

    // Initially only watching read
    reactor.Poll(0);
    EXPECT_EQ(read_count, 0);  // Not readable yet
    EXPECT_EQ(write_count, 0);

    // Update to watch write
    handle->Update(false, true);
    reactor.Poll(0);
    EXPECT_EQ(write_count, 1);  // eventfd always writable

    close(efd);
}

TEST(ReactorTest, RegisterHandleUnregisterOnDestruction) {
    Reactor reactor;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    bool called = false;
    {
        auto handle = reactor.Register(
            efd, false, true,
            []() {},
            [&]() { called = true; },
            [](int) {});

        reactor.Poll(0);
        EXPECT_TRUE(called);
        called = false;
        // handle goes out of scope
    }

    // After handle destruction, no more callbacks
    reactor.Poll(0);
    EXPECT_FALSE(called);

    close(efd);
}

TEST(ReactorTest, DeferFromIEventLoop) {
    Reactor reactor;

    IEventLoop& loop = reactor;

    bool deferred_called = false;
    loop.Defer([&]() { deferred_called = true; });

    EXPECT_FALSE(deferred_called);
    reactor.Poll(0);
    EXPECT_TRUE(deferred_called);
}

TEST(ReactorTest, IsInEventLoopThread) {
    Reactor reactor;

    // Before Poll, no thread recorded yet, so returns false
    EXPECT_FALSE(reactor.IsInEventLoopThread());

    bool in_loop = false;
    reactor.Defer([&]() {
        in_loop = reactor.IsInEventLoopThread();
    });

    reactor.Poll(0);
    EXPECT_TRUE(in_loop);

    // After Poll from same thread, should still return true
    EXPECT_TRUE(reactor.IsInEventLoopThread());
}

TEST(ReactorTest, DeferThreadSafety) {
    Reactor reactor;

    constexpr int kNumThreads = 4;
    constexpr int kCallsPerThread = 1000;
    std::atomic<int> total_calls{0};

    // Start threads that will call Defer() concurrently
    // The internal wake mechanism should handle cross-thread wakeup
    std::vector<std::thread> threads;
    std::atomic<bool> start{false};
    std::atomic<int> threads_done{0};

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&]() {
            // Wait for signal to start
            while (!start.load()) {
                std::this_thread::yield();
            }

            // Rapidly call Defer() from this thread
            // Defer() internally calls Wake() for cross-thread calls
            for (int i = 0; i < kCallsPerThread; ++i) {
                reactor.Defer([&]() {
                    total_calls.fetch_add(1);
                });
            }

            threads_done.fetch_add(1);
        });
    }

    // Start all threads simultaneously
    start.store(true);

    // Poll until all threads done and all callbacks processed
    while (threads_done.load() < kNumThreads ||
           total_calls.load() < kNumThreads * kCallsPerThread) {
        reactor.Poll(10);  // 10ms timeout
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(total_calls.load(), kNumThreads * kCallsPerThread);
}

// Regression test for PR #46: Stop() called before Run() starts should not
// cause Run() to run forever. Previously, Run() unconditionally set running_
// to true, which would overwrite Stop()'s false value.
TEST(ReactorTest, StopBeforeRunDoesNotHang) {
    Reactor reactor;

    // Call Stop() before Run() starts
    reactor.Stop();

    // Run() should return immediately (not block forever)
    // This test will timeout if the bug is present
    std::thread runner([&]() {
        reactor.Run();
    });

    // If Run() hangs, this join will timeout
    // Give it 100ms max - Run() should exit nearly instantly
    auto start = std::chrono::steady_clock::now();
    runner.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Run() should have exited nearly instantly (< 50ms)
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 50);
}

// Regression test: Stop() called while Run() is starting up
TEST(ReactorTest, StopDuringRunStartup) {
    // Run this test multiple times to catch timing-dependent races
    for (int i = 0; i < 10; ++i) {
        Reactor reactor;

        std::atomic<bool> run_started{false};
        std::atomic<bool> run_exited{false};

        std::thread runner([&]() {
            run_started.store(true);
            reactor.Run();
            run_exited.store(true);
        });

        // Call Stop() as soon as possible - may race with Run()'s startup
        while (!run_started.load()) {
            std::this_thread::yield();
        }
        reactor.Stop();

        // Wait for Run() to exit with timeout
        auto start = std::chrono::steady_clock::now();
        while (!run_exited.load()) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 100) {
                FAIL() << "Run() did not exit within 100ms after Stop()";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        runner.join();
    }
}
