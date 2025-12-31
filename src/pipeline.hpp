// src/pipeline.hpp
#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

#include <databento/record.hpp>

#include "dns_resolver.hpp"
#include "error.hpp"
#include "event_loop.hpp"
#include "pipeline_component.hpp"
#include "pipeline_sink.hpp"
#include "protocol_driver.hpp"

namespace databento_async {

// Pipeline states
enum class PipelineState {
    Disconnected,  // Initial state, or after Stop()
    Connecting,    // Connect() called, waiting for TCP connect
    Connected,     // TCP connected, ready for Start()
    Streaming,     // Start() called, streaming data
    Stopping,      // Stop() called, tearing down
    Done,          // Complete - stream finished normally
    Error          // Terminal error occurred
};

// Pipeline<P, Record> - Unified pipeline template for any ProtocolDriver.
//
// This is the core component of the unified design. It works with any protocol
// driver that satisfies the ProtocolDriver concept.
//
// Lifecycle: Single-use. For reconnect, create a new Pipeline instance.
//
// Thread safety: All public methods must be called from reactor thread.
// IsSuspended() is the only exception - it's thread-safe.
//
// Template parameters:
//   P - Protocol driver (e.g., LiveProtocol, HistoricalProtocol)
//   Record - Record type passed through the Sink
template <typename P, typename Record>
    requires ProtocolDriver<P, Record>
class Pipeline : public PipelineBase<Record>,
                 public Suspendable,
                 public std::enable_shared_from_this<Pipeline<P, Record>> {
    // Private constructor - must use Create() for shared_ptr safety
    // (weak_from_this requires shared_ptr ownership)
    struct PrivateTag {};

public:
    using Request = typename P::Request;

    // Factory method required for shared_from_this safety.
    // weak_from_this() only works when the object is owned by a shared_ptr.
    static std::shared_ptr<Pipeline> Create(IEventLoop& loop, std::string api_key) {
        return std::make_shared<Pipeline>(PrivateTag{}, loop, std::move(api_key));
    }

    // Constructor is "public" for make_shared but requires PrivateTag.
    // This prevents direct construction while allowing make_shared to work.
    Pipeline(PrivateTag, IEventLoop& loop, std::string api_key)
        : loop_(loop), api_key_(std::move(api_key)) {}

    // Destructor must run on event loop thread because DoTeardown touches
    // TcpSocket and chain components that have event loop thread affinity.
    // Ensure the last shared_ptr release happens on event loop thread.
    ~Pipeline() {
        // Note: In test scenarios, reactor thread ID may not be set
        // if Poll() was never called. We still need cleanup to work.
        // Invalidate sink first to prevent callbacks during teardown
        // (P::Teardown might trigger Close() which emits OnComplete)
        if (sink_) sink_->Invalidate();
        DoTeardown();  // Synchronous cleanup
    }

    // =========================================================================
    // Setup methods - call before Connect()
    // =========================================================================

    // Set the request parameters. Must be called before Start().
    void SetRequest(Request params) {
        assert(loop_.IsInEventLoopThread());
        request_ = std::move(params);
        request_set_ = true;
    }

    // Set callback for received records (one at a time).
    template <typename H>
        requires std::invocable<H, const Record&>
    void OnRecord(H&& h) {
        assert(loop_.IsInEventLoopThread());
        record_handler_ = std::forward<H>(h);
    }

    // Set callback for record batches (efficient bulk delivery).
    // If set, per-record callback is ignored - batches go directly to this handler.
    template <typename H>
        requires std::invocable<H, RecordBatch&&>
    void OnRecord(H&& h) {
        assert(loop_.IsInEventLoopThread());
        batch_handler_ = std::forward<H>(h);
    }

    // Set callback for errors.
    template <typename H>
    void OnError(H&& h) {
        assert(loop_.IsInEventLoopThread());
        error_handler_ = std::forward<H>(h);
    }

    // Set callback for stream completion.
    template <typename H>
    void OnComplete(H&& h) {
        assert(loop_.IsInEventLoopThread());
        complete_handler_ = std::forward<H>(h);
    }

    // =========================================================================
    // Control methods
    // =========================================================================

    // Connect using protocol-derived hostname/port from request.
    // SetRequest() must be called first.
    // Does synchronous DNS resolution (blocking).
    void Connect() {
        assert(loop_.IsInEventLoopThread());

        // Terminal state guard
        if (teardown_pending_ || state_ == PipelineState::Error ||
            state_ == PipelineState::Done) {
            return;
        }

        if (!request_set_) {
            HandlePipelineError(Error{ErrorCode::InvalidState,
                                      "SetRequest() must be called before Connect()"});
            return;
        }

        std::string hostname = P::GetHostname(request_);
        uint16_t port = P::GetPort(request_);

        auto addr = ResolveHostname(hostname, port);
        if (!addr) {
            HandlePipelineError(Error{ErrorCode::DnsResolutionFailed,
                                      "Failed to resolve hostname: " + hostname});
            return;
        }

        Connect(*addr);
    }

    // Connect to the given address. Must be called once before Start().
    // Terminal state guard - silently ignores if pipeline is done.
    void Connect(const sockaddr_storage& addr) {
        assert(loop_.IsInEventLoopThread());

        // Terminal state guard - can't connect after teardown or terminal error
        if (teardown_pending_ || state_ == PipelineState::Error ||
            state_ == PipelineState::Done) {
            return;  // Silently ignore - pipeline is done
        }

        if (connected_) {
            // Invalid state is terminal - use HandlePipelineError for consistency
            HandlePipelineError(Error{ErrorCode::InvalidState,
                                      "Connect() can only be called once"});
            return;
        }
        BuildPipeline();
        state_ = PipelineState::Connecting;
        chain_->Connect(addr);
    }

    // Start streaming. Must be called after Connect() and SetRequest().
    // Terminal state guard - silently ignores if pipeline is done.
    void Start() {
        assert(loop_.IsInEventLoopThread());

        // Terminal state guard - pipeline is single-use
        // Note: Disconnected is NOT terminal - it's the initial state.
        // teardown_pending_ distinguishes "initial" from "stopped".
        if (teardown_pending_ || state_ == PipelineState::Error ||
            state_ == PipelineState::Done) {
            return;  // Silently ignore - pipeline is done
        }

        if (!request_set_) {
            HandlePipelineError(Error{ErrorCode::InvalidState,
                                      "SetRequest() must be called before Start()"});
            return;
        }
        if (!connected_) {
            HandlePipelineError(Error{ErrorCode::InvalidState,
                                      "Connect() must be called before Start()"});
            return;
        }

        if (ready_to_send_ && !request_sent_) {
            P::SendRequest(chain_, request_);
            request_sent_ = true;
            state_ = PipelineState::Streaming;
        }
        // If not ready yet, Start() is a no-op - request will be sent
        // when OnRead returns true (handshake complete)
        start_requested_ = true;
    }

    // Stop is terminal. State returns to Disconnected which is also the initial state.
    // Use teardown_pending_ to distinguish "never started" from "stopped":
    //   - Initial: state_ == Disconnected && !teardown_pending_
    //   - Stopped: state_ == Disconnected && teardown_pending_
    void Stop() {
        assert(loop_.IsInEventLoopThread());
        if (IsTerminal()) return;  // Already done or errored
        state_ = PipelineState::Stopping;  // Transition to Stopping
        TeardownPipeline();
    }

    // =========================================================================
    // Suspendable interface - propagates through chain to TCP
    // =========================================================================

    // Increment suspend count. When count goes 0->1, propagate to chain.
    void Suspend() override {
        assert(loop_.IsInEventLoopThread());
        if (++suspend_count_ == 1 && chain_) {
            chain_->Suspend();
        }
    }

    // Decrement suspend count. When count goes 1->0, propagate to chain.
    void Resume() override {
        assert(loop_.IsInEventLoopThread());
        assert(suspend_count_ > 0);
        if (--suspend_count_ == 0 && chain_) {
            chain_->Resume();
        }
    }

    // Terminate the connection via TeardownPipeline.
    void Close() override {
        assert(loop_.IsInEventLoopThread());
        TeardownPipeline();
    }

    // IsSuspended is thread-safe (atomic read) unlike other methods.
    // Can be called from any thread to check backpressure state.
    bool IsSuspended() const override {
        return suspend_count_.load(std::memory_order_acquire) > 0;
    }

    // =========================================================================
    // State accessors
    // =========================================================================

    // Get current pipeline state. Must be called from event loop thread.
    PipelineState GetState() const {
        assert(loop_.IsInEventLoopThread());
        return state_;
    }

private:
    // Private helpers don't assert event loop thread - they're only called
    // from public/handler methods that already assert.

    // Check if pipeline is in a terminal state (Done or Error).
    bool IsTerminal() const {
        return state_ == PipelineState::Done || state_ == PipelineState::Error;
    }

    void BuildPipeline() {
        // Pipeline is single-use - assert Connect() called only once
        assert(!connected_ && "Pipeline::Connect() can only be called once");
        connected_ = true;

        // Create sink (bridges to callbacks)
        sink_ = std::make_shared<Sink<Record>>(loop_, this);

        // Build protocol-specific chain (includes TcpSocket as head)
        chain_ = P::BuildChain(loop_, *sink_, api_key_);

        // Set up ready callback for when protocol is ready to send request
        // (Live: on connect, Historical: after TLS handshake)
        chain_->SetReadyCallback([this] { HandleReady(); });

        // Respect pre-connect Suspend() calls by propagating through chain
        if (suspend_count_.load(std::memory_order_acquire) > 0) {
            chain_->Suspend();
        }
    }

    void TeardownPipeline() {
        // Invalidate sink immediately to stop callbacks
        if (sink_) sink_->Invalidate();

        // If already tearing down, skip
        if (teardown_pending_) return;
        teardown_pending_ = true;

        // Defer actual teardown to avoid reentrancy (may be called from callbacks)
        // Use weak_ptr to safely handle destruction before deferred call runs
        auto weak_self = this->weak_from_this();
        loop_.Defer([weak_self] {
            if (auto self = weak_self.lock()) {
                self->DoTeardown();
            }
        });
    }

    void DoTeardown() {
        // Teardown chain (includes TcpSocket)
        P::Teardown(chain_);
        chain_.reset();
        sink_.reset();
    }

    void HandleReady() {
        // Terminal state guard
        if (teardown_pending_) return;

        state_ = PipelineState::Connected;
        ready_to_send_ = true;

        // If Start() was already called, send request now
        if (start_requested_ && !request_sent_) {
            P::SendRequest(chain_, request_);
            request_sent_ = true;
            state_ = PipelineState::Streaming;
        }
    }

    // =========================================================================
    // PipelineBase<Record> interface - called by Sink
    // =========================================================================

    void HandleRecord(const Record& rec) override {
        if (record_handler_) record_handler_(rec);
    }

    void HandleRecordBatch(RecordBatch&& batch) override {
        if (batch_handler_) {
            batch_handler_(std::move(batch));
        } else if (record_handler_) {
            // Fallback: iterate and call per-record handler
            // Only enabled when Record is constructible from RecordHeader*
            if constexpr (std::is_constructible_v<Record, const databento::RecordHeader*>) {
                for (const auto& ref : batch) {
                    Record rec{reinterpret_cast<const databento::RecordHeader*>(ref.data)};
                    record_handler_(rec);
                }
            }
            // If Record is not constructible, silently drop batch
            // (user should use batch handler for custom Record types)
        }
    }

    void HandlePipelineError(const Error& e) override {
        state_ = PipelineState::Error;
        if (error_handler_) error_handler_(e);
        // Protocol errors are also terminal
        TeardownPipeline();
    }

    void HandlePipelineComplete() override {
        state_ = PipelineState::Done;
        if (complete_handler_) complete_handler_();
        // Completion is terminal
        TeardownPipeline();
    }

    // =========================================================================
    // Member variables
    // =========================================================================

    IEventLoop& loop_;
    std::string api_key_;
    Request request_;
    PipelineState state_ = PipelineState::Disconnected;

    bool connected_ = false;        // Single-use guard
    bool request_set_ = false;      // SetRequest() called
    bool start_requested_ = false;
    bool request_sent_ = false;
    bool ready_to_send_ = false;
    bool teardown_pending_ = false; // Deferred teardown scheduled (weak_ptr guards lifetime)

    std::atomic<int> suspend_count_{0};

    std::shared_ptr<Sink<Record>> sink_;
    std::shared_ptr<typename P::ChainType> chain_;

    std::function<void(const Record&)> record_handler_;
    std::function<void(RecordBatch&&)> batch_handler_;
    std::function<void(const Error&)> error_handler_;
    std::function<void()> complete_handler_;
};

}  // namespace databento_async
