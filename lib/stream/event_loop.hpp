// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace dbn_pipe {

/// Handle for a registered file descriptor, returned by IEventLoop::Register().
class IEventHandle {
public:
    virtual ~IEventHandle() = default;

    /// Change which events (read/write) are being monitored.
    virtual void Update(bool want_read, bool want_write) = 0;

    /// Return the monitored file descriptor.
    virtual int fd() const = 0;
};

/// Event loop interface for I/O multiplexing.
///
/// Implement this to integrate dbn-pipe with an existing event loop
/// (libuv, asio, etc.). The built-in EventLoop class wraps an epoll
/// implementation behind this interface.
///
/// All callbacks are invoked on the event loop thread.
class IEventLoop {
public:
    using ReadCallback = std::function<void()>;
    using WriteCallback = std::function<void()>;
    using ErrorCallback = std::function<void(int error_code)>;
    using TimerCallback = std::function<void()>;

    virtual ~IEventLoop() = default;

    /// Register a file descriptor for event monitoring.
    ///
    /// @param fd         File descriptor to monitor
    /// @param want_read  Monitor for readability
    /// @param want_write Monitor for writability
    /// @param on_read    Called when fd is readable
    /// @param on_write   Called when fd is writable
    /// @param on_error   Called on EPOLLERR/EPOLLHUP with SO_ERROR value
    /// @return Handle to modify or unregister the fd (unregisters on destruction)
    virtual std::unique_ptr<IEventHandle> Register(
        int fd,
        bool want_read,
        bool want_write,
        ReadCallback on_read,
        WriteCallback on_write,
        ErrorCallback on_error) = 0;

    /// Schedule a callback for the next event loop iteration.
    virtual void Defer(std::function<void()> fn) = 0;

    /// Schedule a callback after a delay.
    /// @param delay  Minimum time before callback fires
    /// @param fn     Callback to invoke
    virtual void Schedule(std::chrono::milliseconds delay, TimerCallback fn) = 0;

    /// Return true if the caller is on the event loop thread.
    virtual bool IsInEventLoopThread() const = 0;
};

/// Type-erased event loop using epoll internally.
///
/// Provides implicit conversion to IEventLoop& so it can be passed
/// directly to client factory methods:
/// @code
/// EventLoop loop;
/// auto client = LiveClient::Create(loop, "api-key");
/// loop.Run();
/// @endcode
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    /// Block and dispatch events. Returns when no fds remain or Stop() is called.
    /// @param timeout_ms  Max wait per iteration (-1 = infinite)
    void Poll(int timeout_ms = -1);

    /// Run the event loop until Stop() is called.
    void Run();

    /// Signal the event loop to stop after the current iteration.
    void Stop();

    /// Implicit conversion to IEventLoop&.
    operator IEventLoop&();
    /// @copydoc operator IEventLoop&()
    operator const IEventLoop&() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dbn_pipe
