// src/tcp_socket.cpp
#include "tcp_socket.hpp"

#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "reactor.hpp"

namespace databento_async {

TcpSocket::TcpSocket(Reactor* reactor)
    : reactor_(reactor), read_buffer_(kReadBufferSize) {}

TcpSocket::~TcpSocket() { Close(); }

void TcpSocket::Connect(const sockaddr_storage& addr) {
    // Create socket based on address family
    int family = addr.ss_family;
    fd_ = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        if (on_error_) {
            on_error_(std::error_code(errno, std::system_category()));
        }
        return;
    }

    // Disable Nagle for lower latency
    int opt = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Determine sockaddr size based on family
    socklen_t addr_len = (family == AF_INET6) ? sizeof(sockaddr_in6)
                                              : sizeof(sockaddr_in);

    int ret = connect(fd_, reinterpret_cast<const sockaddr*>(&addr), addr_len);
    if (ret < 0 && errno != EINPROGRESS) {
        auto ec = std::error_code(errno, std::system_category());
        close(fd_);
        fd_ = -1;
        if (on_error_) {
            on_error_(ec);
        }
        return;
    }

    // connected_ remains false until HandleWritable confirms connection
    reactor_->Add(fd_, EPOLLOUT | EPOLLIN | EPOLLET,
                  [this](uint32_t events) { HandleEvents(events); });
}

void TcpSocket::Write(std::span<const std::byte> data) {
    if (fd_ < 0) return;

    write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());

    if (connected_ && !write_buffer_.empty()) {
        HandleWritable();
    }
}

void TcpSocket::Close() {
    if (fd_ >= 0) {
        reactor_->Remove(fd_);
        close(fd_);
        fd_ = -1;
    }
    connected_ = false;
    write_buffer_.clear();
}

void TcpSocket::HandleEvents(uint32_t events) {
    // Handle errors first
    if (events & (EPOLLERR | EPOLLHUP)) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
        auto ec = std::error_code(err ? err : ECONNRESET, std::system_category());

        if (on_error_) {
            on_error_(ec);
        }
        Close();
        return;
    }

    if (events & EPOLLIN) {
        HandleReadable();
    }

    if (events & EPOLLOUT) {
        HandleWritable();
    }
}

void TcpSocket::HandleReadable() {
    while (true) {
        ssize_t n = read(fd_, read_buffer_.data(), read_buffer_.size());
        if (n > 0) {
            if (on_read_) {
                on_read_(std::span{read_buffer_.data(), static_cast<size_t>(n)});
            }
        } else if (n == 0) {
            // EOF
            if (on_error_) {
                on_error_(std::make_error_code(std::errc::connection_reset));
            }
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // No more data
            }
            if (on_error_) {
                on_error_(std::error_code(errno, std::system_category()));
            }
            break;
        }
    }
}

void TcpSocket::HandleWritable() {
    // Complete connection if not yet connected
    if (!connected_) {
        connected_ = true;
        if (on_connect_) {
            on_connect_();
        }
    }

    // Drain write buffer
    while (!write_buffer_.empty()) {
        ssize_t n = write(fd_, write_buffer_.data(), write_buffer_.size());
        if (n > 0) {
            write_buffer_.erase(write_buffer_.begin(),
                                write_buffer_.begin() + n);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // Would block, wait for next EPOLLOUT
            }
            if (on_error_) {
                on_error_(std::error_code(errno, std::system_category()));
            }
            break;
        }
    }

    if (write_buffer_.empty() && on_write_) {
        on_write_();
    }
}

}  // namespace databento_async
