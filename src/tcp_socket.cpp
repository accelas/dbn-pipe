// src/tcp_socket.cpp
#include "tcp_socket.hpp"

#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace databento_async {

TcpSocket::TcpSocket(Reactor& reactor)
    : reactor_(reactor) {}

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

    int ret = connect(sock_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
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

void TcpSocket::Write(BufferChain data) {
    if (!event_) return;

    // Extract bytes from chain into write buffer
    while (!data.Empty()) {
        size_t chunk_size = data.ContiguousSize();
        const std::byte* ptr = data.DataAt(0);
        write_buffer_.insert(write_buffer_.end(), ptr, ptr + chunk_size);
        data.Consume(chunk_size);
    }

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
    read_paused_actual_ = false;
    sync_pending_ = false;
    write_buffer_.clear();
}

void TcpSocket::PauseRead() {
    if (read_paused_ || !event_) return;
    read_paused_ = true;
    if (!sync_pending_) {
        sync_pending_ = true;
        reactor_.Defer([this]() { SyncReadState(); });
    }
}

void TcpSocket::ResumeRead() {
    if (!read_paused_ || !event_) return;
    read_paused_ = false;
    if (!sync_pending_) {
        sync_pending_ = true;
        reactor_.Defer([this]() { SyncReadState(); });
    }
}

void TcpSocket::SyncReadState() {
    sync_pending_ = false;
    if (!event_) return;
    if (read_paused_ == read_paused_actual_) return;  // No change needed
    read_paused_actual_ = read_paused_;
    if (read_paused_) {
        event_->Modify(EPOLLOUT | EPOLLET);
    } else {
        event_->Modify(EPOLLOUT | EPOLLIN | EPOLLET);
    }
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
    BufferChain chain;
    chain.SetRecycleCallback(segment_pool_.MakeRecycler());

    while (true) {
        auto seg = segment_pool_.Acquire();
        ssize_t n = read(fd(), seg->data.data(), Segment::kSize);
        if (n > 0) {
            seg->size = static_cast<size_t>(n);
            chain.Append(std::move(seg));
        } else if (n == 0) {
            // EOF - deliver accumulated data first, then signal error
            if (!chain.Empty()) {
                on_read_(std::move(chain));
            }
            on_error_(std::make_error_code(std::errc::connection_reset));
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // No more data available
            }
            // Error - deliver accumulated data first, then signal error
            if (!chain.Empty()) {
                on_read_(std::move(chain));
            }
            on_error_(std::error_code(errno, std::system_category()));
            return;
        }
    }

    // Deliver accumulated data
    if (!chain.Empty()) {
        on_read_(std::move(chain));
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
