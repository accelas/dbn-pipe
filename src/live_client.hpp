// src/live_client.hpp
#pragma once

#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <databento/record.hpp>

#include "buffer_chain.hpp"
#include "dbn_parser_component.hpp"
#include "error.hpp"
#include "cram_auth.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"
#include "record_batch.hpp"
#include "tcp_socket.hpp"

namespace databento_async {

// LiveClient - orchestrates pipeline for live data streaming.
//
// Architecture: TcpSocket -> CramAuth -> DbnParserComponent -> Sink
//
// The Sink is an inner class that bridges parsed records back to the client's
// callbacks (OnRecord/OnError/OnComplete).
//
// Backpressure:
// - Implements Suspendable interface for flow control
// - Suspend()/Resume()/Close() MUST be called from reactor thread only
// - IsSuspended() is thread-safe (can be called from any thread)
// - suspended_ is the single source of truth for suspend state
//
// To call Suspend/Resume from other threads, use Defer():
//   reactor_.Defer([this]() { Suspend(); });
class LiveClient : public Suspendable {
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

    // Sink class - RecordSink that bridges to LiveClient callbacks
    // Implements the RecordSink concept (OnData, OnError, OnComplete) for batch-based
    // record delivery from DbnParserComponent.
    class Sink {
    public:
        explicit Sink(LiveClient* client) : client_(client) {}

        // Invalidate sink (called when client is being destroyed)
        void Invalidate() { valid_ = false; }

        // RecordSink interface
        void OnData(RecordBatch&& batch);
        void OnError(const Error& e);
        void OnComplete();

    private:
        LiveClient* client_;
        bool valid_ = true;
    };

    // Pipeline type aliases
    using ParserType = DbnParserComponent<Sink>;
    using ProtocolType = CramAuth<ParserType>;

    LiveClient(Reactor& reactor, std::string api_key);
    ~LiveClient();

    // Non-copyable, non-movable
    LiveClient(const LiveClient&) = delete;
    LiveClient& operator=(const LiveClient&) = delete;
    LiveClient(LiveClient&&) = delete;
    LiveClient& operator=(LiveClient&&) = delete;

    // Connection (caller responsible for DNS resolution)
    void Connect(const sockaddr_storage& addr);

    // Subscription (call after authenticated, before Start)
    void Subscribe(std::string_view dataset,
                   std::string_view symbols,
                   std::string_view schema);

    // Begin streaming (after Subscribe)
    void Start();
    void Stop();

    // Suspendable interface (idempotent, REACTOR THREAD ONLY)
    // Pause reading from the network. Idempotent - multiple calls are safe.
    void Suspend() override;

    // Resume reading from the network. Idempotent - multiple calls are safe.
    void Resume() override;

    // Terminate the connection. After Close(), no more callbacks will be invoked.
    void Close() override;

    // Query whether reading is currently suspended (thread-safe, any thread).
    bool IsSuspended() const override {
        return suspended_.load(std::memory_order_acquire);
    }

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
    void HandleRead(BufferChain data);

    // Map CramAuthState to LiveClient::State
    void UpdateStateFromProtocol();

    Reactor& reactor_;
    std::string api_key_;
    State state_ = State::Disconnected;
    std::string session_id_;

    // Backpressure state - single source of truth for suspend status
    // Atomic for thread-safe IsSuspended() queries from any thread
    std::atomic<bool> suspended_{false};

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
