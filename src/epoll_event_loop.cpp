// Copyright 2024 Databento, Inc.
// SPDX-License-Identifier: Apache-2.0

#include "epoll_event_loop.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace databento_async {

// EpollEventHandle implementation

EpollEventHandle::EpollEventHandle(EpollEventLoop& loop, int fd,
                                   bool want_read, bool want_write,
                                   IEventLoop::ReadCallback on_read,
                                   IEventLoop::WriteCallback on_write,
                                   IEventLoop::ErrorCallback on_error)
    : loop_(loop),
      fd_(fd),
      on_read_(std::move(on_read)),
      on_write_(std::move(on_write)),
      on_error_(std::move(on_error)) {
    epoll_event ev{};
    ev.events = ComputeEpollFlags(want_read, want_write);
    ev.data.ptr = this;

    if (epoll_ctl(loop_.epoll_fd(), EPOLL_CTL_ADD, fd_, &ev) < 0) {
        throw std::runtime_error(std::string("epoll_ctl ADD failed: ") +
                                 std::strerror(errno));
    }
}

EpollEventHandle::~EpollEventHandle() {
    // Remove from epoll - ignore errors (fd might already be closed)
    epoll_ctl(loop_.epoll_fd(), EPOLL_CTL_DEL, fd_, nullptr);
}

void EpollEventHandle::Update(bool want_read, bool want_write) {
    epoll_event ev{};
    ev.events = ComputeEpollFlags(want_read, want_write);
    ev.data.ptr = this;

    if (epoll_ctl(loop_.epoll_fd(), EPOLL_CTL_MOD, fd_, &ev) < 0) {
        throw std::runtime_error(std::string("epoll_ctl MOD failed: ") +
                                 std::strerror(errno));
    }
}

void EpollEventHandle::HandleEvents(uint32_t events) {
    // Handle errors first
    if ((events & (EPOLLERR | EPOLLHUP)) != 0) {
        int error_code = 0;
        socklen_t len = sizeof(error_code);
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error_code, &len) < 0) {
            error_code = errno;
        }
        if (on_error_) {
            on_error_(error_code);
        }
        return;  // Don't process read/write after error
    }

    // Handle read events
    if ((events & EPOLLIN) != 0 && on_read_) {
        on_read_();
    }

    // Handle write events
    if ((events & EPOLLOUT) != 0 && on_write_) {
        on_write_();
    }
}

uint32_t EpollEventHandle::ComputeEpollFlags(bool want_read,
                                              bool want_write) const {
    uint32_t flags = EPOLLET;  // Edge-triggered mode
    if (want_read) {
        flags |= EPOLLIN;
    }
    if (want_write) {
        flags |= EPOLLOUT;
    }
    return flags;
}

// EpollEventLoop implementation

EpollEventLoop::EpollEventLoop() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::runtime_error(std::string("epoll_create1 failed: ") +
                                 std::strerror(errno));
    }
}

EpollEventLoop::~EpollEventLoop() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}

std::unique_ptr<IEventHandle> EpollEventLoop::Register(
    int fd,
    bool want_read,
    bool want_write,
    ReadCallback on_read,
    WriteCallback on_write,
    ErrorCallback on_error) {
    return std::make_unique<EpollEventHandle>(
        *this, fd, want_read, want_write,
        std::move(on_read), std::move(on_write), std::move(on_error));
}

void EpollEventLoop::Defer(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_callbacks_.push_back(std::move(fn));
}

bool EpollEventLoop::IsInEventLoopThread() const {
    return std::this_thread::get_id() == loop_thread_id_.load();
}

void EpollEventLoop::Poll(int timeout_ms) {
    // Set the thread id for IsInEventLoopThread()
    loop_thread_id_.store(std::this_thread::get_id());

    // Process deferred callbacks first
    ProcessDeferredCallbacks();

    // Wait for events
    epoll_event events[kMaxEvents];
    int nfds = epoll_wait(epoll_fd_, events, kMaxEvents, timeout_ms);

    if (nfds < 0) {
        if (errno == EINTR) {
            return;  // Interrupted, just return
        }
        throw std::runtime_error(std::string("epoll_wait failed: ") +
                                 std::strerror(errno));
    }

    // Dispatch events
    for (int i = 0; i < nfds; ++i) {
        auto* handle = static_cast<EpollEventHandle*>(events[i].data.ptr);
        if (handle != nullptr) {
            handle->HandleEvents(events[i].events);
        }
    }
}

void EpollEventLoop::Run() {
    running_.store(true);
    while (running_.load()) {
        Poll(100);  // 100ms timeout to check running_ periodically
    }
}

void EpollEventLoop::Stop() {
    running_.store(false);
}

void EpollEventLoop::ProcessDeferredCallbacks() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        callbacks.swap(deferred_callbacks_);
    }

    for (auto& cb : callbacks) {
        if (cb) {
            cb();
        }
    }
}

}  // namespace databento_async
