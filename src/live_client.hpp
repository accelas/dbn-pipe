// src/live_client.hpp
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <databento/record.hpp>

#include "dbn_parser_component.hpp"
#include "error.hpp"
#include "live_protocol_handler.hpp"
#include "reactor.hpp"
#include "tcp_socket.hpp"

namespace databento_async {

// LiveClient - orchestrates pipeline for live data streaming.
//
// Architecture: TcpSocket -> LiveProtocolHandler -> DbnParserComponent -> Sink
//
// The Sink is an inner class that bridges parsed records back to the client's
// callbacks (OnRecord/OnError/OnComplete).
class LiveClient {
public:
    enum class State {
        Disconnected,
        Connecting,
        WaitingGreeting,
        WaitingChallenge,
        Authenticating,
        Ready,      // authenticated, can subscribe
        Streaming,  // after Start()
        Complete,   // stream finished normally
        Error       // stream finished with error
    };

    // Sink class - RecordDownstream that bridges to LiveClient callbacks
    class Sink {
    public:
        explicit Sink(LiveClient* client) : client_(client) {}

        // Invalidate sink (called when client is being destroyed)
        void Invalidate() { valid_ = false; }

        // RecordDownstream interface
        void OnRecord(const databento::Record& rec);
        void OnError(const Error& e);
        void OnDone();

    private:
        LiveClient* client_;
        bool valid_ = true;
    };

    // Pipeline type aliases
    using ParserType = DbnParserComponent<Sink>;
    using ProtocolType = LiveProtocolHandler<ParserType>;

    LiveClient(Reactor& reactor, std::string api_key);
    ~LiveClient();

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
    void Start();
    void Stop();

    // Backpressure control
    void Pause();
    void Resume();

    // State
    State GetState() const { return state_; }

    // For testing: get session_id after authentication
    const std::string& GetSessionId() const { return session_id_; }

    // Callbacks
    template <typename Handler>
    void OnRecord(Handler&& h) {
        record_handler_ = std::forward<Handler>(h);
    }

    template <typename Handler>
    void OnError(Handler&& h) {
        error_handler_ = std::forward<Handler>(h);
    }

    template <typename Handler>
    void OnComplete(Handler&& h) {
        complete_handler_ = std::forward<Handler>(h);
    }

private:
    // Build the pipeline components
    void BuildPipeline();

    // Clean up pipeline components
    void TeardownPipeline();

    // Handle TCP socket events
    void HandleConnect();
    void HandleSocketError(std::error_code ec);
    void HandleRead(std::span<const std::byte> data);

    // Map LiveProtocolState to LiveClient::State
    void UpdateStateFromProtocol();

    Reactor& reactor_;
    std::string api_key_;
    State state_ = State::Disconnected;
    std::string session_id_;

    // Pipeline components
    std::unique_ptr<TcpSocket> tcp_;
    std::shared_ptr<Sink> sink_;
    std::shared_ptr<ParserType> parser_;
    std::shared_ptr<ProtocolType> protocol_;

    // Subscription params (stored until pipeline is ready)
    std::string dataset_;
    std::string symbols_;
    std::string schema_;

    // Callbacks
    std::function<void(const databento::Record&)> record_handler_;
    std::function<void(const Error&)> error_handler_;
    std::function<void()> complete_handler_;
};

}  // namespace databento_async
