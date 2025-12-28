// tests/reactor_test.cpp
#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include "src/reactor.hpp"

using namespace databento_async;

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
    reactor.Add(efd, EPOLLIN, [&](uint32_t /*events*/) {
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
