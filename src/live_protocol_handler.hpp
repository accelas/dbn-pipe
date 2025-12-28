// src/live_protocol_handler.hpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cram_auth.hpp"
#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"
#include "tls_socket.hpp"  // For Suspendable

namespace databento_async {

// State machine for live protocol authentication and streaming
enum class LiveProtocolState {
    WaitingGreeting,
    WaitingChallenge,
    Authenticating,
    Ready,
    Streaming
};

// LiveProtocolHandler - Pipeline component for Databento live protocol.
//
// Handles the text-based authentication protocol:
//   1. Receive greeting (session_id|version)
//   2. Receive challenge (cram=...)
//   3. Send auth response (auth=...|bucket|...)
//   4. Wait for auth confirmation
//   5. Subscribe and start streaming
//
// LIMITATION: After entering Streaming state, all data is treated as binary.
// If the server sends text error messages after start_session, they will be
// passed to downstream as binary data.
//
// Uses CRTP with PipelineComponent base for reentrancy-safe lifecycle.
// Implements Suspendable for backpressure control.
//
// Template parameter D must satisfy Downstream concept (bytes out, not records).
template <Downstream D>
class LiveProtocolHandler
    : public PipelineComponent<LiveProtocolHandler<D>>,
      public Suspendable,
      public std::enable_shared_from_this<LiveProtocolHandler<D>> {

    using Base = PipelineComponent<LiveProtocolHandler<D>>;
    friend Base;

    // Enable shared_from_this in constructor via MakeSharedEnabler pattern
    struct MakeSharedEnabler;

public:
    using WriteCallback = std::function<void(std::pmr::vector<std::byte>)>;

    // Factory method for shared_from_this safety
    static std::shared_ptr<LiveProtocolHandler> Create(
        Reactor& reactor,
        std::shared_ptr<D> downstream,
        std::string api_key
    ) {
        return std::make_shared<MakeSharedEnabler>(reactor, std::move(downstream),
                                                    std::move(api_key));
    }

    ~LiveProtocolHandler() = default;

    // Downstream interface (bytes in from upstream)
    void Read(std::pmr::vector<std::byte> data);
    void OnError(const Error& e);
    void OnDone();

    // Suspendable interface
    void Suspend() override;
    void Resume() override;
    void Close() { this->RequestClose(); }

    // Set upstream for backpressure propagation
    void SetUpstream(Suspendable* up) { upstream_ = up; }

    // Set callback for sending data back through the socket
    void SetWriteCallback(WriteCallback cb) { write_callback_ = std::move(cb); }

    // Subscribe to dataset/symbols/schema
    // If already Ready, sends immediately; otherwise queues for later
    void Subscribe(std::string dataset, std::string symbols, std::string schema);

    // Start streaming after subscription
    // Sends "start_session\n" and transitions to Streaming state
    void StartStreaming();

    // State accessors
    LiveProtocolState GetState() const { return state_; }
    const Greeting& GetGreeting() const { return greeting_; }

    // PipelineComponent requirements
    void DisableWatchers() {}
    void DoClose();

    // Buffer limits
    static constexpr std::size_t kMaxPendingData = 16 * 1024 * 1024;  // 16MB
    static constexpr std::size_t kMaxLineLength = 8 * 1024;           // 8KB

private:
    LiveProtocolHandler(Reactor& reactor, std::shared_ptr<D> downstream,
                        std::string api_key);

    // Process accumulated line buffer for text mode
    void ProcessLineBuffer();

    // Process a complete line (without trailing \r\n)
    void ProcessLine(std::string_view line);

    // State-specific handlers
    void HandleGreeting(std::string_view line);
    void HandleChallenge(std::string_view line);
    void HandleAuthResponse(std::string_view line);

    // Send data through write callback
    void SendLine(std::string_view line);

    // Send pending subscription if in Ready state
    void SendPendingSubscription();

    std::shared_ptr<D> downstream_;
    Suspendable* upstream_ = nullptr;
    WriteCallback write_callback_ = [](std::pmr::vector<std::byte>) {};

    std::string api_key_;
    LiveProtocolState state_ = LiveProtocolState::WaitingGreeting;
    Greeting greeting_;

    // Text mode line buffer (used before Streaming state)
    std::vector<std::byte> line_buffer_;

    // Binary mode pending buffer (used in Streaming state when suspended)
    std::vector<std::byte> pending_data_;

    // Subscription parameters (queued until Ready)
    std::optional<std::string> pending_dataset_;
    std::optional<std::string> pending_symbols_;
    std::optional<std::string> pending_schema_;
    bool subscription_sent_ = false;
    bool start_requested_ = false;

    // PMR pool for output buffers
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::polymorphic_allocator<std::byte> alloc_{&pool_};
};

// MakeSharedEnabler - allows make_shared with private constructor
template <Downstream D>
struct LiveProtocolHandler<D>::MakeSharedEnabler : public LiveProtocolHandler<D> {
    MakeSharedEnabler(Reactor& reactor, std::shared_ptr<D> downstream,
                      std::string api_key)
        : LiveProtocolHandler<D>(reactor, std::move(downstream),
                                  std::move(api_key)) {}
};

// Implementation

template <Downstream D>
LiveProtocolHandler<D>::LiveProtocolHandler(Reactor& reactor,
                                             std::shared_ptr<D> downstream,
                                             std::string api_key)
    : Base(reactor)
    , downstream_(std::move(downstream))
    , api_key_(std::move(api_key))
{}

template <Downstream D>
void LiveProtocolHandler<D>::DoClose() {
    downstream_.reset();
    line_buffer_.clear();
    pending_data_.clear();
}

template <Downstream D>
void LiveProtocolHandler<D>::Read(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    if (state_ == LiveProtocolState::Streaming) {
        // Binary mode: pass through or buffer when suspended
        if (this->suspended_) {
            // Check buffer overflow
            if (pending_data_.size() + data.size() > kMaxPendingData) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::BufferOverflow, "Binary buffer overflow"});
                this->RequestClose();
                return;
            }
            pending_data_.insert(pending_data_.end(), data.begin(), data.end());
        } else {
            downstream_->Read(std::move(data));
        }
    } else {
        // Text mode: accumulate in line buffer, process on '\n'
        // Check line buffer overflow
        if (line_buffer_.size() + data.size() > kMaxLineLength) {
            this->EmitError(*downstream_,
                Error{ErrorCode::BufferOverflow, "Line buffer overflow"});
            this->RequestClose();
            return;
        }
        line_buffer_.insert(line_buffer_.end(), data.begin(), data.end());
        ProcessLineBuffer();
    }
}

