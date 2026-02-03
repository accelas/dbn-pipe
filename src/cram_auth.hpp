// SPDX-License-Identifier: MIT

// src/cram_auth.hpp
#pragma once

#include <openssl/sha.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/tls_transport.hpp"  // For Suspendable

namespace dbn_pipe {

// Greeting parsed from server
struct Greeting {
    std::string session_id;
    std::string version;
};

// Utility class for CRAM authentication helpers
class CramAuthUtils {
public:
    // Parse greeting line, supporting both formats:
    // - Legacy: "session_id|version\n"
    // - New LSG: "lsg_version=X.Y.Z (build)\n" or similar key=value format
    static std::optional<Greeting> ParseGreeting(std::string_view data) {
        // Remove trailing newline/carriage return
        auto newline = data.find('\n');
        if (newline != std::string_view::npos) {
            data = data.substr(0, newline);
        }
        if (!data.empty() && data.back() == '\r') {
            data = data.substr(0, data.size() - 1);
        }

        // Try legacy format: "session_id|version"
        auto pipe = data.find('|');
        if (pipe != std::string_view::npos) {
            return Greeting{
                .session_id = std::string(data.substr(0, pipe)),
                .version = std::string(data.substr(pipe + 1)),
            };
        }

        // Try new format: "lsg_version=X.Y.Z" or "key=value (extra)"
        auto eq = data.find('=');
        if (eq != std::string_view::npos) {
            auto value = data.substr(eq + 1);
            // Strip optional parenthetical suffix like " (5)"
            auto paren = value.find(" (");
            if (paren != std::string_view::npos) {
                value = value.substr(0, paren);
            }
            return Greeting{
                .session_id = {},  // Session ID comes in auth response for new format
                .version = std::string(value),
            };
        }

        // Accept any greeting - just store the whole line as version
        // This matches databento-cpp behavior which doesn't strictly validate
        return Greeting{
            .session_id = {},
            .version = std::string(data),
        };
    }

    // Parse "cram=challenge\n"
    static std::optional<std::string> ParseChallenge(std::string_view data) {
        constexpr std::string_view prefix = "cram=";
        if (!data.starts_with(prefix)) {
            return std::nullopt;
        }
        auto newline = data.find('\n', prefix.size());
        if (newline == std::string_view::npos) {
            newline = data.size();
        }
        return std::string(data.substr(prefix.size(), newline - prefix.size()));
    }

    // Compute SHA256(challenge + '|' + api_key) as hex, with bucket_id suffix
    // Format: "<sha256_hex>-<bucket_id>" where bucket_id is last 4 chars of api_key
    static std::string ComputeResponse(std::string_view challenge,
                                       std::string_view api_key) {
        std::string challenge_key;
        challenge_key.reserve(challenge.size() + 1 + api_key.size());
        challenge_key.append(challenge);
        challenge_key.push_back('|');
        challenge_key.append(api_key);

        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(challenge_key.data()),
               challenge_key.size(), digest);

        // SHA256 hex (64 chars) + dash + bucket_id (5 chars) = 70 chars
        std::string result;
        result.reserve(70);
        auto out = std::back_inserter(result);
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            out = fmt::format_to(out, "{:02x}", digest[i]);
        }

        // Append bucket_id (last 5 chars of API key)
        static constexpr std::size_t kBucketIdLength = 5;
        if (api_key.size() >= kBucketIdLength) {
            fmt::format_to(out, "-{}", api_key.substr(api_key.size() - kBucketIdLength));
        }

