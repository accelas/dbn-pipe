// SPDX-License-Identifier: MIT

// src/streaming_client.hpp
#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>

#include "dbn_pipe/dns_resolver.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/pipeline.hpp"
#include "dbn_pipe/stream/protocol.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"
#include "dbn_pipe/stream/sink.hpp"
#include "dbn_pipe/stream/suspendable.hpp"
#include "dbn_pipe/record_batch.hpp"

namespace dbn_pipe {

/// Client lifecycle states, shared across all StreamingClient instantiations.
enum class ClientState {
    Disconnected,  ///< Initial state, or after Stop()
    Connecting,    ///< Connect() called, TCP/TLS handshake in progress
    Connected,     ///< Protocol handshake complete, ready for Start()
    Streaming,     ///< Actively receiving records
    Stopping,      ///< Stop() called, tearing down
    Done,          ///< Stream completed normally
    Error          ///< Terminal error (see OnError callback)
};

/// @internal Concept: Request type has a dataset member.
template <typename R>
concept HasDataset = requires(const R& r) {
    { r.dataset } -> std::convertible_to<std::string>;
};

/// User-facing client for streaming protocols (Live and Historical).
///
/// Wraps Pipeline<Protocol> and provides callback registration, request
/// configuration, a state machine, and backpressure propagation.
///
/// Lifecycle: single-use. For reconnect, create a new instance.
///
/// Thread safety: all public methods must be called from the event loop
/// thread. IsSuspended() is the only exception (thread-safe).
///
/// @code
/// auto client = LiveClient::Create(loop, "api-key");
/// client->SetRequest({.dataset="GLBX.MDP3", .symbols="ESZ4", .schema="mbp-1"});
/// client->OnRecord([](const RecordRef& r) { /* ... */ });
/// client->Connect();
/// client->Start();
/// @endcode
template <typename P>
    requires Protocol<P> && std::same_as<typename P::SinkType, StreamRecordSink> &&
             HasDataset<typename P::Request>
class StreamingClient : public Suspendable,
                        public std::enable_shared_from_this<StreamingClient<P>> {
    struct PrivateTag {};

public:
    using Request = typename P::Request;
    using PipelineType = Pipeline<P>;
    using State = ClientState;

    /// Create a new client. Factory method required for shared_from_this safety.
    /// @param loop     Event loop that drives this client
    /// @param api_key  Databento API key
    static std::shared_ptr<StreamingClient> Create(IEventLoop& loop, std::string api_key) {
        return std::make_shared<StreamingClient>(PrivateTag{}, loop, std::move(api_key));
    }

    /// Create a new client with a caller-provided PMR memory resource.
    /// All segment allocations in the pipeline will use this resource.
    /// @param loop      Event loop that drives this client
    /// @param api_key   Databento API key
    /// @param resource  PMR memory resource for segment allocation
    static std::shared_ptr<StreamingClient> Create(
        IEventLoop& loop, std::string api_key,
        std::shared_ptr<std::pmr::memory_resource> resource)
    {
        auto client = std::make_shared<StreamingClient>(
            PrivateTag{}, loop, std::move(api_key));
        client->allocator_ = SegmentAllocator(std::move(resource));
        return client;
    }

    /// @internal
    StreamingClient(PrivateTag, IEventLoop& loop, std::string api_key)
        : loop_(loop), api_key_(std::move(api_key)) {}

    ~StreamingClient() {
        if (sink_) sink_->Invalidate();
    }

    /// Set request parameters. Must be called before Connect().
    void SetRequest(Request params) {
        assert(loop_.IsInEventLoopThread());
        request_ = std::move(params);
        request_set_ = true;
    }

    /// Set callback for record batches (efficient bulk delivery).
    template <typename H>
        requires std::invocable<H, RecordBatch&&>
    void OnRecord(H&& h) {
        assert(loop_.IsInEventLoopThread());
        batch_handler_ = std::forward<H>(h);
    }

    /// Set callback for individual records.
    template <typename H>
        requires std::invocable<H, const RecordRef&>
    void OnRecord(H&& h) {
        assert(loop_.IsInEventLoopThread());
        record_handler_ = std::forward<H>(h);
    }

    /// Set callback for errors. The Error struct contains code, message, and optional retry_after.
    template <typename H>
    void OnError(H&& h) {
        assert(loop_.IsInEventLoopThread());
        error_handler_ = std::forward<H>(h);
    }

    /// Set callback for stream completion (historical downloads only).
    template <typename H>
    void OnComplete(H&& h) {
        assert(loop_.IsInEventLoopThread());
        complete_handler_ = std::forward<H>(h);
    }

    /// Resolve hostname and connect. Requires SetRequest() first.
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

    /// Connect to a pre-resolved address. Requires SetRequest() first.
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

    /// Begin streaming. Sends the protocol request after connection is ready.
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

    /// Tear down the connection and stop receiving records.
    void Stop() {
        assert(loop_.IsInEventLoopThread());
        if (IsTerminal()) return;
        state_ = State::Stopping;
        if (sink_) sink_->Invalidate();
        if (pipeline_) pipeline_->Stop();
    }

    /// Pause reading from the network (backpressure). Nestable.
    void Suspend() override {
        assert(loop_.IsInEventLoopThread());
        if (++suspend_count_ == 1 && pipeline_) {
            pipeline_->Suspend();
        }
    }

    /// Resume reading after Suspend(). Must balance each Suspend() call.
    void Resume() override {
        assert(loop_.IsInEventLoopThread());
        assert(suspend_count_ > 0);
        if (--suspend_count_ == 0 && pipeline_) {
            pipeline_->Resume();
        }
    }

    /// Alias for Stop(). Satisfies Suspendable interface.
    void Close() override {
        Stop();
    }

    /// Return true if currently suspended. Thread-safe.
    bool IsSuspended() const override {
        return suspend_count_.load(std::memory_order_acquire) > 0;
    }

    /// Return the current client state.
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

        // Build chain (pass allocator so all components share the same PMR)
        chain_ = P::BuildChain(loop_, *sink_, api_key_, &allocator_);

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
    SegmentAllocator allocator_;
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
