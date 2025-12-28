// src/reactor.cpp
#include "reactor.hpp"

#include <unistd.h>

#include <stdexcept>
#include <system_error>

namespace databento_async {

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

void Reactor::Add(int fd, uint32_t events, Callback cb) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl ADD");
    }

    callbacks_[fd] = std::move(cb);
}

void Reactor::Modify(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl MOD");
    }
}

void Reactor::Remove(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    callbacks_.erase(fd);
}

int Reactor::Poll(int timeout_ms) {
    int n = epoll_wait(epoll_fd_, events_.data(), kMaxEvents, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) {
            return 0;
        }
        throw std::system_error(errno, std::system_category(), "epoll_wait");
    }

    for (int i = 0; i < n; ++i) {
        int fd = events_[i].data.fd;
        auto it = callbacks_.find(fd);
        if (it != callbacks_.end()) {
            it->second(events_[i].events);
        }
    }

    return n;
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
