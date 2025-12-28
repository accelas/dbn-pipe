// src/tcp_socket.hpp
#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <functional>
#include <span>
#include <system_error>
#include <vector>

namespace databento_async {

class Reactor;

class TcpSocket {
public:
    using ConnectCallback = std::function<void()>;
    using ReadCallback = std::function<void(std::span<const std::byte>)>;
    using WriteCallback = std::function<void()>;
    using ErrorCallback = std::function<void(std::error_code)>;

    explicit TcpSocket(Reactor* reactor);
    ~TcpSocket();

    // Non-copyable, non-movable
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&&) = delete;
    TcpSocket& operator=(TcpSocket&&) = delete;

    // Connect to address (caller responsible for DNS resolution)
    void Connect(const sockaddr_storage& addr);

    // Write data (queued if not yet writable)
    void Write(std::span<const std::byte> data);

    // Close connection
    void Close();

    // Callbacks
    void OnConnect(ConnectCallback cb) { on_connect_ = std::move(cb); }
    void OnRead(ReadCallback cb) { on_read_ = std::move(cb); }
    void OnWrite(WriteCallback cb) { on_write_ = std::move(cb); }
    void OnError(ErrorCallback cb) { on_error_ = std::move(cb); }

    // State
    bool IsConnected() const { return connected_; }
    int fd() const { return fd_; }

private:
    void HandleEvents(uint32_t events);
    void HandleReadable();
    void HandleWritable();

    Reactor* reactor_;
    int fd_ = -1;
    bool connected_ = false;

    std::vector<std::byte> write_buffer_;
    std::vector<std::byte> read_buffer_;

    ConnectCallback on_connect_ = []() {};
    ReadCallback on_read_ = [](std::span<const std::byte>) {};
    WriteCallback on_write_ = []() {};
    ErrorCallback on_error_ = [](std::error_code) {};

    static constexpr size_t kReadBufferSize = 65536;
};

}  // namespace databento_async
