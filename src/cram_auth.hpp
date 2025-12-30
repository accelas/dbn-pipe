// src/cram_auth.hpp
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

#include "buffer_chain.hpp"
#include "cram_auth_utils.hpp"
#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"
#include "tls_socket.hpp"  // For Suspendable

namespace databento_async {

// State machine for CRAM authentication and streaming
enum class CramAuthState {
    WaitingGreeting,
    WaitingChallenge,
    Authenticating,
    Ready,
    Streaming
};

// CramAuth - Pipeline component for Databento CRAM authentication protocol.
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
// PipelineComponent provides Suspendable interface with suspend count semantics.
//
// Template parameter D must satisfy Downstream concept (receives BufferChain).
template <Downstream D>
class CramAuth
    : public PipelineComponent<CramAuth<D>>,
      public std::enable_shared_from_this<CramAuth<D>> {

    using Base = PipelineComponent<CramAuth<D>>;
    friend Base;

    // Enable shared_from_this in constructor via MakeSharedEnabler pattern
    struct MakeSharedEnabler;

public:
    using WriteCallback = std::function<void(BufferChain)>;

    // Factory method for shared_from_this safety
    static std::shared_ptr<CramAuth> Create(
        Reactor& reactor,
        std::shared_ptr<D> downstream,
        std::string api_key
    ) {
        return std::make_shared<MakeSharedEnabler>(reactor, std::move(downstream),
                                                    std::move(api_key));
    }

    ~CramAuth() = default;

    // Downstream interface (bytes in from upstream)
    void OnData(BufferChain& data);
    void OnError(const Error& e);
    void OnDone();

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
    CramAuthState GetState() const { return state_; }
    const Greeting& GetGreeting() const { return greeting_; }

    // PipelineComponent requirements
    void DisableWatchers() {}
    void DoClose();

    // Suspendable hooks (called by PipelineComponent base)
    void OnSuspend() {
        if (upstream_) upstream_->Suspend();
    }

    void OnResume() {
        auto guard = this->TryGuard();
        if (!guard) return;
        ProcessPending();
        if (this->IsSuspended()) return;
        if (upstream_) upstream_->Resume();
    }

    void ProcessPending() {
        // Process pending binary data through streaming chain
        if (!pending_chain_.Empty()) {
            // Check overflow before splicing (use subtraction pattern)
            if (pending_chain_.Size() > kMaxPendingData - streaming_chain_.Size()) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::BufferOverflow, "Binary buffer overflow"});
                this->RequestClose();
                return;
            }
            streaming_chain_.Splice(std::move(pending_chain_));
        }
        this->ForwardData(*downstream_, streaming_chain_);
    }

    void FlushAndComplete() {
        // Process any pending data
        if (!pending_chain_.Empty()) {
            // Check overflow before splicing (use subtraction pattern)
            if (pending_chain_.Size() > kMaxPendingData - streaming_chain_.Size()) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::BufferOverflow, "Binary buffer overflow"});
                this->RequestClose();
                return;
            }
            streaming_chain_.Splice(std::move(pending_chain_));
        }
        // Flush and complete - only close if completion happened
        if (this->CompleteWithFlush(*downstream_, streaming_chain_)) {
            this->RequestClose();
        }
    }

    // Buffer limits
    static constexpr std::size_t kMaxPendingData = 16 * 1024 * 1024;  // 16MB
    static constexpr std::size_t kMaxLineLength = 8 * 1024;           // 8KB

private:
    CramAuth(Reactor& reactor, std::shared_ptr<D> downstream,
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
    WriteCallback write_callback_ = [](BufferChain) {};

    std::string api_key_;
    CramAuthState state_ = CramAuthState::WaitingGreeting;
    Greeting greeting_;

    // Text mode line buffer (used before Streaming state)
    std::vector<std::byte> line_buffer_;

    // Binary mode streaming chain (persistent - retains incomplete records)
    BufferChain streaming_chain_;

    // Binary mode pending chain (used in Streaming state when suspended)
    BufferChain pending_chain_;

    // Subscription parameters (queued until Ready)
    std::optional<std::string> pending_dataset_;
    std::optional<std::string> pending_symbols_;
    std::optional<std::string> pending_schema_;
    bool subscription_sent_ = false;
    bool start_requested_ = false;

    // PMR pool for output buffers (write path only)
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::polymorphic_allocator<std::byte> alloc_{&pool_};
};

