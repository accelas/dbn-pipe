// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "lib/stream/epoll_event_loop.hpp"
#include "lib/stream/timer.hpp"

using namespace dbn_pipe;

TEST(TimerTest, OneShotFires) {
    EpollEventLoop loop;

    Timer timer(loop);

    bool fired = false;
    timer.OnTimer([&]() {
        fired = true;
        loop.Stop();
    });

    timer.Start(10);  // 10ms one-shot
    EXPECT_TRUE(timer.IsArmed());

    loop.Run();

    EXPECT_TRUE(fired);
    EXPECT_FALSE(timer.IsArmed());  // One-shot should disarm after firing
}

TEST(TimerTest, RepeatingFires) {
    EpollEventLoop loop;

    Timer timer(loop);

    int count = 0;
    timer.OnTimer([&]() {
        count++;
        if (count >= 3) {
            loop.Stop();
        }
    });

    timer.Start(10, 10);  // 10ms delay, 10ms interval

    loop.Run();

    EXPECT_GE(count, 3);
}

TEST(TimerTest, StopPreventsCallback) {
    EpollEventLoop loop;

    Timer timer(loop);

    bool fired = false;
    timer.OnTimer([&]() { fired = true; });

    timer.Start(50);  // 50ms
    timer.Stop();
    EXPECT_FALSE(timer.IsArmed());

    loop.Poll(100);  // Wait past when it would have fired

    EXPECT_FALSE(fired);
}
