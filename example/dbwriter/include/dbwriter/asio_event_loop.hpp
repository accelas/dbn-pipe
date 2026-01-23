// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "lib/stream/event_loop.hpp"
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace dbwriter {

// Shared state for AsioEventHandle to avoid use-after-free in async handlers.
// Handlers capture shared_ptr<State> instead of raw this pointer.
// The stream_descriptor is also owned by the state to ensure it outlives handlers.
struct AsioEventHandleState : public std::enable_shared_from_this<AsioEventHandleState> {
    asio::posix::stream_descriptor stream;
    std::function<void()> on_read;
    std::function<void()> on_write;
    std::function<void(int)> on_error;
    bool want_read = false;
    bool want_write = false;
    std::atomic<bool> destroyed{false};  // Set when handle is destroyed

    explicit AsioEventHandleState(asio::io_context& ctx, int fd)
        : stream(ctx, fd) {}

    void start_waiting() {
        if (want_read) {
            start_read_wait();
        }
        if (want_write) {
            start_write_wait();
        }
    }

    void start_read_wait() {
        // Capture weak_ptr to avoid preventing destruction
        std::weak_ptr<AsioEventHandleState> weak_self = shared_from_this();
        stream.async_wait(
            asio::posix::stream_descriptor::wait_read,
            [weak_self](std::error_code ec) {
                auto self = weak_self.lock();
                if (!self || self->destroyed) return;  // Handle was destroyed
                if (!ec) {
                    if (self->on_read) self->on_read();
                    if (self->want_read && !self->destroyed) {
                        self->start_read_wait();
                    }
                } else if (ec != asio::error::operation_aborted && self->on_error) {
                    self->on_error(ec.value());
                }
            });
    }

    void start_write_wait() {
        // Capture weak_ptr to avoid preventing destruction
        std::weak_ptr<AsioEventHandleState> weak_self = shared_from_this();
        stream.async_wait(
            asio::posix::stream_descriptor::wait_write,
            [weak_self](std::error_code ec) {
                auto self = weak_self.lock();
                if (!self || self->destroyed) return;  // Handle was destroyed
                if (!ec) {
                    if (self->on_write) self->on_write();
                    if (self->want_write && !self->destroyed) {
                        self->start_write_wait();
                    }
                } else if (ec != asio::error::operation_aborted && self->on_error) {
                    self->on_error(ec.value());
                }
            });
    }
};

// ASIO-based event handle for fd registration
class AsioEventHandle : public dbn_pipe::IEventHandle {
public:
    AsioEventHandle(asio::io_context& ctx, int fd,
                    bool want_read, bool want_write,
                    std::function<void()> on_read,
                    std::function<void()> on_write,
                    std::function<void(int)> on_error)
        : fd_(fd)
        , state_(std::make_shared<AsioEventHandleState>(ctx, fd)) {
        state_->on_read = std::move(on_read);
        state_->on_write = std::move(on_write);
        state_->on_error = std::move(on_error);
        state_->want_read = want_read;
        state_->want_write = want_write;
        state_->start_waiting();
    }

    ~AsioEventHandle() override {
        // Mark as destroyed so pending handlers don't access callbacks
        state_->destroyed = true;
        // Cancel pending operations (handlers will see operation_aborted)
        state_->stream.cancel();
        // Don't close the fd, we don't own it
        state_->stream.release();
    }

    void Update(bool want_read, bool want_write) override {
        state_->want_read = want_read;
        state_->want_write = want_write;
        // Cancel current waits and restart
        state_->stream.cancel();
        state_->start_waiting();
    }

    int fd() const override { return fd_; }

private:
    int fd_;
    std::shared_ptr<AsioEventHandleState> state_;
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
