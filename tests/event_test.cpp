// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "lib/stream/epoll_event_loop.hpp"
#include "lib/stream/event.hpp"

using namespace dbn_pipe;

TEST(EventTest, CallbackOnReadable) {
    EpollEventLoop loop;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    bool called = false;
    Event event(loop, efd, true, false);
    event.OnEvent([&](uint32_t) { called = true; });

    // Write to make readable
    uint64_t val = 1;
    write(efd, &val, sizeof(val));

    loop.Poll(0);
    EXPECT_TRUE(called);

    close(efd);
}

TEST(EventTest, UnregistersOnDestruction) {
    EpollEventLoop loop;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    bool called = false;
    {
        Event event(loop, efd, false, true);
        event.OnEvent([&](uint32_t) { called = true; });

        loop.Poll(0);
        EXPECT_TRUE(called);
        called = false;
    }

    // After destruction, no callback
    loop.Poll(0);
    EXPECT_FALSE(called);

    close(efd);
}

TEST(EventTest, Update) {
    EpollEventLoop loop;

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    int call_count = 0;
    Event event(loop, efd, true, false);  // Read only
    event.OnEvent([&](uint32_t) { call_count++; });

    loop.Poll(0);
    EXPECT_EQ(call_count, 0);  // Not readable

    event.Update(false, true);  // Switch to write
    loop.Poll(0);
    EXPECT_EQ(call_count, 1);  // eventfd always writable

    close(efd);
}
