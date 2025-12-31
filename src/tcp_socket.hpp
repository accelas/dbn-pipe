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
#include "event_loop.hpp"

namespace dbn_pipe {

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

    TcpSocket(IEventLoop& loop, std::shared_ptr<D> downstream)
        : loop_(loop), downstream_(std::move(downstream)) {}

    ~TcpSocket() override { Close(); }

    // Non-copyable, non-movable
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&&) = delete;
    TcpSocket& operator=(TcpSocket&&) = delete;

    // Factory for shared_ptr (consistent with other components)
    // Wires up upstream write callback and backpressure after construction.
    static std::shared_ptr<TcpSocket> Create(IEventLoop& loop, std::shared_ptr<D> downstream) {
        auto tcp = std::make_shared<TcpSocket>(loop, std::move(downstream));
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

        // Start with read+write to detect connect completion and early errors
        fd_ = sock_fd;
        handle_ = loop_.Register(
            sock_fd,
            /*want_read=*/true,
            /*want_write=*/true,  // For connect completion
            [this]() { HandleReadable(); },
            [this]() { HandleWritable(); },
            [this](int err) {
                downstream_->OnError(Error{ErrorCode::ConnectionFailed,
                                           "socket error", err});
                Close();
            }
        );
    }

    // Write data to socket (queued if not yet writable)
    void Write(BufferChain data) {
        if (!handle_) return;

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
        handle_.reset();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        connected_ = false;
        read_paused_ = false;
        watching_write_ = false;
        suspend_count_ = 0;
        write_buffer_.clear();
    }

    // Callback for connect completion
    template <typename F>
        requires std::invocable<F>
    void OnConnect(F&& cb) { on_connect_ = std::forward<F>(cb); }

    // State
    bool IsConnected() const { return connected_; }
    int fd() const { return fd_; }

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
            // Switch to read-only mode after connect (no EPOLLOUT unless writes pending)
            UpdateEpollFlags();
            on_connect_();
        }

        while (!write_buffer_.empty()) {
            ssize_t n = write(fd(), write_buffer_.data(), write_buffer_.size());
            if (n > 0) {
                write_buffer_.erase(write_buffer_.begin(),
                                    write_buffer_.begin() + n);
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Need to wait for writability - ensure EPOLLOUT is set
                    if (!watching_write_) {
                        watching_write_ = true;
                        UpdateEpollFlags();
                    }
                    return;
                }
                downstream_->OnError(Error{ErrorCode::ConnectionFailed,
                                           "write() failed", errno});
                return;
            }
        }

        // Write buffer drained - stop watching for writability
        if (watching_write_) {
            watching_write_ = false;
            UpdateEpollFlags();
        }
    }

    void PauseRead() {
        if (read_paused_ || !handle_) return;
        read_paused_ = true;
        UpdateEpollFlags();
    }

    void ResumeRead() {
        if (!read_paused_ || !handle_) return;
        read_paused_ = false;
        UpdateEpollFlags();
    }

    // Compute and apply correct epoll flags based on current state
    void UpdateEpollFlags() {
        if (!handle_ || !connected_) return;
        handle_->Update(!read_paused_, watching_write_);
    }

    IEventLoop& loop_;
    std::shared_ptr<D> downstream_;
    std::unique_ptr<IEventHandle> handle_;
    int fd_ = -1;
    bool connected_ = false;
    bool read_paused_ = false;
    bool watching_write_ = false;
    int suspend_count_ = 0;

    std::vector<std::byte> write_buffer_;
    SegmentPool segment_pool_{4};

    ConnectCallback on_connect_ = []() {};
};

}  // namespace dbn_pipe
