// src/reactor.cpp
#include "reactor.hpp"

#include <sys/timerfd.h>
#include <unistd.h>

#include <system_error>

namespace databento_async {

// Event implementation
Event::Event(Reactor& reactor, int fd, uint32_t events)
    : reactor_(reactor), fd_(fd) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = this;

    if (epoll_ctl(reactor_.epoll_fd(), EPOLL_CTL_ADD, fd_, &ev) < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl ADD");
    }
}

Event::~Event() {
    Remove();
}

void Event::Modify(uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = this;

    if (epoll_ctl(reactor_.epoll_fd(), EPOLL_CTL_MOD, fd_, &ev) < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl MOD");
    }
}

void Event::Remove() {
    if (fd_ >= 0) {
        epoll_ctl(reactor_.epoll_fd(), EPOLL_CTL_DEL, fd_, nullptr);
        fd_ = -1;
    }
}

// Timer implementation
Timer::Timer(Reactor& reactor)
    : event_(reactor, timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC), EPOLLIN) {
    if (event_.fd() < 0) {
        throw std::system_error(errno, std::system_category(), "timerfd_create");
    }
    event_.OnEvent([this](uint32_t events) { HandleEvent(events); });
}

Timer::~Timer() {
    int fd = event_.fd();
    event_.Remove();
    if (fd >= 0) {
        close(fd);
    }
}

void Timer::Start(int delay_ms, int interval_ms) {
    itimerspec spec{};
    spec.it_value.tv_sec = delay_ms / 1000;
    spec.it_value.tv_nsec = (delay_ms % 1000) * 1000000L;
    spec.it_interval.tv_sec = interval_ms / 1000;
    spec.it_interval.tv_nsec = (interval_ms % 1000) * 1000000L;

    if (timerfd_settime(event_.fd(), 0, &spec, nullptr) < 0) {
        throw std::system_error(errno, std::system_category(), "timerfd_settime");
    }
    armed_ = true;
}

void Timer::Stop() {
    itimerspec spec{};  // Zero = disarm
    timerfd_settime(event_.fd(), 0, &spec, nullptr);
    armed_ = false;
}

void Timer::HandleEvent(uint32_t /*events*/) {
    // Read to clear the timer
    uint64_t expirations;
    read(event_.fd(), &expirations, sizeof(expirations));
    callback_();
}

// Reactor implementation
Reactor::Reactor() : events_(kMaxEvents) {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_create1");
    }
}

Reactor::~Reactor() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}

int Reactor::Poll(int timeout_ms) {
    // Record the reactor thread ID for IsInReactorThread() checks
    reactor_thread_id_.store(std::this_thread::get_id(), std::memory_order_release);

    // Run deferred callbacks before polling (enables lazy epoll modifications)
    RunDeferred();

    int n = epoll_wait(epoll_fd_, events_.data(), kMaxEvents, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) {
            return 0;
        }
        throw std::system_error(errno, std::system_category(), "epoll_wait");
    }

    for (int i = 0; i < n; ++i) {
        auto* event = static_cast<Event*>(events_[i].data.ptr);
        event->Handle(events_[i].events);
    }

    // Run deferred callbacks after events (for callbacks scheduled by handlers)
    RunDeferred();

    return n;
}

void Reactor::RunDeferred() {
    while (!deferred_.empty()) {
        auto callbacks = std::move(deferred_);
        deferred_.clear();
        for (auto& cb : callbacks) {
            cb();
        }
    }
}

void Reactor::Run() {
    running_ = true;
    while (running_) {
        Poll(-1);
    }
}

void Reactor::Stop() {
    running_ = false;
}

}  // namespace databento_async
