// SPDX-License-Identifier: MIT

#include "lib/stream/epoll_event_loop.hpp"

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace dbn_pipe {

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

    // Create eventfd for cross-thread wakeup
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        close(epoll_fd_);
        throw std::runtime_error(std::string("eventfd failed: ") +
                                 std::strerror(errno));
    }

    // Register wake_fd_ with epoll (data.ptr = nullptr to distinguish from handles)
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) < 0) {
        close(wake_fd_);
        close(epoll_fd_);
        throw std::runtime_error(std::string("epoll_ctl ADD wake_fd failed: ") +
                                 std::strerror(errno));
    }
}

EpollEventLoop::~EpollEventLoop() {
    // Clean up any pending timers
    for (auto& entry : timers_) {
        if (entry && entry->fd >= 0) {
            close(entry->fd);
        }
    }
    timers_.clear();

    // Close wake_fd_ before epoll_fd_
    if (wake_fd_ >= 0) {
        close(wake_fd_);
    }

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
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        deferred_callbacks_.push_back(std::move(fn));
    }

    // Wake up the event loop if called from another thread
    if (!IsInEventLoopThread()) {
        Wake();
    }
}

void EpollEventLoop::Schedule(std::chrono::milliseconds delay, TimerCallback fn) {
    // Create timerfd for one-shot timer
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        throw std::runtime_error(std::string("timerfd_create failed: ") +
                                 std::strerror(errno));
    }

    // Set the timer expiration
    itimerspec ts{};
    ts.it_value.tv_sec = delay.count() / 1000;
    ts.it_value.tv_nsec = (delay.count() % 1000) * 1000000;
    // it_interval is zero for one-shot timer

    if (timerfd_settime(tfd, 0, &ts, nullptr) < 0) {
        close(tfd);
        throw std::runtime_error(std::string("timerfd_settime failed: ") +
                                 std::strerror(errno));
    }

    // Create timer entry
    auto entry = std::make_unique<TimerEntry>();
    entry->fd = tfd;
    entry->callback = std::move(fn);

    // Capture raw pointer for lambda (entry will be moved)
    int timer_fd = tfd;

    // Register with epoll for read events
    entry->handle = Register(
        tfd,
        true,   // want_read
        false,  // want_write
        [this, timer_fd]() { HandleTimerExpired(timer_fd); },
        nullptr,
        nullptr);

    // Store the timer entry
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        timers_.push_back(std::move(entry));
    }
}

void EpollEventLoop::HandleTimerExpired(int timer_fd) {
    // Read the timer to clear it (required for timerfd)
    uint64_t expirations = 0;
    [[maybe_unused]] ssize_t n = read(timer_fd, &expirations, sizeof(expirations));

    // Find and execute the callback
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        auto it = std::find_if(timers_.begin(), timers_.end(),
            [timer_fd](const auto& e) { return e->fd == timer_fd; });
        if (it != timers_.end()) {
            callback = std::move((*it)->callback);
            // Handle will be destroyed, which removes from epoll
            close(timer_fd);
            timers_.erase(it);
        }
    }

    // Execute callback outside the lock
    if (callback) {
        callback();
    }
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
        } else {
            // wake_fd_ event - read and discard to clear the eventfd
            uint64_t val;
            [[maybe_unused]] ssize_t n = read(wake_fd_, &val, sizeof(val));
        }
    }

    // Process deferred callbacks that may have been added during wake
    ProcessDeferredCallbacks();
}

void EpollEventLoop::Run() {
    // Only run if we're in Idle state (not already Stopped)
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Running)) {
        return;  // Already running or stopped
    }
    while (state_.load() == State::Running) {
        Poll(100);  // 100ms timeout to check state_ periodically
    }
}

void EpollEventLoop::Stop() {
    state_.store(State::Stopped);
    Wake();  // Interrupt epoll_wait so Run() exits immediately
}

void EpollEventLoop::Wake() {
    uint64_t val = 1;
    [[maybe_unused]] ssize_t n = write(wake_fd_, &val, sizeof(val));
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

}  // namespace dbn_pipe
