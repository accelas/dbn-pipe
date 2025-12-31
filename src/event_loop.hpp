// Copyright 2024 Databento, Inc.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace databento_async {

// Handle for a registered file descriptor
class IEventHandle {
public:
    virtual ~IEventHandle() = default;

    // Modify watched events
    virtual void Update(bool want_read, bool want_write) = 0;

    // Get file descriptor
    virtual int fd() const = 0;
};

// Event loop interface - minimal surface for socket I/O
class IEventLoop {
public:
    using ReadCallback = std::function<void()>;
    using WriteCallback = std::function<void()>;
    using ErrorCallback = std::function<void(int error_code)>;

    virtual ~IEventLoop() = default;

    // Register fd for read/write events. Returns handle for updates.
    // Callbacks are invoked when fd becomes readable/writable.
    // Error callback invoked on EPOLLERR/EPOLLHUP with SO_ERROR value.
    virtual std::unique_ptr<IEventHandle> Register(
        int fd,
        bool want_read,
        bool want_write,
        ReadCallback on_read,
        WriteCallback on_write,
        ErrorCallback on_error) = 0;

    // Schedule callback for next event loop iteration
    virtual void Defer(std::function<void()> fn) = 0;

    // Check if current thread is the event loop thread
    virtual bool IsInEventLoopThread() const = 0;
};

// Type-erased event loop wrapper.
// Hides concrete implementation (epoll) behind stable ABI.
// Provides implicit conversion to IEventLoop& for use with Pipeline.
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    // Non-copyable, non-movable
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    // Event loop control
    void Poll(int timeout_ms = -1);
    void Run();
    void Stop();

    // Implicit conversion for Pipeline::Create(loop, ...)
    operator IEventLoop&();
    operator const IEventLoop&() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace databento_async
