// SPDX-License-Identifier: MIT

// src/streaming_client.hpp
#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <string>

#include "dns_resolver.hpp"
#include "lib/stream/error.hpp"
#include "lib/stream/event_loop.hpp"
#include "lib/stream/pipeline.hpp"
#include "lib/stream/protocol.hpp"
#include "lib/stream/sink.hpp"
#include "lib/stream/suspendable.hpp"
#include "record_batch.hpp"

namespace dbn_pipe {

// Client states (user-visible) - shared across all StreamingClient instantiations
enum class ClientState {
    Disconnected,  // Initial state, or after Stop()
    Connecting,    // Connect() called
    Connected,     // Ready for Start()
    Streaming,     // Actively streaming
    Stopping,      // Stop() called, tearing down
    Done,          // Completed normally
    Error          // Terminal error
};

// Concept: Request type has a dataset member
template <typename R>
concept HasDataset = requires(const R& r) {
    { r.dataset } -> std::convertible_to<std::string>;
};

// StreamingClient<P> - User-facing client for streaming protocols
//
// Wraps Pipeline<Protocol> and provides:
// - Callback registration (OnRecord, OnError, OnComplete)
// - Request configuration (SetRequest)
// - State machine for user visibility
// - Backpressure (Suspend/Resume)
//
// Template parameter P must satisfy Protocol concept, use StreamRecordSink,
// and have a Request type with a dataset member.
//
// Lifecycle: Single-use. For reconnect, create a new client instance.
//
// Thread safety: All public methods must be called from event loop thread.
// IsSuspended() is the only exception - it's thread-safe.
template <typename P>
    requires Protocol<P> && std::same_as<typename P::SinkType, StreamRecordSink> &&
             HasDataset<typename P::Request>
class StreamingClient : public Suspendable,
                        public std::enable_shared_from_this<StreamingClient<P>> {
    struct PrivateTag {};

public:
    using Request = typename P::Request;
    using PipelineType = Pipeline<P>;
    using State = ClientState;  // Use shared enum

    // Factory method required for shared_from_this safety.
    static std::shared_ptr<StreamingClient> Create(IEventLoop& loop, std::string api_key) {
        return std::make_shared<StreamingClient>(PrivateTag{}, loop, std::move(api_key));
    }

    StreamingClient(PrivateTag, IEventLoop& loop, std::string api_key)
        : loop_(loop), api_key_(std::move(api_key)) {}

    // Destructor: Only invalidate sink to prevent callbacks during teardown.
    // Pipeline's destructor handles actual cleanup. We avoid calling Stop()
    // here because the destructor may run from any thread (when the last
    // shared_ptr ref is released), and Stop() requires the event loop thread.
    ~StreamingClient() {
        if (sink_) sink_->Invalidate();
    }

    // =========================================================================
    // Setup methods - call before Connect()
    // =========================================================================

    void SetRequest(Request params) {
        assert(loop_.IsInEventLoopThread());
        request_ = std::move(params);
        request_set_ = true;
    }

    // Set callback for record batches (efficient bulk delivery)
    template <typename H>
        requires std::invocable<H, RecordBatch&&>
    void OnRecord(H&& h) {
        assert(loop_.IsInEventLoopThread());
        batch_handler_ = std::forward<H>(h);
    }

    // Set callback for individual records
    template <typename H>
        requires std::invocable<H, const RecordRef&>
    void OnRecord(H&& h) {
        assert(loop_.IsInEventLoopThread());
        record_handler_ = std::forward<H>(h);
    }

    template <typename H>
    void OnError(H&& h) {
        assert(loop_.IsInEventLoopThread());
        error_handler_ = std::forward<H>(h);
    }

    template <typename H>
    void OnComplete(H&& h) {
        assert(loop_.IsInEventLoopThread());
        complete_handler_ = std::forward<H>(h);
    }

    // =========================================================================
    // Control methods
    // =========================================================================

    void Connect() {
        assert(loop_.IsInEventLoopThread());
        if (IsTerminal()) return;

        if (!request_set_) {
            HandleError(Error{ErrorCode::InvalidState,
                              "SetRequest() must be called before Connect()"});
            return;
        }

        std::string hostname = P::GetHostname(request_);
        uint16_t port = P::GetPort(request_);

        auto addr = ResolveHostname(hostname, port);
        if (!addr) {
            HandleError(Error{ErrorCode::DnsResolutionFailed,
                              "Failed to resolve hostname: " + hostname});
            return;
        }

        Connect(*addr);
    }

    void Connect(const sockaddr_storage& addr) {
        assert(loop_.IsInEventLoopThread());
        if (IsTerminal() || connected_) return;

        if (!request_set_) {
            HandleError(Error{ErrorCode::InvalidState,
                              "SetRequest() must be called before Connect()"});
            return;
        }

        BuildPipeline();
        state_ = State::Connecting;
        pipeline_->Connect(addr);
    }

    void Start() {
        assert(loop_.IsInEventLoopThread());
        if (IsTerminal()) return;

        if (!connected_) {
            HandleError(Error{ErrorCode::InvalidState,
                              "Connect() must be called before Start()"});
            return;
        }

        start_requested_ = true;
        if (ready_ && !started_) {
            pipeline_->Start();
            started_ = true;
            state_ = State::Streaming;
        }
    }

    void Stop() {
        assert(loop_.IsInEventLoopThread());
        if (IsTerminal()) return;
        state_ = State::Stopping;
        if (sink_) sink_->Invalidate();
        if (pipeline_) pipeline_->Stop();
    }

    // =========================================================================
    // Suspendable interface
    // =========================================================================

    void Suspend() override {
        assert(loop_.IsInEventLoopThread());
        if (++suspend_count_ == 1 && pipeline_) {
            pipeline_->Suspend();
        }
    }

    void Resume() override {
        assert(loop_.IsInEventLoopThread());
        assert(suspend_count_ > 0);
        if (--suspend_count_ == 0 && pipeline_) {
            pipeline_->Resume();
        }
    }

    void Close() override {
        Stop();
    }

    bool IsSuspended() const override {
        return suspend_count_.load(std::memory_order_acquire) > 0;
    }

    // =========================================================================
    // State accessors
    // =========================================================================

    State GetState() const {
        assert(loop_.IsInEventLoopThread());
        return state_;
    }

private:
    // Terminal states: Done, Error, or Stopping (Stop() was called)
    // Once in a terminal state, no further state transitions should occur.
    bool IsTerminal() const {
        return state_ == State::Done || state_ == State::Error ||
               state_ == State::Stopping;
    }

    void BuildPipeline() {
        assert(!connected_);
        connected_ = true;

        // Create sink with callbacks using weak_ptr to avoid ref cycle.
        // StreamingClient owns sink_/chain_, so capturing shared_ptr would
        // create a cycle that prevents destruction.
        std::weak_ptr<StreamingClient> weak_self = this->shared_from_this();
        sink_ = std::make_shared<StreamRecordSink>(
            // OnData
            [weak_self](RecordBatch&& batch) {
                if (auto self = weak_self.lock()) {
                    self->HandleBatch(std::move(batch));
                }
            },
            // OnError
            [weak_self](const Error& e) {
                if (auto self = weak_self.lock()) {
                    self->HandleError(e);
                }
            },
            // OnComplete
            [weak_self]() {
                if (auto self = weak_self.lock()) {
                    self->HandleComplete();
                }
            }
        );

        // Build chain
        chain_ = P::BuildChain(loop_, *sink_, api_key_);

        // Set dataset for protocols that need it (e.g., LiveProtocol CRAM auth)
        chain_->SetDataset(request_.dataset);

        // Set ready callback (also uses weak_ptr to avoid cycle)
        chain_->SetReadyCallback([weak_self]() {
            if (auto self = weak_self.lock()) {
                self->HandleReady();
            }
        });

        // Create pipeline
        pipeline_ = std::make_shared<PipelineType>(
            typename PipelineType::PrivateTag{},
            loop_, chain_, sink_, request_);

        // Apply pre-connect suspend state
        if (suspend_count_.load(std::memory_order_acquire) > 0) {
            pipeline_->Suspend();
        }
    }

    void HandleReady() {
        if (IsTerminal()) return;
        ready_ = true;
        state_ = State::Connected;
        pipeline_->MarkReady();

        if (start_requested_ && !started_) {
            pipeline_->Start();
            started_ = true;
            state_ = State::Streaming;
        }
    }

    void HandleBatch(RecordBatch&& batch) {
        if (batch_handler_) {
            batch_handler_(std::move(batch));
        } else if (record_handler_) {
            for (const auto& ref : batch) {
                record_handler_(ref);
            }
        }
    }

    void HandleError(const Error& e) {
        if (IsTerminal()) return;  // Already handled
        state_ = State::Error;
        // Teardown pipeline to release sockets and protocol components
        if (sink_) sink_->Invalidate();
        if (pipeline_) pipeline_->Stop();
        // CRITICAL: Defer user callback to ensure pipeline's callback stack has unwound.
        // Without this, if the user's callback spawns a coroutine that destroys
        // this client, the destruction happens while still on the OnError stack,
        // causing use-after-free.
        if (error_handler_) {
            Error error_copy = e;  // Copy since e may be invalidated
            std::weak_ptr<StreamingClient> weak_self = this->shared_from_this();
            loop_.Defer([weak_self, error_copy = std::move(error_copy)]() {
                if (auto self = weak_self.lock()) {
                    if (self->error_handler_) self->error_handler_(error_copy);
                }
            });
        }
    }

    void HandleComplete() {
        if (IsTerminal()) return;  // Already handled
        state_ = State::Done;
        // Teardown pipeline to release sockets and protocol components
        if (sink_) sink_->Invalidate();
        if (pipeline_) pipeline_->Stop();
        // CRITICAL: Defer user callback to ensure pipeline's callback stack has unwound.
        // Without this, if the user's callback spawns a coroutine that destroys
        // this client (e.g., calling client_.reset()), the destruction happens
        // while still on the OnDone stack, causing use-after-free.
        if (complete_handler_) {
            std::weak_ptr<StreamingClient> weak_self = this->shared_from_this();
            loop_.Defer([weak_self]() {
                if (auto self = weak_self.lock()) {
                    if (self->complete_handler_) self->complete_handler_();
                }
            });
        }
    }

    IEventLoop& loop_;
    std::string api_key_;
    Request request_;
    State state_ = State::Disconnected;

    bool request_set_ = false;
    bool connected_ = false;
    bool ready_ = false;
    bool start_requested_ = false;
    bool started_ = false;

    std::atomic<int> suspend_count_{0};

    std::shared_ptr<StreamRecordSink> sink_;
    std::shared_ptr<typename P::ChainType> chain_;
    std::shared_ptr<PipelineType> pipeline_;

    std::function<void(RecordBatch&&)> batch_handler_;
    std::function<void(const RecordRef&)> record_handler_;
    std::function<void(const Error&)> error_handler_;
    std::function<void()> complete_handler_;
};

}  // namespace dbn_pipe
