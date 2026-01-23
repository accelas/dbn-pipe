// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#include "dbwriter/asio_event_loop.hpp"
#include <gtest/gtest.h>

namespace dbwriter {
namespace {

TEST(AsioEventLoopTest, ConstructsWithContext) {
    asio::io_context ctx;
    AsioEventLoop loop(ctx);

    SUCCEED();
}

TEST(AsioEventLoopTest, DeferExecutesCallback) {
    asio::io_context ctx;
    AsioEventLoop loop(ctx);

    bool called = false;
    loop.Defer([&] { called = true; });

    ctx.run();

    EXPECT_TRUE(called);
}

TEST(AsioEventLoopTest, ScheduleExecutesAfterDelay) {
    asio::io_context ctx;
    AsioEventLoop loop(ctx);

    bool called = false;
    loop.Schedule(std::chrono::milliseconds(10), [&] { called = true; });

    ctx.run();

    EXPECT_TRUE(called);
}

}  // namespace
}  // namespace dbwriter
