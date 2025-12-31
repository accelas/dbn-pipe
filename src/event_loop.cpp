// Copyright 2024 Databento, Inc.
// SPDX-License-Identifier: Apache-2.0

#include "event_loop.hpp"
#include "epoll_event_loop.hpp"

namespace databento_async {

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

}  // namespace databento_async
