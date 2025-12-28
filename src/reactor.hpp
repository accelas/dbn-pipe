// src/reactor.hpp
#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace databento_async {

class Reactor {
public:
    using Callback = std::function<void(uint32_t events)>;

    Reactor();
    ~Reactor();

    // Non-copyable, non-movable
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;
    Reactor& operator=(Reactor&&) = delete;

    // Register fd with events and callback
    void Add(int fd, uint32_t events, Callback cb);

    // Modify events for existing fd
    void Modify(int fd, uint32_t events);

    // Remove fd from reactor
    void Remove(int fd);

    // Poll for events, returns number handled
    // timeout_ms: -1 = block, 0 = non-blocking, >0 = timeout
    int Poll(int timeout_ms = -1);

    // Run until Stop() called
    void Run();

    // Signal Run() to stop
    void Stop();

private:
    int epoll_fd_;
    bool running_ = false;
    std::unordered_map<int, Callback> callbacks_;
    std::vector<epoll_event> events_;

    static constexpr int kMaxEvents = 64;
};

}  // namespace databento_async
