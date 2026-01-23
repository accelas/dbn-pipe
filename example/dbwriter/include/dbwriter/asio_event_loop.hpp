// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "lib/stream/event_loop.hpp"
#include <asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace dbwriter {

// ASIO-based event handle for fd registration
class AsioEventHandle : public dbn_pipe::IEventHandle {
public:
    AsioEventHandle(asio::io_context& ctx, int fd,
                    bool want_read, bool want_write,
                    std::function<void()> on_read,
                    std::function<void()> on_write,
                    std::function<void(int)> on_error)
        : fd_(fd)
        , stream_(ctx, fd)
        , on_read_(std::move(on_read))
        , on_write_(std::move(on_write))
        , on_error_(std::move(on_error))
        , want_read_(want_read)
        , want_write_(want_write) {
        start_waiting();
    }

    ~AsioEventHandle() override {
        stream_.release();  // Don't close the fd, we don't own it
    }

    void Update(bool want_read, bool want_write) override {
        want_read_ = want_read;
        want_write_ = want_write;
        // Cancel current waits and restart
        stream_.cancel();
        start_waiting();
    }

    int fd() const override { return fd_; }

private:
    void start_waiting() {
        if (want_read_) {
            stream_.async_wait(
                asio::posix::stream_descriptor::wait_read,
                [this](std::error_code ec) {
                    if (!ec && on_read_) {
                        on_read_();
                        if (want_read_) start_read_wait();
                    } else if (ec && ec != asio::error::operation_aborted && on_error_) {
                        on_error_(ec.value());
                    }
                });
        }
        if (want_write_) {
            stream_.async_wait(
                asio::posix::stream_descriptor::wait_write,
                [this](std::error_code ec) {
                    if (!ec && on_write_) {
                        on_write_();
                        if (want_write_) start_write_wait();
                    } else if (ec && ec != asio::error::operation_aborted && on_error_) {
                        on_error_(ec.value());
                    }
                });
        }
    }

    void start_read_wait() {
        stream_.async_wait(
            asio::posix::stream_descriptor::wait_read,
            [this](std::error_code ec) {
                if (!ec && on_read_) {
                    on_read_();
                    if (want_read_) start_read_wait();
                }
            });
    }

    void start_write_wait() {
        stream_.async_wait(
            asio::posix::stream_descriptor::wait_write,
            [this](std::error_code ec) {
                if (!ec && on_write_) {
                    on_write_();
                    if (want_write_) start_write_wait();
                }
            });
    }

    int fd_;
    asio::posix::stream_descriptor stream_;
    std::function<void()> on_read_;
    std::function<void()> on_write_;
    std::function<void(int)> on_error_;
    bool want_read_;
    bool want_write_;
};

// ASIO-based implementation of dbn_pipe::IEventLoop
class AsioEventLoop : public dbn_pipe::IEventLoop {
public:
    explicit AsioEventLoop(asio::io_context& ctx)
        : ctx_(ctx)
        , thread_id_(std::this_thread::get_id()) {}

    std::unique_ptr<dbn_pipe::IEventHandle> Register(
        int fd,
        bool want_read,
        bool want_write,
        ReadCallback on_read,
        WriteCallback on_write,
        ErrorCallback on_error) override {
        return std::make_unique<AsioEventHandle>(
            ctx_, fd, want_read, want_write,
            std::move(on_read), std::move(on_write), std::move(on_error));
    }

    void Defer(std::function<void()> fn) override {
        asio::post(ctx_, std::move(fn));
    }

    void Schedule(std::chrono::milliseconds delay, TimerCallback fn) override {
        auto timer = std::make_shared<asio::steady_timer>(ctx_, delay);
        timer->async_wait([timer, fn = std::move(fn)](auto ec) {
            if (!ec) fn();
        });
    }

    bool IsInEventLoopThread() const override {
        return std::this_thread::get_id() == thread_id_;
    }

    // Additional ASIO-specific methods
    void Run() { ctx_.run(); }
    void Stop() { ctx_.stop(); }
    void Poll() { ctx_.poll(); }

    template <typename Awaitable>
    void Spawn(Awaitable&& coro) {
        asio::co_spawn(ctx_, std::forward<Awaitable>(coro), asio::detached);
    }

    asio::io_context& context() { return ctx_; }

private:
    asio::io_context& ctx_;
    std::thread::id thread_id_;
};

}  // namespace dbwriter
