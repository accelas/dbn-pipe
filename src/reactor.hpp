// src/reactor.hpp
#pragma once

#include <sys/epoll.h>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "event_loop.hpp"

namespace dbn_pipe {

class Reactor;

// Callback concept
template<typename F>
concept EventHandler = std::invocable<F, uint32_t>;

// Event wraps fd + callback, registered with Reactor
class Event {
public:
    using Callback = std::function<void(uint32_t events)>;

    Event(Reactor& reactor, int fd, uint32_t events);
    ~Event();

    // Non-copyable, non-movable (registered with epoll)
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    // Set callback
    template<EventHandler F>
    void OnEvent(F&& cb) { callback_ = std::forward<F>(cb); }

    // Modify watched events
    void Modify(uint32_t events);

    // Remove from reactor
    void Remove();

    int fd() const { return fd_; }

private:
    friend class Reactor;
    void Handle(uint32_t events) { callback_(events); }

    Reactor& reactor_;
    int fd_;
    Callback callback_ = [](uint32_t) {};
};

// Timer callback concept
template<typename F>
concept TimerHandler = std::invocable<F>;

// Timer wraps timerfd, integrated with Reactor
class Timer {
public:
    using Callback = std::function<void()>;

    explicit Timer(Reactor& reactor);
    ~Timer();

    // Non-copyable, non-movable
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    // Set callback
    template<TimerHandler F>
    void OnTimer(F&& cb) { callback_ = std::forward<F>(cb); }

    // Arm timer (milliseconds). Set interval_ms > 0 for repeating.
    void Start(int delay_ms, int interval_ms = 0);

    // Disarm timer
    void Stop();

    bool IsArmed() const { return armed_; }

private:
    void HandleEvent(uint32_t events);

    Event event_;
    Callback callback_ = []() {};
    bool armed_ = false;
};

class Reactor : public IEventLoop {
public:
    Reactor();
    ~Reactor() override;

    // Non-copyable, non-movable
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;
    Reactor& operator=(Reactor&&) = delete;

    // IEventLoop interface
    std::unique_ptr<IEventHandle> Register(
        int fd,
        bool want_read,
        bool want_write,
        ReadCallback on_read,
        WriteCallback on_write,
        ErrorCallback on_error) override;

    void Defer(std::function<void()> fn) override {
        deferred_.push_back(std::move(fn));
    }

    bool IsInEventLoopThread() const override {
        return reactor_thread_id_.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

    // Poll for events, returns number handled
    int Poll(int timeout_ms = -1);

    // Run until Stop() called
    void Run();

    // Signal Run() to stop
    void Stop();

    int epoll_fd() const { return epoll_fd_; }

    // Legacy alias
    bool IsInReactorThread() const { return IsInEventLoopThread(); }

private:
    int epoll_fd_;
    bool running_ = false;
    std::vector<epoll_event> events_;
    std::vector<std::function<void()>> deferred_;
    mutable std::atomic<std::thread::id> reactor_thread_id_{};

    static constexpr int kMaxEvents = 64;
};

}  // namespace dbn_pipe
