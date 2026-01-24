// SPDX-License-Identifier: MIT

#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <functional>
#include <memory>

#include "lib/stream/event_loop.hpp"

namespace dbn_pipe {

// Event wraps fd registration with IEventLoop
// Portable - works with any IEventLoop implementation
class Event {
public:
    using Callback = std::function<void(uint32_t events)>;

    Event(IEventLoop& loop, int fd, bool want_read, bool want_write)
        : fd_(fd) {
        handle_ = loop.Register(
            fd, want_read, want_write,
            [this]() { if (callback_) callback_(EPOLLIN); },
            [this]() { if (callback_) callback_(EPOLLOUT); },
            [this](int err) { if (callback_) callback_(EPOLLERR); (void)err; }
        );
    }

    ~Event() = default;

    // Non-copyable, non-movable
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    void OnEvent(Callback cb) { callback_ = std::move(cb); }

    void Update(bool want_read, bool want_write) {
        handle_->Update(want_read, want_write);
    }

    int fd() const { return fd_; }

private:
    int fd_;
    std::unique_ptr<IEventHandle> handle_;
    Callback callback_;
};

}  // namespace dbn_pipe