template <Downstream D>
void LiveProtocolHandler<D>::ProcessLineBuffer() {
    // Process complete lines from buffer
    while (true) {
        auto it = std::find(line_buffer_.begin(), line_buffer_.end(),
                            std::byte{'\n'});
        if (it == line_buffer_.end()) {
            break;  // No complete line yet
        }

        // Extract line (including potential \r before \n)
        std::size_t line_end = static_cast<std::size_t>(it - line_buffer_.begin());
        std::size_t content_end = line_end;

        // Trim \r if present
        if (content_end > 0 &&
            line_buffer_[content_end - 1] == std::byte{'\r'}) {
            --content_end;
        }

        // Convert to string_view
        std::string_view line(
            reinterpret_cast<const char*>(line_buffer_.data()),
            content_end
        );

        // Process the line
        ProcessLine(line);

        // Remove processed line from buffer (including \n)
        line_buffer_.erase(line_buffer_.begin(),
                           line_buffer_.begin() + static_cast<std::ptrdiff_t>(line_end + 1));

        // If we transitioned to Streaming, any remaining data is binary
        if (state_ == LiveProtocolState::Streaming && !line_buffer_.empty()) {
            std::pmr::vector<std::byte> remaining(alloc_);
            remaining.assign(line_buffer_.begin(), line_buffer_.end());
            line_buffer_.clear();
            downstream_->Read(std::move(remaining));
            break;
        }
    }
}

template <Downstream D>
void LiveProtocolHandler<D>::ProcessLine(std::string_view line) {
    switch (state_) {
        case LiveProtocolState::WaitingGreeting:
            HandleGreeting(line);
            break;
        case LiveProtocolState::WaitingChallenge:
            HandleChallenge(line);
            break;
        case LiveProtocolState::Authenticating:
            HandleAuthResponse(line);
            break;
        case LiveProtocolState::Ready:
        case LiveProtocolState::Streaming:
            // Unexpected text in Ready/Streaming states
            // This shouldn't happen, but we'll ignore it
            break;
    }
}

template <Downstream D>
void LiveProtocolHandler<D>::HandleGreeting(std::string_view line) {
    auto greeting = CramAuth::ParseGreeting(line);
    if (!greeting) {
        this->EmitError(*downstream_,
            Error{ErrorCode::InvalidGreeting,
                  std::string("Invalid greeting: ") + std::string(line)});
        this->RequestClose();
        return;
    }

    greeting_ = std::move(*greeting);
    state_ = LiveProtocolState::WaitingChallenge;
}

