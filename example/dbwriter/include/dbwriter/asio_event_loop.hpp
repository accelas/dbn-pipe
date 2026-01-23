// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

namespace dbwriter {

// Matches dbn_pipe::IEventLoop interface
class AsioEventLoop {
public:
    using ReadCallback = std::function<void()>;
    using WriteCallback = std::function<void()>;
    using ErrorCallback = std::function<void(int)>;
    using TimerCallback = std::function<void()>;

    explicit AsioEventLoop(asio::io_context& ctx)
        : ctx_(ctx)
        , thread_id_(std::this_thread::get_id()) {}

    void Run() { ctx_.run(); }
    void Stop() { ctx_.stop(); }

    void Defer(std::function<void()> fn) {
        asio::post(ctx_, std::move(fn));
    }

    void Schedule(std::chrono::milliseconds delay, TimerCallback fn) {
        auto timer = std::make_shared<asio::steady_timer>(ctx_, delay);
        timer->async_wait([timer, fn = std::move(fn)](auto ec) {
            if (!ec) fn();
        });
    }

    bool IsInEventLoopThread() const {
        return std::this_thread::get_id() == thread_id_;
    }

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
