// src/tcp_socket.hpp
#pragma once

#include <netinet/in.h>

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <system_error>
#include <vector>

#include "buffer_chain.hpp"
#include "reactor.hpp"

namespace databento_async {

// Callback concepts
template<typename F>
concept ConnectHandler = std::invocable<F>;

template<typename F>
concept ReadHandler = std::invocable<F, BufferChain>;

template<typename F>
concept WriteHandler = std::invocable<F>;

template<typename F>
concept ErrorHandler = std::invocable<F, std::error_code>;

class TcpSocket {
public:
    using ConnectCallback = std::function<void()>;
    using ReadCallback = std::function<void(BufferChain)>;
    using WriteCallback = std::function<void()>;
    using ErrorCallback = std::function<void(std::error_code)>;

    explicit TcpSocket(Reactor& reactor);
    ~TcpSocket();

    // Non-copyable, non-movable
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&&) = delete;
    TcpSocket& operator=(TcpSocket&&) = delete;

    // Connect to address (caller responsible for DNS resolution)
    void Connect(const sockaddr_storage& addr);

    // Write data (queued if not yet writable)
    void Write(BufferChain data);

    // Close connection
    void Close();

    // Callbacks
    template<ConnectHandler F>
    void OnConnect(F&& cb) { on_connect_ = std::forward<F>(cb); }

    template<ReadHandler F>
    void OnRead(F&& cb) { on_read_ = std::forward<F>(cb); }

    template<WriteHandler F>
    void OnWrite(F&& cb) { on_write_ = std::forward<F>(cb); }

    template<ErrorHandler F>
    void OnError(F&& cb) { on_error_ = std::forward<F>(cb); }

    // State
    bool IsConnected() const { return connected_; }
    int fd() const { return event_ ? event_->fd() : -1; }

    // Backpressure control
    void PauseRead();
    void ResumeRead();
    bool IsReadPaused() const { return read_paused_; }

private:
    void HandleEvents(uint32_t events);
    void HandleReadable();
    void HandleWritable();

    Reactor& reactor_;
    std::unique_ptr<Event> event_;
    bool connected_ = false;
    bool read_paused_ = false;

    std::vector<std::byte> write_buffer_;
    SegmentPool segment_pool_{4};  // Pool for zero-copy reads

    ConnectCallback on_connect_ = []() {};
    ReadCallback on_read_ = [](BufferChain) {};
    WriteCallback on_write_ = []() {};
    ErrorCallback on_error_ = [](std::error_code) {};
};

}  // namespace databento_async