template <Downstream D>
void LiveProtocolHandler<D>::HandleChallenge(std::string_view line) {
    auto challenge = CramAuth::ParseChallenge(line);
    if (!challenge) {
        this->EmitError(*downstream_,
            Error{ErrorCode::InvalidChallenge,
                  std::string("Invalid challenge: ") + std::string(line)});
        this->RequestClose();
        return;
    }

    // Compute CRAM response
    std::string response = CramAuth::ComputeResponse(*challenge, api_key_);

    // Send auth: auth=response|bucket_id
    // Using empty bucket_id for now - can be extended later
    std::string auth_line = "auth=" + response + "|";
    SendLine(auth_line);

    state_ = LiveProtocolState::Authenticating;
}

template <Downstream D>
void LiveProtocolHandler<D>::HandleAuthResponse(std::string_view line) {
    // Check for auth success or failure
    // Server responds with various status messages

    // Check for error responses
    if (line.starts_with("err=")) {
        this->EmitError(*downstream_,
            Error{ErrorCode::AuthFailed,
                  std::string("Authentication failed: ") + std::string(line)});
        this->RequestClose();
        return;
    }

    // Any non-error response means auth succeeded
    state_ = LiveProtocolState::Ready;

    // Send any pending subscription
    SendPendingSubscription();
}

template <Downstream D>
void LiveProtocolHandler<D>::Subscribe(std::string dataset, std::string symbols,
                                        std::string schema) {
    pending_dataset_ = std::move(dataset);
    pending_symbols_ = std::move(symbols);
    pending_schema_ = std::move(schema);
    subscription_sent_ = false;

    if (state_ == LiveProtocolState::Ready) {
        SendPendingSubscription();
    }
}

template <Downstream D>
void LiveProtocolHandler<D>::SendPendingSubscription() {
    if (subscription_sent_ || !pending_dataset_ || !pending_symbols_ ||
        !pending_schema_) {
        return;
    }

    // Format: subscription=dataset|schema|stype_in|symbols
    // Using 'raw_symbol' as stype_in for now
    std::string sub_line = "subscription=" + *pending_dataset_ + "|" +
                           *pending_schema_ + "|raw_symbol|" + *pending_symbols_;
    SendLine(sub_line);
    subscription_sent_ = true;

    // If start was requested before subscription was sent, send it now
    if (start_requested_) {
        SendLine("start_session");
        state_ = LiveProtocolState::Streaming;
    }
}

template <Downstream D>
void LiveProtocolHandler<D>::StartStreaming() {
    start_requested_ = true;

    if (state_ == LiveProtocolState::Ready && subscription_sent_) {
        SendLine("start_session");
        state_ = LiveProtocolState::Streaming;
    }
    // If not ready yet, will be sent after subscription in SendPendingSubscription
}

template <Downstream D>
void LiveProtocolHandler<D>::SendLine(std::string_view line) {
    std::string with_newline(line);
    with_newline += '\n';

    std::pmr::vector<std::byte> data(alloc_);
    data.resize(with_newline.size());
    std::memcpy(data.data(), with_newline.data(), with_newline.size());

    write_callback_(std::move(data));
}

template <Downstream D>
void LiveProtocolHandler<D>::OnError(const Error& e) {
    auto guard = this->TryGuard();
    if (!guard) return;

    this->EmitError(*downstream_, e);
    this->RequestClose();
}

template <Downstream D>
void LiveProtocolHandler<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    // Check for incomplete protocol state
    if (state_ != LiveProtocolState::Streaming) {
        this->EmitError(*downstream_,
            Error{ErrorCode::ConnectionClosed,
                  "Connection closed during authentication"});
    } else if (!pending_data_.empty()) {
        // Flush any pending data before signaling done
        std::pmr::vector<std::byte> data(alloc_);
        data.assign(pending_data_.begin(), pending_data_.end());
        pending_data_.clear();
        downstream_->Read(std::move(data));
        this->EmitDone(*downstream_);
    } else {
        this->EmitDone(*downstream_);
    }
    this->RequestClose();
}

template <Downstream D>
void LiveProtocolHandler<D>::Suspend() {
    auto guard = this->TryGuard();
    if (!guard) return;

    this->suspended_ = true;
    if (upstream_) upstream_->Suspend();
}

template <Downstream D>
void LiveProtocolHandler<D>::Resume() {
    auto guard = this->TryGuard();
    if (!guard) return;

    this->suspended_ = false;

    // Flush any pending binary data
    if (!pending_data_.empty()) {
        std::pmr::vector<std::byte> data(alloc_);
        data.assign(pending_data_.begin(), pending_data_.end());
        pending_data_.clear();
        downstream_->Read(std::move(data));
    }

    // Propagate resume upstream if not re-suspended
    if (!this->suspended_ && upstream_) {
        upstream_->Resume();
    }
}

}  // namespace databento_async
