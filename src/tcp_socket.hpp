// src/tcp_socket.hpp
#pragma once

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <system_error>
#include <vector>

#include "buffer_chain.hpp"
#include "error.hpp"
#include "pipeline_component.hpp"
#include "reactor.hpp"

namespace databento_async {

// TcpSocket - Network socket as a chain component
//
// Template parameter Downstream receives data via OnData/OnError/OnDone.
// Static dispatch for all downstream calls.
//
// Data flow: Network -> TcpSocket -> Downstream
// Backpressure: TcpSocket <- (via Suspend/Resume)
// Write path: Downstream -> TcpSocket (via SetUpstreamWriteCallback)
template <Downstream D>
class TcpSocket : public Suspendable,
                  public std::enable_shared_from_this<TcpSocket<D>> {
public:
    using ConnectCallback = std::function<void()>;
    using WriteCallback = std::function<void()>;

    TcpSocket(Reactor& reactor, std::shared_ptr<D> downstream)
        : reactor_(reactor), downstream_(std::move(downstream)) {}

    ~TcpSocket() override { Close(); }

    // Non-copyable, non-movable
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&&) = delete;
    TcpSocket& operator=(TcpSocket&&) = delete;

    // Factory for shared_ptr (consistent with other components)
    // Wires up upstream write callback and backpressure after construction.
    static std::shared_ptr<TcpSocket> Create(Reactor& reactor, std::shared_ptr<D> downstream) {
        auto tcp = std::make_shared<TcpSocket>(reactor, std::move(downstream));
        tcp->WireDownstream();
        return tcp;
    }

    // Wire downstream for write callbacks and backpressure.
    // Called by Create() after shared_ptr is available.
    void WireDownstream() {
        // Wire upstream write callback if downstream supports it
        if constexpr (requires(D& d) {
            d.SetUpstreamWriteCallback(std::function<void(BufferChain)>{});
        }) {
            std::weak_ptr<TcpSocket> weak_self = this->shared_from_this();
            downstream_->SetUpstreamWriteCallback([weak_self](BufferChain data) {
                if (auto self = weak_self.lock()) {
                    self->Write(std::move(data));
                }
            });
        }

        // Set upstream for backpressure if downstream supports it
        if constexpr (requires(D& d) { d.SetUpstream(static_cast<Suspendable*>(nullptr)); }) {
            downstream_->SetUpstream(this);
        }
    }

    // Connect to address (caller responsible for DNS resolution)
    void Connect(const sockaddr_storage& addr) {
        int family = addr.ss_family;
        int sock_fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (sock_fd < 0) {
            downstream_->OnError(Error{ErrorCode::ConnectionFailed,
                                       "socket() failed", errno});
            return;
        }

        // Disable Nagle for lower latency
        int opt = 1;
        setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        int ret = connect(sock_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (ret < 0 && errno != EINPROGRESS) {
            auto err = errno;
            ::close(sock_fd);
            downstream_->OnError(Error{ErrorCode::ConnectionFailed,
                                       "connect() failed", err});
            return;
        }

        event_ = std::make_unique<Event>(reactor_, sock_fd, EPOLLOUT | EPOLLIN | EPOLLET);
        event_->OnEvent([this](uint32_t events) { HandleEvents(events); });
    }

    // Write data to socket (queued if not yet writable)
    void Write(BufferChain data) {
        if (!event_) return;

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

    // Close connection (implements Suspendable::Close)
    void Close() override {
        if (event_) {
            int sock_fd = event_->fd();
            event_.reset();
            if (sock_fd >= 0) {
                ::close(sock_fd);
            }
        }
        connected_ = false;
        read_paused_ = false;
        suspend_count_ = 0;
        write_buffer_.clear();
    }

    // Callbacks for connect/write completion (kept for compatibility)
    template <typename F>
        requires std::invocable<F>
    void OnConnect(F&& cb) { on_connect_ = std::forward<F>(cb); }

    template <typename F>
        requires std::invocable<F>
    void OnWrite(F&& cb) { on_write_ = std::forward<F>(cb); }

    // State
    bool IsConnected() const { return connected_; }
    int fd() const { return event_ ? event_->fd() : -1; }

    // =========================================================================
    // Suspendable interface
    // =========================================================================

    void Suspend() override {
        if (++suspend_count_ == 1) {
            PauseRead();
        }
    }

    void Resume() override {
        assert(suspend_count_ > 0);
        if (--suspend_count_ == 0) {
            ResumeRead();
        }
    }

    bool IsSuspended() const override { return suspend_count_ > 0; }

private:
    void HandleEvents(uint32_t events) {
        if (events & (EPOLLERR | EPOLLHUP)) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd(), SOL_SOCKET, SO_ERROR, &err, &len);
            downstream_->OnError(Error{ErrorCode::ConnectionFailed,
                                       "socket error", err ? err : ECONNRESET});
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

    void HandleReadable() {
        BufferChain chain;
        chain.SetRecycleCallback(segment_pool_.MakeRecycler());

        while (true) {
            auto seg = segment_pool_.Acquire();
            ssize_t n = read(fd(), seg->data.data(), Segment::kSize);
            if (n > 0) {
                seg->size = static_cast<size_t>(n);
                chain.Append(std::move(seg));
            } else if (n == 0) {
                // EOF - deliver accumulated data first, then signal done
                if (!chain.Empty()) {
                    downstream_->OnData(chain);
                }
                downstream_->OnDone();
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                // Error - deliver accumulated data first, then signal error
                if (!chain.Empty()) {
                    downstream_->OnData(chain);
                }
                downstream_->OnError(Error{ErrorCode::ConnectionFailed,
                                           "read() failed", errno});
                return;
            }
        }

        if (!chain.Empty()) {
            downstream_->OnData(chain);
        }
    }

    void HandleWritable() {
        if (!connected_) {
            connected_ = true;
            on_connect_();
        }

        while (!write_buffer_.empty()) {
            ssize_t n = write(fd(), write_buffer_.data(), write_buffer_.size());
            if (n > 0) {
                write_buffer_.erase(write_buffer_.begin(),
                                    write_buffer_.begin() + n);
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                downstream_->OnError(Error{ErrorCode::ConnectionFailed,
                                           "write() failed", errno});
                break;
            }
        }

        if (write_buffer_.empty()) {
            on_write_();
        }
    }

    void PauseRead() {
        if (read_paused_ || !event_) return;
        read_paused_ = true;
        event_->Modify(EPOLLOUT | EPOLLET);
    }

    void ResumeRead() {
        if (!read_paused_ || !event_) return;
        read_paused_ = false;
        event_->Modify(EPOLLOUT | EPOLLIN | EPOLLET);
    }

    Reactor& reactor_;
    std::shared_ptr<D> downstream_;
    std::unique_ptr<Event> event_;
    bool connected_ = false;
    bool read_paused_ = false;
    int suspend_count_ = 0;

    std::vector<std::byte> write_buffer_;
    SegmentPool segment_pool_{4};

    ConnectCallback on_connect_ = []() {};
    WriteCallback on_write_ = []() {};
};

}  // namespace databento_async
