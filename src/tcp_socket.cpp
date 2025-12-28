// src/tcp_socket.cpp
#include "tcp_socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "reactor.hpp"

namespace databento_async {

TcpSocket::TcpSocket(Reactor* reactor)
    : reactor_(reactor), read_buffer_(kReadBufferSize) {}

TcpSocket::~TcpSocket() { Close(); }

void TcpSocket::Connect(std::string_view host, int port) {
    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        if (on_connect_) {
            on_connect_(std::error_code(errno, std::system_category()));
        }
        return;
    }

    // Disable Nagle for lower latency
    int opt = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    std::string host_str(host);
    if (inet_pton(AF_INET, host_str.c_str(), &addr.sin_addr) != 1) {
        close(fd_);
        fd_ = -1;
        if (on_connect_) {
            on_connect_(std::make_error_code(std::errc::invalid_argument));
        }
        return;
    }

    int ret = connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        auto ec = std::error_code(errno, std::system_category());
        close(fd_);
        fd_ = -1;
        if (on_connect_) {
            on_connect_(ec);
        }
        return;
    }

    connecting_ = true;
    reactor_->Add(fd_, EPOLLOUT | EPOLLIN | EPOLLET,
                  [this](uint32_t events) { HandleEvents(events); });
}

void TcpSocket::Write(std::span<const std::byte> data) {
    if (fd_ < 0) return;

    write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());

    if (connected_ && !write_buffer_.empty()) {
        HandleWrite();
    }
}

void TcpSocket::Close() {
    if (fd_ >= 0) {
        reactor_->Remove(fd_);
        close(fd_);
        fd_ = -1;
    }
    connected_ = false;
    connecting_ = false;
    write_buffer_.clear();
}

void TcpSocket::HandleEvents(uint32_t events) {
    if (connecting_) {
        HandleConnect();
        return;
    }

    if (events & (EPOLLERR | EPOLLHUP)) {
        if (on_read_) {
            on_read_({}, std::make_error_code(std::errc::connection_reset));
        }
        return;
    }

    if (events & EPOLLIN) {
        HandleRead();
    }

    if (events & EPOLLOUT) {
        HandleWrite();
    }
}

void TcpSocket::HandleConnect() {
    connecting_ = false;

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);

    if (err != 0) {
        if (on_connect_) {
            on_connect_(std::error_code(err, std::system_category()));
        }
        Close();
        return;
    }

    connected_ = true;

    if (on_connect_) {
        on_connect_({});
    }

    // Flush any pending writes
    if (!write_buffer_.empty()) {
        HandleWrite();
    }
}

void TcpSocket::HandleRead() {
    while (true) {
        ssize_t n = read(fd_, read_buffer_.data(), read_buffer_.size());
        if (n > 0) {
            if (on_read_) {
                on_read_(std::span{read_buffer_.data(), static_cast<size_t>(n)}, {});
            }
        } else if (n == 0) {
            // EOF
            if (on_read_) {
                on_read_({}, std::make_error_code(std::errc::connection_reset));
            }
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // No more data
            }
            if (on_read_) {
                on_read_({}, std::error_code(errno, std::system_category()));
            }
            break;
        }
    }
}

void TcpSocket::HandleWrite() {
    while (!write_buffer_.empty()) {
        ssize_t n = write(fd_, write_buffer_.data(), write_buffer_.size());
        if (n > 0) {
            write_buffer_.erase(write_buffer_.begin(),
                                write_buffer_.begin() + n);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // Would block, wait for EPOLLOUT
            }
            if (on_write_) {
                on_write_(std::error_code(errno, std::system_category()));
            }
            break;
        }
    }

    if (write_buffer_.empty() && on_write_) {
        on_write_({});
    }
}

}  // namespace databento_async
