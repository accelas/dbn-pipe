// src/tcp_socket.cpp
#include "tcp_socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
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

    // connected_ remains false until HandleWritable confirms connection
    reactor_->Add(fd_, EPOLLOUT | EPOLLIN | EPOLLET,
                  [this](uint32_t events) { HandleEvents(events); });
}

void TcpSocket::Write(std::span<const std::byte> data) {
    if (fd_ < 0) return;

    write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());

    if (connected_ && !write_buffer_.empty()) {
        HandleWritable(EPOLLOUT);
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
    if (events & EPOLLIN) {
        HandleReadable(events);
    }

    if (events & EPOLLOUT) {
        HandleWritable(events);
    }
}

void TcpSocket::HandleReadable(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        if (on_read_) {
            on_read_({}, std::make_error_code(std::errc::connection_reset));
        }
        return;
    }

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

void TcpSocket::HandleWritable(uint32_t events) {
    // Complete connection if not yet connected
    if (!connected_) {
        if (events & (EPOLLERR | EPOLLHUP)) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
            if (on_connect_) {
                on_connect_(std::error_code(err, std::system_category()));
            }
            Close();
            return;
        }

        // Check SO_ERROR for connection result
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            if (on_connect_) {
                on_connect_(std::error_code(err ? err : errno, std::system_category()));
            }
            Close();
            return;
        }

        connected_ = true;
        if (on_connect_) {
            on_connect_({});
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