        return result;
    }

    // Format auth request with required fields
    // Format: auth=<auth>|dataset=<dataset>|encoding=dbn|ts_out=0|client=<client>
    static std::string FormatAuthRequest(std::string_view auth,
                                         std::string_view dataset,
                                         std::string_view client = "dbn-pipe/0.1") {
        return fmt::format("auth={}|dataset={}|encoding=dbn|ts_out=0|client={}",
            auth, dataset, client);
    }
};

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
        IEventLoop& loop,
        std::shared_ptr<D> downstream,
        std::string api_key,
        std::string dataset = {}
    ) {
        return std::make_shared<MakeSharedEnabler>(loop, std::move(downstream),
                                                    std::move(api_key),
                                                    std::move(dataset));
    }

    ~CramAuth() = default;

    // Downstream interface (bytes in from upstream)
    void OnData(BufferChain& data);
    void OnBinaryData(BufferChain& data);
    void OnTextData(BufferChain& data);
    void OnError(const Error& e);
    void OnDone();

    // Set callback for sending data back through the socket
    // Name matches TcpSocket::WireDownstream() auto-wiring convention
    void SetUpstreamWriteCallback(WriteCallback cb) { write_callback_ = std::move(cb); }

    // Subscribe to symbols/schema
    // If already Ready, sends immediately; otherwise queues for later
    // Format matches official API: schema=<s>|stype_in=<t>|id=<n>|symbols=<syms>|snapshot=<b>|is_last=1
    // Note: dataset is no longer needed in subscription - it's sent during auth
    void Subscribe(std::string symbols, std::string schema,
                   std::string stype_in = "raw_symbol",
                   std::string start = {},
                   bool snapshot = false);

    // Start streaming after subscription
    // Sends "start_session\n" and transitions to Streaming state
    void StartStreaming();

    // State accessors
    CramAuthState GetState() const { return state_; }
    const Greeting& GetGreeting() const { return greeting_; }

    // Set dataset for authentication (must be called before auth completes)
    void SetDataset(const std::string& dataset) { dataset_ = dataset; }

    // PipelineComponent requirements
    void DisableWatchers() {}
    void DoClose();

    void ProcessPending() {
        if (!SpliceChecked(streaming_chain_, pending_chain_)) return;
        this->ForwardData(*downstream_, streaming_chain_);
    }

    void FlushAndComplete() {
        if (!SpliceChecked(streaming_chain_, pending_chain_)) return;
        if (this->CompleteWithFlush(*downstream_, streaming_chain_)) {
            this->RequestClose();
        }
    }

    // Buffer limits
    static constexpr std::size_t kMaxPendingData = 16 * 1024 * 1024;  // 16MB
    static constexpr std::size_t kMaxLineLength = 8 * 1024;           // 8KB

private:
    CramAuth(IEventLoop& loop, std::shared_ptr<D> downstream,
             std::string api_key, std::string dataset);

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

    // Emit buffer overflow error and request close
    void EmitOverflow(const char* msg) {
        this->EmitError(*downstream_, Error{ErrorCode::BufferOverflow, msg});
        this->RequestClose();
    }

    // Splice source into target with overflow check. Returns false on overflow.
    bool SpliceChecked(BufferChain& target, BufferChain& source) {
        if (source.Empty()) return true;
        if (target.WouldOverflow(source.Size(), kMaxPendingData)) {
            EmitOverflow("Binary buffer overflow");
            return false;
        }
        target.CompactAndSplice(source);
        return true;
    }

    // Forward data through streaming chain to downstream.
    void ForwardBinaryData(BufferChain& data) {
        if (!SpliceChecked(streaming_chain_, pending_chain_)) return;
        if (!SpliceChecked(streaming_chain_, data)) return;
        downstream_->OnData(streaming_chain_);
    }

    // Handle remaining data after transitioning to streaming mode
    void HandleRemainingData(BufferChain& data) {
        if (data.Empty()) return;
        if (this->IsSuspended()) {
            SpliceChecked(pending_chain_, data);
        } else {
            ForwardBinaryData(data);
        }
    }

    std::shared_ptr<D> downstream_;
    WriteCallback write_callback_ = [](BufferChain) {};

    std::string api_key_;
    std::string dataset_;  // Dataset for auth request
    CramAuthState state_ = CramAuthState::WaitingGreeting;
    Greeting greeting_;

    // Text mode line buffer (used before Streaming state)
    std::vector<std::byte> line_buffer_;

    // Binary mode streaming chain (persistent - retains incomplete records)
    BufferChain streaming_chain_;

    // Binary mode pending chain (used in Streaming state when suspended)
    BufferChain pending_chain_;

    // Subscription parameters (queued until Ready)
    std::optional<std::string> pending_symbols_;
    std::optional<std::string> pending_schema_;
    std::optional<std::string> pending_stype_in_;
    std::optional<std::string> pending_start_;
    bool pending_snapshot_ = false;
    bool subscription_sent_ = false;
    bool start_requested_ = false;
    std::uint32_t sub_counter_ = 0;

    // PMR pool for output buffers (write path only)
    std::pmr::unsynchronized_pool_resource pool_;
    std::pmr::polymorphic_allocator<std::byte> alloc_{&pool_};
};

