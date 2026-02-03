// SPDX-License-Identifier: MIT

#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/epoll_event_loop.hpp"

namespace dbn_pipe {

// Pimpl implementation using EpollEventLoop
struct EventLoop::Impl : EpollEventLoop {};

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {}

EventLoop::~EventLoop() = default;

void EventLoop::Poll(int timeout_ms) {
    impl_->Poll(timeout_ms);
}

void EventLoop::Run() {
    impl_->Run();
}

void EventLoop::Stop() {
    impl_->Stop();
}

EventLoop::operator IEventLoop&() {
    return *impl_;
}

EventLoop::operator const IEventLoop&() const {
    return *impl_;
}

}  // namespace dbn_pipe
