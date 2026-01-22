// lib/stream/pipeline.hpp
#pragma once

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "lib/stream/event_loop.hpp"
#include "lib/stream/protocol.hpp"
#include "lib/stream/suspendable.hpp"
#include "src/dns_resolver.hpp"

namespace dbn_pipe {

// Pipeline<P> - Runtime shell for any Protocol.
//
// Pipeline is a thin runtime wrapper - just lifecycle management.
// The Protocol provides construction logic via static methods.
//
// State machine:
//   Created -> Connecting -> Ready -> Started -> TornDown
//                  |           |         |
//                  +-----------+---------+-> TornDown (on error or Stop)
//
// Start() can only be called in Ready state. Calling it earlier is a programming error.
//
// Thread safety:
// - All public methods must be called from event loop thread (enforced via RequireLoopThread)
// - IsSuspended() is thread-safe, can be called from any thread
//
// Ownership:
// - Pipeline is the root object, owns both chain and sink
// - Destructor controls teardown order: invalidate sink → teardown chain
template<typename P>
    requires Protocol<P>
class Pipeline : public Suspendable {
public:
    struct PrivateTag {};  // Force use of Protocol::Create()

    enum class State {
        Created,     // Initial state after construction
        Connecting,  // Connect() called, waiting for handshake
        Ready,       // Ready callback fired, can call Start()
        Started,     // Start() called, streaming
        TornDown     // Stopped or error
    };

    Pipeline(
        PrivateTag,
        IEventLoop& loop,
        std::shared_ptr<typename P::ChainType> chain,
        std::shared_ptr<typename P::SinkType> sink,
        typename P::Request request
    ) : loop_(loop),
        chain_(std::move(chain)),
        sink_(std::move(sink)),
        request_(std::move(request)),
        state_(State::Created) {}

    ~Pipeline() {
        DoTeardown();
    }

    // Connect using protocol-derived hostname/port
    void Connect() {
        RequireLoopThread(__func__);
        if (state_ == State::TornDown) return;

        std::string hostname = P::GetHostname(request_);
        uint16_t port = P::GetPort(request_);

        auto addr = ResolveHostname(hostname, port);
        if (!addr) {
            if (sink_) sink_->OnError(Error{ErrorCode::DnsResolutionFailed,
                "Failed to resolve hostname: " + hostname});
            DoTeardown();
            return;
        }

        Connect(*addr);
    }

    // Connect to specific address
    void Connect(const sockaddr_storage& addr) {
        RequireLoopThread(__func__);
        if (state_ == State::TornDown) return;
        if (state_ != State::Created) {
            std::fprintf(stderr, "Pipeline::Connect called in invalid state\n");
            std::terminate();
        }
        state_ = State::Connecting;
        if (chain_) chain_->Connect(addr);
    }

    // Mark pipeline as ready (called by ready callback)
    void MarkReady() {
        RequireLoopThread(__func__);
        if (state_ == State::TornDown) return;
        if (state_ != State::Connecting) {
            std::fprintf(stderr, "Pipeline::MarkReady called in invalid state\n");
            std::terminate();
        }
        state_ = State::Ready;
    }

    // Start streaming (sends protocol request)
    // Must be called after MarkReady() - typically from the ready callback
    void Start() {
        RequireLoopThread(__func__);
        if (state_ == State::TornDown) return;
        if (state_ != State::Ready) {
            std::fprintf(stderr, "Pipeline::Start called before ready (state=%d)\n",
                static_cast<int>(state_));
            std::terminate();
        }
        state_ = State::Started;
        P::SendRequest(chain_, request_);
    }

    // Get current state (for testing/debugging)
    State GetState() const { return state_; }

    // Stop and teardown - can be called in any state
    void Stop() {
        RequireLoopThread(__func__);
        DoTeardown();
    }

    // Check if pipeline is ready for Start()
    bool IsReady() const { return state_ == State::Ready; }

    // Suspendable interface
    void Suspend() override {
        RequireLoopThread(__func__);
        if (state_ != State::TornDown && chain_) chain_->Suspend();
    }

    void Resume() override {
        RequireLoopThread(__func__);
        if (state_ != State::TornDown && chain_) chain_->Resume();
    }

    void Close() override {
        RequireLoopThread(__func__);
        DoTeardown();
    }

    bool IsSuspended() const override {
        // Thread-safe - can be called from any thread
        return chain_ && chain_->IsSuspended();
    }

private:
    // Fail-fast thread check - works in release builds
    void RequireLoopThread(const char* func) const {
        if (loop_.IsInEventLoopThread()) return;
        std::fprintf(stderr, "Pipeline::%s called off event loop thread\n", func);
        std::terminate();
    }

    // Idempotent teardown - safe to call multiple times
    void DoTeardown() {
        if (state_ == State::TornDown) return;
        state_ = State::TornDown;

        // Order matters: invalidate sink first, then teardown chain
        if (sink_) sink_->Invalidate();
        if (chain_) P::Teardown(chain_);
    }

    IEventLoop& loop_;
    std::shared_ptr<typename P::ChainType> chain_;
    std::shared_ptr<typename P::SinkType> sink_;
    typename P::Request request_;
    State state_;
};

}  // namespace dbn_pipe
