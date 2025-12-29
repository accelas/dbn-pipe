// src/tcp_socket.cpp
#include "tcp_socket.hpp"

#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace databento_async {

TcpSocket::TcpSocket(Reactor& reactor)
    : reactor_(reactor), read_buffer_(kReadBufferSize) {}

TcpSocket::~TcpSocket() { Close(); }

void TcpSocket::Connect(const sockaddr_storage& addr) {
    // Create socket based on address family
    int family = addr.ss_family;
    int sock_fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock_fd < 0) {
        on_error_(std::error_code(errno, std::system_category()));
        return;
    }

    // Disable Nagle for lower latency
    int opt = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Determine sockaddr size based on family
    socklen_t addr_len = (family == AF_INET6) ? sizeof(sockaddr_in6)
                                              : sizeof(sockaddr_in);

    int ret = connect(sock_fd, reinterpret_cast<const sockaddr*>(&addr), addr_len);
    if (ret < 0 && errno != EINPROGRESS) {
        auto ec = std::error_code(errno, std::system_category());
        close(sock_fd);
        on_error_(ec);
        return;
    }

    // Create event and register with reactor
    event_ = std::make_unique<Event>(reactor_, sock_fd, EPOLLOUT | EPOLLIN | EPOLLET);
    event_->OnEvent([this](uint32_t events) { HandleEvents(events); });
}

void TcpSocket::Write(std::span<const std::byte> data) {
    if (!event_) return;

    write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());

    if (connected_ && !write_buffer_.empty()) {
        HandleWritable();
    }
}

void TcpSocket::Close() {
    if (event_) {
        int sock_fd = event_->fd();
        event_.reset();
        if (sock_fd >= 0) {
            close(sock_fd);
        }
    }
    connected_ = false;
    read_paused_ = false;
    write_buffer_.clear();
}

void TcpSocket::PauseRead() {
    if (read_paused_ || !event_) return;
    read_paused_ = true;
    event_->Modify(EPOLLOUT | EPOLLET);
}

void TcpSocket::ResumeRead() {
    if (!read_paused_ || !event_) return;
    read_paused_ = false;
    event_->Modify(EPOLLOUT | EPOLLIN | EPOLLET);
}

void TcpSocket::HandleEvents(uint32_t events) {
    // Handle errors first
    if (events & (EPOLLERR | EPOLLHUP)) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd(), SOL_SOCKET, SO_ERROR, &err, &len);
        on_error_(std::error_code(err ? err : ECONNRESET, std::system_category()));
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
        ssize_t n = read(fd(), read_buffer_.data(), read_buffer_.size());
        if (n > 0) {
            on_read_(std::span{read_buffer_.data(), static_cast<size_t>(n)});
        } else if (n == 0) {
            // EOF
            on_error_(std::make_error_code(std::errc::connection_reset));
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // No more data
            }
            on_error_(std::error_code(errno, std::system_category()));
            break;
        }
    }
}

void TcpSocket::HandleWritable() {
    // Complete connection if not yet connected
    if (!connected_) {
        connected_ = true;
        on_connect_();
    }

    // Drain write buffer
    while (!write_buffer_.empty()) {
        ssize_t n = write(fd(), write_buffer_.data(), write_buffer_.size());
        if (n > 0) {
            write_buffer_.erase(write_buffer_.begin(),
                                write_buffer_.begin() + n);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // Would block, wait for next EPOLLOUT
            }
            on_error_(std::error_code(errno, std::system_category()));
            break;
        }
    }

    if (write_buffer_.empty()) {
        on_write_();
    }
}

}  // namespace databento_async
