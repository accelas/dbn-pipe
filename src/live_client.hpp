// src/live_client.hpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "data_source.hpp"
#include "reactor.hpp"
#include "tcp_socket.hpp"

namespace databento_async {

class LiveClient : public DataSource {
public:
    enum class State {
        Disconnected,
        Connecting,
        WaitingGreeting,
        WaitingChallenge,
        Authenticating,
        Ready,      // authenticated, can subscribe
        Streaming   // after Start()
    };

    LiveClient(Reactor* reactor, std::string api_key);
    ~LiveClient() override;

    // Non-copyable, non-movable
    LiveClient(const LiveClient&) = delete;
    LiveClient& operator=(const LiveClient&) = delete;
    LiveClient(LiveClient&&) = delete;
    LiveClient& operator=(LiveClient&&) = delete;

    // Connection (caller responsible for DNS resolution)
    void Connect(const sockaddr_storage& addr);
    void Close();

    // Subscription (call after authenticated, before Start)
    void Subscribe(std::string_view dataset,
                   std::string_view symbols,
                   std::string_view schema);

    // Begin streaming (after Subscribe)
    void Start() override;
    void Stop() override;

    // State
    State GetState() const { return state_; }

    // For testing: get session_id after authentication
    const std::string& GetSessionId() const { return session_id_; }

private:
    void HandleRead(std::span<const std::byte> data, std::error_code ec);
    void HandleConnect(std::error_code ec);

    void ProcessLine(std::string_view line);
    void HandleGreeting(std::string_view line);
    void HandleChallenge(std::string_view line);
    void HandleAuthResponse(std::string_view line);

    void SendAuth(std::string_view challenge);
    void SendSubscription();
    void SendStart();

    Reactor* reactor_;
    TcpSocket socket_;
    std::string api_key_;
    State state_ = State::Disconnected;

    std::string session_id_;
    std::string line_buffer_;  // for text protocol parsing

    // Subscription params
    std::string dataset_;
    std::string symbols_;
    std::string schema_;

    // Subscription counter for unique IDs
    std::uint32_t sub_counter_ = 0;
};

}  // namespace databento_async