// MakeSharedEnabler - allows make_shared with private constructor
template <Downstream D>
struct CramAuth<D>::MakeSharedEnabler : public CramAuth<D> {
    MakeSharedEnabler(Reactor& reactor, std::shared_ptr<D> downstream,
                      std::string api_key)
        : CramAuth<D>(reactor, std::move(downstream),
                      std::move(api_key)) {}
};

// Implementation

template <Downstream D>
CramAuth<D>::CramAuth(Reactor& reactor,
                      std::shared_ptr<D> downstream,
                      std::string api_key)
    : Base(reactor)
    , downstream_(std::move(downstream))
    , api_key_(std::move(api_key))
{}

template <Downstream D>
void CramAuth<D>::DoClose() {
    downstream_.reset();
    line_buffer_.clear();
    streaming_chain_.Clear();
    pending_chain_.Clear();
}

template <Downstream D>
void CramAuth<D>::OnData(BufferChain& data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    if (state_ == CramAuthState::Streaming) {
        // Binary mode: use persistent chain to retain incomplete records
        if (this->IsSuspended()) {
            // Check buffer overflow
            if (data.Size() > kMaxPendingData - pending_chain_.Size()) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::BufferOverflow, "Binary buffer overflow"});
                this->RequestClose();
                return;
            }
            // Compact if upstream passed a partially consumed chain
            if (data.IsPartiallyConsumed()) {
                data.Compact();
            }
            pending_chain_.Splice(std::move(data));
        } else {
            // Splice pending data first if any (with overflow check)
            if (!pending_chain_.Empty()) {
                if (pending_chain_.Size() > kMaxPendingData - streaming_chain_.Size()) {
                    this->EmitError(*downstream_,
                        Error{ErrorCode::BufferOverflow, "Binary buffer overflow"});
                    this->RequestClose();
                    return;
                }
                streaming_chain_.Splice(std::move(pending_chain_));
            }
            // Compact if upstream passed a partially consumed chain
            if (data.IsPartiallyConsumed()) {
                data.Compact();
            }
            // Check overflow before splicing (use subtraction pattern)
            if (data.Size() > kMaxPendingData - streaming_chain_.Size()) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::BufferOverflow, "Binary buffer overflow"});
                this->RequestClose();
                return;
            }
            // Splice new data into persistent chain
            streaming_chain_.Splice(std::move(data));
            // Pass persistent chain to downstream - it consumes what it can
            // and leaves incomplete records for next call
            downstream_->OnData(streaming_chain_);
        }
    } else {
        // Text mode: extract bytes to line buffer, process on '\n'
        // Extract bytes from chain to line buffer (auth handshake only - small data)
        while (!data.Empty()) {
            std::byte b;
            data.CopyTo(0, 1, &b);
            data.Consume(1);

            if (b == std::byte{'\n'}) {
                // Check line buffer overflow before adding newline
                if (line_buffer_.size() >= kMaxLineLength) {
                    this->EmitError(*downstream_,
                        Error{ErrorCode::BufferOverflow, "Line buffer overflow"});
                    this->RequestClose();
                    return;
                }
                // Complete line - push newline and process
                line_buffer_.push_back(b);
                ProcessLineBuffer();
                // Check if we're now in streaming mode
                if (state_ == CramAuthState::Streaming) {
                    // Pass remaining data as binary through streaming chain
                    if (!data.Empty()) {
                        // Compact if partially consumed from line extraction
                        if (data.IsPartiallyConsumed()) {
                            data.Compact();
                        }
                        // Buffer to pending if suspended, otherwise forward
                        if (this->IsSuspended()) {
                            // Check overflow before buffering
                            if (data.Size() > kMaxPendingData - pending_chain_.Size()) {
                                this->EmitError(*downstream_,
                                    Error{ErrorCode::BufferOverflow, "Binary buffer overflow"});
                                this->RequestClose();
                                return;
                            }
                            pending_chain_.Splice(std::move(data));
                        } else {
                            // Check overflow before splicing (use subtraction pattern)
                            if (data.Size() > kMaxPendingData - streaming_chain_.Size()) {
                                this->EmitError(*downstream_,
                                    Error{ErrorCode::BufferOverflow, "Binary buffer overflow"});
                                this->RequestClose();
                                return;
                            }
                            streaming_chain_.Splice(std::move(data));
                            // Pass through persistent chain - handles suspension
                            downstream_->OnData(streaming_chain_);
                        }
                    }
                    return;
                }
            } else {
                // Check line buffer overflow before adding byte
                if (line_buffer_.size() >= kMaxLineLength) {
                    this->EmitError(*downstream_,
                        Error{ErrorCode::BufferOverflow, "Line buffer overflow"});
                    this->RequestClose();
                    return;
                }
                line_buffer_.push_back(b);
            }
        }
    }
}