// MakeSharedEnabler - allows make_shared with private constructor
template <Downstream D>
struct CramAuth<D>::MakeSharedEnabler : public CramAuth<D> {
    MakeSharedEnabler(IEventLoop& loop, std::shared_ptr<D> downstream,
                      std::string api_key, std::string dataset)
        : CramAuth<D>(loop, std::move(downstream),
                      std::move(api_key), std::move(dataset)) {}
};

// Implementation

template <Downstream D>
CramAuth<D>::CramAuth(IEventLoop& loop,
                      std::shared_ptr<D> downstream,
                      std::string api_key,
                      std::string dataset)
    : Base(loop)
    , downstream_(std::move(downstream))
    , api_key_(std::move(api_key))
    , dataset_(std::move(dataset))
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
        OnBinaryData(data);
    } else {
        OnTextData(data);
    }
}

template <Downstream D>
void CramAuth<D>::OnBinaryData(BufferChain& data) {
    if (this->IsSuspended()) {
        SpliceChecked(pending_chain_, data);
    } else {
        ForwardBinaryData(data);
    }
}

template <Downstream D>
void CramAuth<D>::OnTextData(BufferChain& data) {
    while (!data.Empty()) {
        std::byte b;
        data.CopyTo(0, 1, &b);
        data.Consume(1);

        if (line_buffer_.size() >= kMaxLineLength) {
            EmitOverflow("Line buffer overflow");
            return;
        }
        line_buffer_.push_back(b);

        if (b == std::byte{'\n'}) {
            ProcessLineBuffer();
            if (state_ == CramAuthState::Streaming) {
                HandleRemainingData(data);
                return;
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
            streaming_chain_.AppendBytes(line_buffer_.data(), line_buffer_.size());
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

    // Compute CRAM response (includes bucket_id from API key)
    std::string response = CramAuthUtils::ComputeResponse(*challenge, api_key_);

    // Send auth request with dataset and required fields
    // Format: auth=<auth>|dataset=<dataset>|encoding=dbn|ts_out=0|client=dbn-pipe/0.1
    std::string auth_line = CramAuthUtils::FormatAuthRequest(response, dataset_);
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
void CramAuth<D>::Subscribe(std::string symbols, std::string schema,
                            std::string stype_in, std::string start,
                            bool snapshot) {
    pending_symbols_ = std::move(symbols);
    pending_schema_ = std::move(schema);
    pending_stype_in_ = std::move(stype_in);
    pending_start_ = start.empty() ? std::nullopt : std::optional{std::move(start)};
    pending_snapshot_ = snapshot;
    subscription_sent_ = false;

    if (state_ == CramAuthState::Ready) {
        SendPendingSubscription();
    }
}

template <Downstream D>
void CramAuth<D>::SendPendingSubscription() {
    if (subscription_sent_ || !pending_symbols_ || !pending_schema_) {
        return;
    }

    // Increment subscription ID
    ++sub_counter_;

    // Format matches official API:
    // schema=<s>|stype_in=<t>|start=<ts>|id=<n>|symbols=<syms>|snapshot=<b>|is_last=<b>
    std::string msg;
    msg.reserve(128 + pending_symbols_->size());
    auto out = std::back_inserter(msg);
    out = fmt::format_to(out, "schema={}|stype_in={}",
        *pending_schema_, pending_stype_in_.value_or("raw_symbol"));
    if (pending_start_) {
        out = fmt::format_to(out, "|start={}", *pending_start_);
    }
    fmt::format_to(out, "|id={}|symbols={}|snapshot={}|is_last=1",
        sub_counter_, *pending_symbols_, pending_snapshot_ ? "1" : "0");
    // TODO: support chunking for large symbol lists

    SendLine(msg);
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

    // Create BufferChain with the line
    BufferChain chain;
    chain.AppendBytes(with_newline.data(), with_newline.size());

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

}  // namespace dbn_pipe
