// lib/stream/reactor.cpp
#include "lib/stream/reactor.hpp"
#include "lib/stream/event_loop.hpp"

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <system_error>

namespace dbn_pipe {

// ReactorEventHandle: IEventHandle implementation for Reactor
class ReactorEventHandle : public IEventHandle {
public:
    ReactorEventHandle(Reactor& reactor, int fd,
                       bool want_read, bool want_write,
                       IEventLoop::ReadCallback on_read,
                       IEventLoop::WriteCallback on_write,
                       IEventLoop::ErrorCallback on_error)
        : reactor_(reactor)
        , fd_(fd)
        , on_read_(std::move(on_read))
        , on_write_(std::move(on_write))
        , on_error_(std::move(on_error))
        , event_(reactor, fd, ComputeEvents(want_read, want_write)) {
        event_.OnEvent([this](uint32_t events) { HandleEvents(events); });
    }

    ~ReactorEventHandle() override = default;

    void Update(bool want_read, bool want_write) override {
        event_.Modify(ComputeEvents(want_read, want_write));
    }

    int fd() const override { return fd_; }

private:
    static uint32_t ComputeEvents(bool want_read, bool want_write) {
        uint32_t events = 0;
        if (want_read) events |= EPOLLIN;
        if (want_write) events |= EPOLLOUT;
        return events;
    }

    void HandleEvents(uint32_t events) {
        if (events & (EPOLLERR | EPOLLHUP)) {
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len);
            if (on_error_) on_error_(error);
            return;
        }
        if ((events & EPOLLIN) && on_read_) {
            on_read_();
        }
        if ((events & EPOLLOUT) && on_write_) {
            on_write_();
        }
    }

    Reactor& reactor_;
    int fd_;
    IEventLoop::ReadCallback on_read_;
    IEventLoop::WriteCallback on_write_;
    IEventLoop::ErrorCallback on_error_;
    Event event_;
};

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

    // Create eventfd for cross-thread wakeup
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        close(epoll_fd_);
        throw std::system_error(errno, std::system_category(), "eventfd");
    }

    // Register wake_fd_ with epoll (no Event wrapper, handled specially in Poll)
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;  // nullptr indicates wake event
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) < 0) {
        close(wake_fd_);
        close(epoll_fd_);
        throw std::system_error(errno, std::system_category(), "epoll_ctl wake_fd");
    }
}

Reactor::~Reactor() {
    if (wake_fd_ >= 0) {
        close(wake_fd_);
    }
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
    }
}

std::unique_ptr<IEventHandle> Reactor::Register(
    int fd,
    bool want_read,
    bool want_write,
    ReadCallback on_read,
    WriteCallback on_write,
    ErrorCallback on_error) {
    return std::make_unique<ReactorEventHandle>(
        *this, fd, want_read, want_write,
        std::move(on_read), std::move(on_write), std::move(on_error));
}

int Reactor::Poll(int timeout_ms) {
    // Record the reactor thread ID for IsInReactorThread() checks
    reactor_thread_id_.store(std::this_thread::get_id(), std::memory_order_release);

    int n = epoll_wait(epoll_fd_, events_.data(), kMaxEvents, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) {
            return 0;
        }
        throw std::system_error(errno, std::system_category(), "epoll_wait");
    }

    for (int i = 0; i < n; ++i) {
        auto* event = static_cast<Event*>(events_[i].data.ptr);
        if (event == nullptr) {
            // Wake event - drain the eventfd
            uint64_t val;
            read(wake_fd_, &val, sizeof(val));
            continue;
        }
        event->Handle(events_[i].events);
    }

    // Run deferred callbacks (thread-safe: callbacks may be added from other threads)
    for (;;) {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(deferred_mutex_);
            if (deferred_.empty()) break;
            callbacks.swap(deferred_);
        }
        for (auto& cb : callbacks) {
            cb();
        }
    }

    return n;
}

void Reactor::Run() {
    // Only run if we're in Idle state (not already Stopped)
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Running, std::memory_order_relaxed)) {
        return;  // Already running or stopped
    }
    while (state_.load(std::memory_order_relaxed) == State::Running) {
        Poll(-1);
    }
}

void Reactor::Stop() {
    state_.store(State::Stopped, std::memory_order_relaxed);
    Wake();  // Interrupt epoll_wait so Run() exits immediately
}

void Reactor::Wake() {
    uint64_t val = 1;
    write(wake_fd_, &val, sizeof(val));
}

}  // namespace dbn_pipe