template <Downstream D>
void CramAuth<D>::ProcessLineBuffer() {
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
        if (state_ == CramAuthState::Streaming && !line_buffer_.empty()) {
            // Check overflow before appending (use subtraction pattern)
            if (line_buffer_.size() > kMaxPendingData - streaming_chain_.Size()) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::BufferOverflow, "Binary buffer overflow"});
                this->RequestClose();
                return;
            }
            // Create a chain with leftover bytes (one-time copy at auth completion)
            auto seg = std::make_shared<Segment>();
            std::memcpy(seg->data.data(), line_buffer_.data(), line_buffer_.size());
            seg->size = line_buffer_.size();
            streaming_chain_.Append(std::move(seg));
            line_buffer_.clear();

            // Respect IsSuspended() check for leftover bytes (backpressure)
            if (!this->IsSuspended()) {
                downstream_->OnData(streaming_chain_);
            }
            break;
        }
    }
}

template <Downstream D>
void CramAuth<D>::ProcessLine(std::string_view line) {
    switch (state_) {
        case CramAuthState::WaitingGreeting:
            HandleGreeting(line);
            break;
        case CramAuthState::WaitingChallenge:
            HandleChallenge(line);
            break;
        case CramAuthState::Authenticating:
            HandleAuthResponse(line);
            break;
        case CramAuthState::Ready:
        case CramAuthState::Streaming:
            // Unexpected text in Ready/Streaming states
            // This shouldn't happen, but we'll ignore it
            break;
    }
}

template <Downstream D>
void CramAuth<D>::HandleGreeting(std::string_view line) {
    auto greeting = CramAuthUtils::ParseGreeting(line);
    if (!greeting) {
        this->EmitError(*downstream_,
            Error{ErrorCode::InvalidGreeting,
                  std::string("Invalid greeting: ") + std::string(line)});
        this->RequestClose();
        return;
    }

    greeting_ = std::move(*greeting);
    state_ = CramAuthState::WaitingChallenge;
}

template <Downstream D>
void CramAuth<D>::HandleChallenge(std::string_view line) {
    auto challenge = CramAuthUtils::ParseChallenge(line);
    if (!challenge) {
        this->EmitError(*downstream_,
            Error{ErrorCode::InvalidChallenge,
                  std::string("Invalid challenge: ") + std::string(line)});
        this->RequestClose();
        return;
    }

    // Compute CRAM response
    std::string response = CramAuthUtils::ComputeResponse(*challenge, api_key_);

    // Send auth: auth=response|bucket_id
    // Using empty bucket_id for now - can be extended later
    std::string auth_line = "auth=" + response + "|";
    SendLine(auth_line);

    state_ = CramAuthState::Authenticating;
}

