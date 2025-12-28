// src/tcp_socket.hpp
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace databento_async {

class Reactor;

class TcpSocket {
public:
    using ConnectCallback = std::function<void(std::error_code)>;
    using ReadCallback = std::function<void(std::span<const std::byte>, std::error_code)>;
    using WriteCallback = std::function<void(std::error_code)>;

    explicit TcpSocket(Reactor* reactor);
    ~TcpSocket();

    // Non-copyable, non-movable
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&&) = delete;
    TcpSocket& operator=(TcpSocket&&) = delete;

    // Connect to host:port
    void Connect(std::string_view host, int port);

    // Write data (queued if not yet writable)
    void Write(std::span<const std::byte> data);

    // Close connection
    void Close();

    // Callbacks
    void OnConnect(ConnectCallback cb) { on_connect_ = std::move(cb); }
    void OnRead(ReadCallback cb) { on_read_ = std::move(cb); }
    void OnWrite(WriteCallback cb) { on_write_ = std::move(cb); }

    // State
    bool IsConnected() const { return connected_; }
    int fd() const { return fd_; }

private:
    void HandleEvents(uint32_t events);
    void HandleConnect();
    void HandleRead();
    void HandleWrite();

    Reactor* reactor_;
    int fd_ = -1;
    bool connected_ = false;
    bool connecting_ = false;

    std::vector<std::byte> write_buffer_;
    std::vector<std::byte> read_buffer_;

    ConnectCallback on_connect_;
    ReadCallback on_read_;
    WriteCallback on_write_;

    static constexpr size_t kReadBufferSize = 65536;
};

}  // namespace databento_async
