// tests/reactor_test.cpp
#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include "src/reactor.hpp"

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
