// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#include "dbwriter/asio_event_loop.hpp"
#include "lib/stream/event_loop.hpp"
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dbwriter {
namespace {

TEST(AsioEventLoopTest, ConstructsWithContext) {
    asio::io_context ctx;
    AsioEventLoop loop(ctx);

    SUCCEED();
}

TEST(AsioEventLoopTest, ImplementsIEventLoop) {
    asio::io_context ctx;
    AsioEventLoop loop(ctx);

    // Should be convertible to IEventLoop reference
    dbn_pipe::IEventLoop& iloop = loop;
    EXPECT_TRUE(iloop.IsInEventLoopThread());
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

TEST(AsioEventLoopTest, RegisterFdForRead) {
    asio::io_context ctx;
    AsioEventLoop loop(ctx);

    // Create a pipe for testing
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    bool read_called = false;
    auto handle = loop.Register(
        pipefd[0],  // read end
        true,       // want read
        false,      // no write
        [&] { read_called = true; },
        [] {},
        [](int) {});

    // Write to pipe to trigger read
    char c = 'x';
    ASSERT_EQ(write(pipefd[1], &c, 1), 1);

    // Run event loop briefly
    ctx.run_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(read_called);

    // Cleanup
    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(AsioEventLoopTest, RegisterFdForWrite) {
    asio::io_context ctx;
    AsioEventLoop loop(ctx);

    // Create a pipe for testing
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    bool write_called = false;
    auto handle = loop.Register(
        pipefd[1],  // write end
        false,      // no read
        true,       // want write
        [] {},
        [&] { write_called = true; },
        [](int) {});

    // Run event loop briefly - write end should be immediately writable
    ctx.run_for(std::chrono::milliseconds(50));

    EXPECT_TRUE(write_called);

    // Cleanup
    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(AsioEventLoopTest, EventHandleUpdate) {
    asio::io_context ctx;
    AsioEventLoop loop(ctx);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    int read_count = 0;
    int write_count = 0;
    auto handle = loop.Register(
        pipefd[1],
        false, true,  // Initially want write only
        [&] { read_count++; },
        [&] { write_count++; },
        [](int) {});

    ctx.run_for(std::chrono::milliseconds(10));
    EXPECT_GT(write_count, 0);

    // Update to disable write
    handle->Update(false, false);

    int prev_write_count = write_count;
    ctx.run_for(std::chrono::milliseconds(10));
    // Write count should not increase significantly after disabling
    EXPECT_LE(write_count, prev_write_count + 1);

    close(pipefd[0]);
    close(pipefd[1]);
}

}  // namespace
}  // namespace dbwriter
