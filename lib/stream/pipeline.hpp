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

    Pipeline(
        PrivateTag,
        IEventLoop& loop,
        std::shared_ptr<typename P::ChainType> chain,
        std::shared_ptr<typename P::SinkType> sink,
        typename P::Request request
    ) : loop_(loop),
        chain_(std::move(chain)),
        sink_(std::move(sink)),
        request_(std::move(request)) {}

    ~Pipeline() {
        DoTeardown();
    }

    // Connect using protocol-derived hostname/port
    void Connect() {
        RequireLoopThread(__func__);
        if (torn_down_) return;

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
        if (torn_down_) return;
        if (chain_) chain_->Connect(addr);
    }

    // Start streaming (sends protocol request)
    void Start() {
        RequireLoopThread(__func__);
        if (torn_down_) return;
        P::SendRequest(chain_, request_);
    }

    // Stop and teardown
    void Stop() {
        RequireLoopThread(__func__);
        DoTeardown();
    }

    // Suspendable interface
    void Suspend() override {
        RequireLoopThread(__func__);
        if (!torn_down_ && chain_) chain_->Suspend();
    }

    void Resume() override {
        RequireLoopThread(__func__);
        if (!torn_down_ && chain_) chain_->Resume();
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
        if (torn_down_) return;
        torn_down_ = true;

        // Order matters: invalidate sink first, then teardown chain
        if (sink_) sink_->Invalidate();
        if (chain_) P::Teardown(chain_);
    }

    IEventLoop& loop_;
    std::shared_ptr<typename P::ChainType> chain_;
    std::shared_ptr<typename P::SinkType> sink_;
    typename P::Request request_;
    bool torn_down_ = false;
};

}  // namespace dbn_pipe