template <Downstream D>
void CramAuth<D>::HandleAuthResponse(std::string_view line) {
    // Check for auth success or failure
    // Server responds with "success=1|session_id=X|" or "success=0|error=msg|"

    // Check for error responses (legacy format)
    if (line.starts_with("err=")) {
        this->EmitError(*downstream_,
            Error{ErrorCode::AuthFailed,
                  std::string("Authentication failed: ") + std::string(line)});
        this->RequestClose();
        return;
    }

    // Parse key=value pairs separated by |
    bool found_success = false;
    bool is_success = false;
    std::string error_msg;

    std::string_view remaining = line;
    while (!remaining.empty()) {
        auto pipe_pos = remaining.find('|');
        std::string_view kv;
        if (pipe_pos != std::string_view::npos) {
            kv = remaining.substr(0, pipe_pos);
            remaining = remaining.substr(pipe_pos + 1);
        } else {
            kv = remaining;
            remaining = {};
        }

        if (kv.empty()) continue;

        auto eq_pos = kv.find('=');
        if (eq_pos == std::string_view::npos) continue;

        auto key = kv.substr(0, eq_pos);
        auto value = kv.substr(eq_pos + 1);

        if (key == "success") {
            found_success = true;
            is_success = (value == "1");
        } else if (key == "error") {
            error_msg = value;
        } else if (key == "session_id") {
            // Update session_id in greeting if provided
            greeting_.session_id = value;
        }
    }

    if (found_success && !is_success) {
        std::string msg = "Authentication failed";
        if (!error_msg.empty()) {
            msg += ": " + error_msg;
        }
        this->EmitError(*downstream_,
            Error{ErrorCode::AuthFailed, msg});
        this->RequestClose();
        return;
    }

    // Auth succeeded
    state_ = CramAuthState::Ready;

    // Send any pending subscription
    SendPendingSubscription();
}

template <Downstream D>
void CramAuth<D>::Subscribe(std::string dataset, std::string symbols,
                            std::string schema) {
    pending_dataset_ = std::move(dataset);
    pending_symbols_ = std::move(symbols);
    pending_schema_ = std::move(schema);
    subscription_sent_ = false;

    if (state_ == CramAuthState::Ready) {
        SendPendingSubscription();
    }
}

template <Downstream D>
void CramAuth<D>::SendPendingSubscription() {
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
        state_ = CramAuthState::Streaming;
    }
}

template <Downstream D>
void CramAuth<D>::StartStreaming() {
    start_requested_ = true;

    if (state_ == CramAuthState::Ready && subscription_sent_) {
        SendLine("start_session");
        state_ = CramAuthState::Streaming;
    }
    // If not ready yet, will be sent after subscription in SendPendingSubscription
}

template <Downstream D>
void CramAuth<D>::SendLine(std::string_view line) {
    std::string with_newline(line);
    with_newline += '\n';

    // Check line fits in segment (64KB limit for auth/subscription lines)
    if (with_newline.size() > Segment::kSize) {
        this->EmitError(*downstream_,
            Error{ErrorCode::BufferOverflow, "Line too large to send"});
        this->RequestClose();
        return;
    }

    // Create BufferChain with single segment for the line
    auto seg = std::make_shared<Segment>();
    std::memcpy(seg->data.data(), with_newline.data(), with_newline.size());
    seg->size = with_newline.size();

    BufferChain chain;
    chain.Append(std::move(seg));

    write_callback_(std::move(chain));
}

template <Downstream D>
void CramAuth<D>::OnError(const Error& e) {
    this->PropagateError(*downstream_, e);
}

template <Downstream D>
void CramAuth<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    // Check for incomplete protocol state
    if (state_ != CramAuthState::Streaming) {
        this->EmitError(*downstream_,
            Error{ErrorCode::ConnectionClosed,
                  "Connection closed during authentication"});
        this->RequestClose();
        return;
    }

    // If suspended, defer OnDone until Resume()
    if (this->IsSuspended()) {
        this->DeferOnDone();
        return;
    }

    // Not suspended - flush and complete immediately
    FlushAndComplete();
}

}  // namespace databento_async
