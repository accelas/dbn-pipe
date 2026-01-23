// SPDX-License-Identifier: MIT

#pragma once

#include <sys/epoll.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "lib/stream/event_loop.hpp"

namespace dbn_pipe {

class EpollEventLoop;

// Handle for a registered file descriptor in the epoll event loop
class EpollEventHandle : public IEventHandle {
public:
    EpollEventHandle(EpollEventLoop& loop, int fd,
                     bool want_read, bool want_write,
                     IEventLoop::ReadCallback on_read,
                     IEventLoop::WriteCallback on_write,
                     IEventLoop::ErrorCallback on_error);

    ~EpollEventHandle() override;

    // Non-copyable, non-movable
    EpollEventHandle(const EpollEventHandle&) = delete;
    EpollEventHandle& operator=(const EpollEventHandle&) = delete;
    EpollEventHandle(EpollEventHandle&&) = delete;
    EpollEventHandle& operator=(EpollEventHandle&&) = delete;

    // IEventHandle interface
    void Update(bool want_read, bool want_write) override;
    int fd() const override { return fd_; }

    // Called by EpollEventLoop when events occur
    void HandleEvents(uint32_t events);

private:
    uint32_t ComputeEpollFlags(bool want_read, bool want_write) const;

    EpollEventLoop& loop_;
    int fd_;
    IEventLoop::ReadCallback on_read_;
    IEventLoop::WriteCallback on_write_;
    IEventLoop::ErrorCallback on_error_;
};

// Timer entry for scheduled callbacks
struct TimerEntry {
    int fd;                             // timerfd file descriptor
    std::function<void()> callback;
    std::unique_ptr<IEventHandle> handle;
};

// Epoll-based implementation of IEventLoop
class EpollEventLoop : public IEventLoop {
public:
    EpollEventLoop();
    ~EpollEventLoop() override;

    // Non-copyable, non-movable
    EpollEventLoop(const EpollEventLoop&) = delete;
    EpollEventLoop& operator=(const EpollEventLoop&) = delete;
    EpollEventLoop(EpollEventLoop&&) = delete;
    EpollEventLoop& operator=(EpollEventLoop&&) = delete;

    // IEventLoop interface
    std::unique_ptr<IEventHandle> Register(
        int fd,
        bool want_read,
        bool want_write,
        ReadCallback on_read,
        WriteCallback on_write,
        ErrorCallback on_error) override;

    void Defer(std::function<void()> fn) override;
    void Schedule(std::chrono::milliseconds delay, TimerCallback fn) override;
    bool IsInEventLoopThread() const override;

    // Event loop control
    void Poll(int timeout_ms);
    void Run();
    void Stop();

    // Internal: called by EpollEventHandle
    int epoll_fd() const { return epoll_fd_; }

private:
    void ProcessDeferredCallbacks();
    void HandleTimerExpired(int timer_fd);

    int epoll_fd_;
    std::atomic<bool> running_{false};
    std::atomic<std::thread::id> loop_thread_id_{};

    std::mutex deferred_mutex_;
    std::vector<std::function<void()>> deferred_callbacks_;

    // Active timers (protected by deferred_mutex_)
    std::vector<std::unique_ptr<TimerEntry>> timers_;

    static constexpr int kMaxEvents = 64;
};

}  // namespace dbn_pipe
