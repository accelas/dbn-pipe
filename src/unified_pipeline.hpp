// src/unified_pipeline.hpp
#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>

#include "error.hpp"
#include "pipeline.hpp"
#include "pipeline_base.hpp"
#include "protocol_driver.hpp"
#include "reactor.hpp"
#include "tcp_socket.hpp"

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
    static std::shared_ptr<Pipeline> Create(Reactor& reactor, std::string api_key) {
        return std::make_shared<Pipeline>(PrivateTag{}, reactor, std::move(api_key));
    }

    // Constructor is "public" for make_shared but requires PrivateTag.
    // This prevents direct construction while allowing make_shared to work.
    Pipeline(PrivateTag, Reactor& reactor, std::string api_key)
        : reactor_(reactor), api_key_(std::move(api_key)) {}

    // Destructor must run on reactor thread because DoTeardown touches
    // TcpSocket and chain components that have reactor-thread affinity.
    // Ensure the last shared_ptr release happens on reactor thread.
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
        assert(reactor_.IsInReactorThread());
        request_ = std::move(params);
        request_set_ = true;
    }

    // Set callback for received records (one at a time).
    template <typename H>
        requires std::invocable<H, const Record&>
    void OnRecord(H&& h) {
        assert(reactor_.IsInReactorThread());
        record_handler_ = std::forward<H>(h);
    }

    // Set callback for record batches (efficient bulk delivery).
    // If set, per-record callback is ignored - batches go directly to this handler.
    template <typename H>
        requires std::invocable<H, RecordBatch&&>
    void OnRecord(H&& h) {
        assert(reactor_.IsInReactorThread());
        batch_handler_ = std::forward<H>(h);
    }

    // Set callback for errors.
    template <typename H>
    void OnError(H&& h) {
        assert(reactor_.IsInReactorThread());
        error_handler_ = std::forward<H>(h);
    }

    // Set callback for stream completion.
    template <typename H>
    void OnComplete(H&& h) {
        assert(reactor_.IsInReactorThread());
        complete_handler_ = std::forward<H>(h);
    }

    // =========================================================================
    // Control methods
    // =========================================================================

    // Connect to the given address. Must be called once before Start().
    // Terminal state guard - silently ignores if pipeline is done.
    void Connect(const sockaddr_storage& addr) {
        assert(reactor_.IsInReactorThread());

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
        tcp_->Connect(addr);
    }

    // Start streaming. Must be called after Connect() and SetRequest().
    // Terminal state guard - silently ignores if pipeline is done.
    void Start() {
        assert(reactor_.IsInReactorThread());

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
        assert(reactor_.IsInReactorThread());
        if (IsTerminal()) return;  // Already done or errored
        state_ = PipelineState::Stopping;  // Transition to Stopping
        TeardownPipeline();
    }

    // =========================================================================
    // Suspendable interface - uses count-based semantics
    // =========================================================================

    // Increment suspend count. When count goes 0->1, pause reading.
    void Suspend() override {
        assert(reactor_.IsInReactorThread());
        if (++suspend_count_ == 1 && tcp_) {
            tcp_->PauseRead();
        }
    }

    // Decrement suspend count. When count goes 1->0, resume reading.
    void Resume() override {
        assert(reactor_.IsInReactorThread());
        assert(suspend_count_ > 0);
        if (--suspend_count_ == 0 && tcp_) {
            tcp_->ResumeRead();
        }
    }

    // Terminate the connection via TeardownPipeline.
    void Close() override {
        assert(reactor_.IsInReactorThread());
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

    // Get current pipeline state. Must be called from reactor thread.
    PipelineState GetState() const {
        assert(reactor_.IsInReactorThread());
        return state_;
    }

private:
    // Private helpers don't assert reactor thread - they're only called
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
        sink_ = std::make_shared<Sink<Record>>(reactor_, this);

        // Build protocol-specific chain
        chain_ = P::BuildChain(reactor_, *sink_, api_key_);

        // Create TCP socket
        tcp_ = std::make_unique<TcpSocket>(reactor_);

        // Wire TCP callbacks
        tcp_->OnConnect([this] { HandleConnect(); });
        tcp_->OnRead([this](auto data) { HandleRead(data); });
        tcp_->OnError([this](auto ec) { HandleTcpError(ec); });

        // Respect pre-connect Suspend() calls
        if (suspend_count_.load(std::memory_order_acquire) > 0) {
            tcp_->PauseRead();
        }

        // Wire chain write callback to TCP
        P::WireTcp(*tcp_, chain_);
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
        reactor_.Defer([weak_self] {
            if (auto self = weak_self.lock()) {
                self->DoTeardown();
            }
        });
    }

    void DoTeardown() {
        // Teardown chain before TCP to ensure no pending writes
        P::Teardown(chain_);
        chain_.reset();

        // Close TCP socket
        if (tcp_) {
            tcp_->Close();
        }
        tcp_.reset();
        sink_.reset();
    }

    void HandleConnect() {
        // Terminal state guard
        if (teardown_pending_) return;

        state_ = PipelineState::Connected;
        ready_to_send_ = P::OnConnect(chain_);

        // If ready immediately (Live) and Start() was called, send request now
        if (ready_to_send_ && start_requested_ && !request_sent_) {
            P::SendRequest(chain_, request_);
            request_sent_ = true;
            state_ = PipelineState::Streaming;
        }
    }

    void HandleRead(std::span<const std::byte> data) {
        // Terminal state guard
        if (teardown_pending_) return;

        std::pmr::vector<std::byte> buffer(&pool_);
        buffer.assign(data.begin(), data.end());

        bool now_ready = P::OnRead(chain_, std::move(buffer));

        // If just became ready and Start() was already called, send request
        if (now_ready && !ready_to_send_) {
            ready_to_send_ = true;
            if (start_requested_ && !request_sent_) {
                P::SendRequest(chain_, request_);
                request_sent_ = true;
                state_ = PipelineState::Streaming;
            }
        }
    }

    void HandleTcpError(std::error_code ec) {
        // Terminal guard - suppress late errors during teardown
        if (teardown_pending_) return;

        state_ = PipelineState::Error;
        if (error_handler_) {
            error_handler_(Error{ErrorCode::ConnectionFailed, ec.message(), ec.value()});
        }
        // Error is terminal for single-use pipeline
        TeardownPipeline();
    }

    // =========================================================================
    // PipelineBase<Record> interface - called by Sink
    // =========================================================================

    void HandleRecord(const Record& rec) override {
        if (record_handler_) record_handler_(rec);
    }

    void HandleRecordBatch(RecordBatch&& batch) override {
        // batch_handler_ must be set - no fallback to per-record iteration
        if (batch_handler_) {
            batch_handler_(std::move(batch));
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

    Reactor& reactor_;
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

    std::unique_ptr<TcpSocket> tcp_;
    std::shared_ptr<Sink<Record>> sink_;
    std::shared_ptr<typename P::ChainType> chain_;

    std::pmr::unsynchronized_pool_resource pool_;

    std::function<void(const Record&)> record_handler_;
    std::function<void(RecordBatch&&)> batch_handler_;
    std::function<void(const Error&)> error_handler_;
    std::function<void()> complete_handler_;
};

}  // namespace databento_async
