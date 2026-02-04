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

#include "dbn_pipe/stream/event_loop.hpp"

namespace dbn_pipe {

class EpollEventLoop;

/// Handle for a registered file descriptor in the epoll event loop.
///
/// Returned by EpollEventLoop::Register().  Destroying the handle
/// automatically removes the fd from the epoll set.
class EpollEventHandle : public IEventHandle {
public:
    /// Construct a handle and register @p fd with the epoll instance.
    EpollEventHandle(EpollEventLoop& loop, int fd,
                     bool want_read, bool want_write,
                     IEventLoop::ReadCallback on_read,
                     IEventLoop::WriteCallback on_write,
                     IEventLoop::ErrorCallback on_error);

    ~EpollEventHandle() override;

    EpollEventHandle(const EpollEventHandle&) = delete;
    EpollEventHandle& operator=(const EpollEventHandle&) = delete;
    EpollEventHandle(EpollEventHandle&&) = delete;
    EpollEventHandle& operator=(EpollEventHandle&&) = delete;

    /// Change which I/O directions are monitored.
    /// @param want_read   True to receive read-ready callbacks.
    /// @param want_write  True to receive write-ready callbacks.
    void Update(bool want_read, bool want_write) override;

    /// @return The monitored file descriptor.
    int fd() const override { return fd_; }

    /// Dispatch callbacks for the given epoll event mask (called internally).
    /// @param events  Bitmask of epoll events (EPOLLIN, EPOLLOUT, EPOLLERR, ...).
    void HandleEvents(uint32_t events);

private:
    uint32_t ComputeEpollFlags(bool want_read, bool want_write) const;

    EpollEventLoop& loop_;
    int fd_;
    IEventLoop::ReadCallback on_read_;
    IEventLoop::WriteCallback on_write_;
    IEventLoop::ErrorCallback on_error_;
};

/// Internal timer entry for scheduled callbacks.
struct TimerEntry {
    int fd;                             ///< timerfd file descriptor.
    std::function<void()> callback;     ///< User callback to invoke on expiry.
    std::unique_ptr<IEventHandle> handle;  ///< Epoll registration for the timerfd.
};

/// Epoll-based event loop for non-blocking I/O and timer scheduling.
///
/// Wraps Linux epoll to multiplex reads, writes, and timers on a single
/// thread.  Create one instance, register file descriptors with
/// Register(), and drive the loop with Run() or Poll().
///
/// Thread safety: the loop itself runs on a single thread.  Defer() and
/// Wake() may be called from any thread.
class EpollEventLoop : public IEventLoop {
public:
    /// Create an epoll instance and an internal eventfd for cross-thread wakeups.
    EpollEventLoop();
    ~EpollEventLoop() override;

    EpollEventLoop(const EpollEventLoop&) = delete;
    EpollEventLoop& operator=(const EpollEventLoop&) = delete;
    EpollEventLoop(EpollEventLoop&&) = delete;
    EpollEventLoop& operator=(EpollEventLoop&&) = delete;

    /// Register a file descriptor for I/O monitoring.
    ///
    /// @param fd          File descriptor to monitor.
    /// @param want_read   True to receive read-ready callbacks.
    /// @param want_write  True to receive write-ready callbacks.
    /// @param on_read     Callback invoked when the fd is readable.
    /// @param on_write    Callback invoked when the fd is writable.
    /// @param on_error    Callback invoked on error or hangup.
    /// @return An IEventHandle whose lifetime controls the registration.
    std::unique_ptr<IEventHandle> Register(
        int fd,
        bool want_read,
        bool want_write,
        ReadCallback on_read,
        WriteCallback on_write,
        ErrorCallback on_error) override;

    /// Queue a callback to run on the event-loop thread.
    void Defer(std::function<void()> fn) override;

    /// Schedule a one-shot callback after @p delay milliseconds.
    void Schedule(std::chrono::milliseconds delay, TimerCallback fn) override;

    /// @return True if the calling thread is the event-loop thread.
    bool IsInEventLoopThread() const override;

    /// Poll for events with the given timeout (milliseconds).  -1 blocks.
    void Poll(int timeout_ms);

    /// Run the event loop until Stop() is called.
    void Run();

    /// Signal the loop to exit after the current poll completes.
    void Stop();

    /// Wake the event loop from another thread (e.g. after Defer()).
    void Wake();

    /// @return The underlying epoll file descriptor (used internally by EpollEventHandle).
    int epoll_fd() const { return epoll_fd_; }

private:
    void ProcessDeferredCallbacks();
    void HandleTimerExpired(int timer_fd);

    enum class State { Idle, Running, Stopped };

    int epoll_fd_;
    int wake_fd_ = -1;
    std::atomic<State> state_{State::Idle};
    std::atomic<std::thread::id> loop_thread_id_{};

    std::mutex deferred_mutex_;
    std::vector<std::function<void()>> deferred_callbacks_;

    // Active timers (protected by deferred_mutex_)
    std::vector<std::unique_ptr<TimerEntry>> timers_;

    static constexpr int kMaxEvents = 64;
};

}  // namespace dbn_pipe
