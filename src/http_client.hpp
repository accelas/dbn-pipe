// src/http_client.hpp
#pragma once

#include <llhttp.h>

#include <cstring>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <vector>

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/error.hpp"
#include "lib/stream/event_loop.hpp"
#include "lib/stream/component.hpp"
#include "tls_transport.hpp"

namespace dbn_pipe {

// HttpClient parses HTTP responses using llhttp.
// Sits between TlsTransport (upstream) and ZstdDecompressor (downstream).
// PipelineComponent provides Suspendable interface with suspend count semantics.
//
// Template parameter D must satisfy the Downstream concept.
template <Downstream D>
class HttpClient : public PipelineComponent<HttpClient<D>>,
                   public std::enable_shared_from_this<HttpClient<D>> {
public:
    // Factory method for shared_from_this safety
    static std::shared_ptr<HttpClient> Create(IEventLoop& loop,
                                               std::shared_ptr<D> downstream) {
        struct MakeSharedEnabler : public HttpClient {
            MakeSharedEnabler(IEventLoop& l, std::shared_ptr<D> ds)
                : HttpClient(l, std::move(ds)) {}
        };
        return std::make_shared<MakeSharedEnabler>(loop, std::move(downstream));
    }

    ~HttpClient() = default;

    // Downstream interface: receive decrypted HTTP response data from TlsTransport
    void OnData(BufferChain& data);

    // Forward errors from upstream
    void OnError(const Error& e);

    // Handle EOF from upstream
    void OnDone();

    // Send data upstream (for HTTP requests)
    void Write(BufferChain data);

    // Required by PipelineComponent
    void DisableWatchers() {
        // No direct epoll watchers; HTTP operates on parsed data
    }

    void DoClose();

    void ProcessPending() {
        // Forward any pending body data first
        if (this->ForwardData(*downstream_, pending_chain_)) return;

        // If message was complete but we suspended during final flush, complete now
        // Skip if OnDone is pending - FlushAndComplete will handle connection closure
        if (message_complete_ && status_code_ < 300 && !this->IsOnDonePending()) {
            // Use ForwardData (not CompleteWithFlush) to avoid setting done_pending_
            // which would trigger FlushAndComplete and close the keep-alive connection
            if (this->ForwardData(*downstream_, pending_chain_)) return;  // Suspended, retry later
            // Check for unconsumed body data
            if (!pending_chain_.Empty()) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::ParseError,
                          "Incomplete body at end of HTTP message (" +
                          std::to_string(pending_chain_.Size()) + " bytes remaining)"});
                this->RequestClose();
                return;
            }
            // Message complete - emit Done but don't close (keep-alive)
            this->EmitDone(*downstream_);
            ResetMessageState();
        }
        // Always resume the parser - it may have been paused even if pending_input_ is empty
        llhttp_resume(&parser_);
        // Process pending input if any
        if (!pending_input_.Empty()) {
            ProcessPendingInput();
        }
    }

    // Process pending input without risk of self-splice
    void ProcessPendingInput() {
        while (!pending_input_.Empty()) {
            size_t chunk_size = pending_input_.ContiguousSize();
            const char* chunk_ptr = reinterpret_cast<const char*>(pending_input_.DataAt(0));

            auto err = llhttp_execute(&parser_, chunk_ptr, chunk_size);

            if (err == HPE_PAUSED) {
                const char* pause_pos = llhttp_get_error_pos(&parser_);
                size_t consumed = static_cast<size_t>(pause_pos - chunk_ptr);
                pending_input_.Consume(consumed);
                return;  // Wait for next OnResume
            }

            if (err == HPE_USER) {
                // Error already emitted in callback
                return;
            }

            if (err != HPE_OK) {
                this->EmitError(*downstream_,
                                Error{ErrorCode::HttpError, llhttp_errno_name(err)});
                this->RequestClose();
                return;
            }

            pending_input_.Consume(chunk_size);
        }
    }

    void FlushAndComplete() {
        // Called by base class when done_pending_ is set (deferred from OnDone or OnResume)
        // Process any buffered input before finishing
        if (!pending_input_.Empty()) {
            llhttp_resume(&parser_);
            ProcessPendingInput();
            if (this->IsSuspended()) {
                this->DeferOnDone();
                return;
            }
        }

        // Handle close-delimited responses if message wasn't complete
        if (!message_complete_) {
            llhttp_errno_t err = llhttp_finish(&parser_);
            if (err != HPE_OK) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::HttpError, llhttp_errno_name(err)});
                this->RequestClose();
                return;
            }
        }

        // Check for HTTP error status
        if (status_code_ >= 300 && !this->IsFinalized()) {
            std::string msg = "HTTP " + std::to_string(status_code_);
            if (!error_body_.empty()) {
                msg += ": " + error_body_;
            }
            this->EmitError(*downstream_, Error{ErrorCode::HttpError, std::move(msg)});
            this->RequestClose();
            return;
        }

        // Flush and complete
        if (this->CompleteWithFlush(*downstream_, pending_chain_)) {
            this->RequestClose();
        }
    }

    // Reset state for HTTP keep-alive
    void ResetMessageState();

    // Accessor for testing
    int StatusCode() const { return status_code_; }
    bool IsMessageComplete() const { return message_complete_; }

private:
    // Private constructor - use Create() factory method
    HttpClient(IEventLoop& loop, std::shared_ptr<D> downstream);

    // llhttp callbacks (static, use parser->data to get this pointer)
    static int OnMessageBegin(llhttp_t* parser);
    static int OnStatus(llhttp_t* parser, const char* at, size_t len);
    static int OnHeaderField(llhttp_t* parser, const char* at, size_t len);
    static int OnHeaderValue(llhttp_t* parser, const char* at, size_t len);
    static int OnHeadersComplete(llhttp_t* parser);
    static int OnBody(llhttp_t* parser, const char* at, size_t len);
    static int OnMessageComplete(llhttp_t* parser);

    // Copy remaining bytes from a partially consumed chain to fresh segments
    // Returns false if overflow detected
    bool CopyToPendingInput(BufferChain& source) {
        while (!source.Empty()) {
            // Check for overflow before copying
            if (source.Size() > kMaxPendingInput - pending_input_.Size()) {
                this->EmitError(*downstream_,
                    Error{ErrorCode::BufferOverflow, "HTTP input buffer overflow"});
                this->RequestClose();
                return false;
            }
            size_t chunk = std::min(source.ContiguousSize(), Segment::kSize);
            auto seg = segment_pool_.Acquire();
            source.CopyTo(0, chunk, seg->data.data());
            seg->size = chunk;
            pending_input_.Append(std::move(seg));
            source.Consume(chunk);
        }
        return true;
    }

    // Copy body bytes to pending_chain_, splitting into segments
    // Returns false if overflow detected
    bool AppendBodyToPending(const std::byte* bytes, size_t len) {
        if (len > kMaxBufferedBody - pending_chain_.Size()) {
            this->EmitError(*downstream_,
                Error{ErrorCode::BufferOverflow, "HTTP body buffer overflow"});
            this->RequestClose();
            return false;
        }
        pending_chain_.AppendBytes(bytes, len, segment_pool_);
        return true;
    }

    // Downstream component
    std::shared_ptr<D> downstream_;

    // llhttp state
    llhttp_t parser_;
    llhttp_settings_t settings_;

    // HTTP response state
    int status_code_ = 0;
    bool message_complete_ = false;

    // Segment pool for body output
    SegmentPool segment_pool_{4};

    // Current segment being filled with body data
    std::shared_ptr<Segment> current_segment_;

    // Pending body chain when suspended (output to downstream)
    BufferChain pending_chain_;

    // Pending input chain when suspended (input from upstream)
    BufferChain pending_input_;

    // Error body for HTTP errors (status >= 300)
    std::string error_body_;

    // Buffer size constants
    static constexpr size_t kMaxBufferedBody = 16 * 1024 * 1024;  // 16MB
    static constexpr size_t kMaxPendingInput = 16 * 1024 * 1024;  // 16MB
    static constexpr size_t kMaxErrorBodySize = 4096;
};

// Implementation - must be in header due to template

template <Downstream D>
HttpClient<D>::HttpClient(IEventLoop& loop, std::shared_ptr<D> downstream)
    : PipelineComponent<HttpClient<D>>(loop), downstream_(std::move(downstream)) {
    // Initialize llhttp settings
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = OnMessageBegin;
    settings_.on_status = OnStatus;
    settings_.on_header_field = OnHeaderField;
    settings_.on_header_value = OnHeaderValue;
    settings_.on_headers_complete = OnHeadersComplete;
    settings_.on_body = OnBody;
    settings_.on_message_complete = OnMessageComplete;

    // Initialize parser for HTTP response parsing
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;

    // Set up segment recycling for pending chain
    pending_chain_.SetRecycleCallback(segment_pool_.MakeRecycler());
}

template <Downstream D>
void HttpClient<D>::DoClose() {
    pending_chain_.Clear();
    pending_input_.Clear();
    current_segment_.reset();
    downstream_.reset();
}

template <Downstream D>
void HttpClient<D>::ResetMessageState() {
    status_code_ = 0;
    message_complete_ = false;
    error_body_.clear();
    pending_chain_.Clear();
    current_segment_.reset();
    this->ResetFinalized();

    // Re-initialize parser for next message (keep-alive)
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

template <Downstream D>
void HttpClient<D>::OnData(BufferChain& data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    // If we have pending input, splice new data and process from pending
    if (!pending_input_.Empty()) {
        // Check for overflow before splicing
        if (data.Size() > kMaxPendingInput - pending_input_.Size()) {
            this->EmitError(*downstream_,
                Error{ErrorCode::BufferOverflow, "HTTP input buffer overflow"});
            this->RequestClose();
            return;
        }
        // Compact both chains if partially consumed before splicing
        if (pending_input_.IsPartiallyConsumed()) {
            pending_input_.Compact();
        }
        if (data.IsPartiallyConsumed()) {
            data.Compact();
        }
        pending_input_.Splice(std::move(data));
        ProcessPendingInput();
        return;
    }

    // Process directly from data
    while (!data.Empty()) {
        size_t chunk_size = data.ContiguousSize();
        const char* chunk_ptr = reinterpret_cast<const char*>(data.DataAt(0));

        auto err = llhttp_execute(&parser_, chunk_ptr, chunk_size);

        if (err == HPE_PAUSED) {
            // Parser was paused due to backpressure
            // Only consume bytes that were actually parsed before the pause
            const char* pause_pos = llhttp_get_error_pos(&parser_);
            size_t consumed = static_cast<size_t>(pause_pos - chunk_ptr);
            data.Consume(consumed);
            // Copy remaining bytes to fresh segments in pending_input_
            // (can't Splice because data now has consumed_offset_ > 0)
            if (!CopyToPendingInput(data)) {
                return;  // Overflow error already emitted
            }
            return;  // Wait for OnResume to continue
        }

        if (err == HPE_USER) {
            // Callback returned error - error was already emitted in callback
            return;
        }

        if (err != HPE_OK) {
            this->EmitError(*downstream_,
                            Error{ErrorCode::HttpError, llhttp_errno_name(err)});
            this->RequestClose();
            return;
        }

        data.Consume(chunk_size);
    }
}

template <Downstream D>
void HttpClient<D>::Write(BufferChain /*data*/) {
    throw std::logic_error(
        "HttpClient::Write() is not supported - HttpClient is a receiver-only "
        "component for HTTP responses. Use TlsTransport directly for sending requests.");
}

template <Downstream D>
void HttpClient<D>::OnError(const Error& e) {
    this->PropagateError(*downstream_, e);
}

template <Downstream D>
void HttpClient<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

    // Process any buffered input before finishing
    if (!pending_input_.Empty()) {
        llhttp_resume(&parser_);
        ProcessPendingInput();
        if (this->IsSuspended()) {
            this->DeferOnDone();
            return;
        }
    }

    if (!message_complete_) {
        // Connection closed before message complete
        // Try to finalize the parser for close-delimited responses
        llhttp_errno_t err = llhttp_finish(&parser_);
        if (err == HPE_OK && !this->IsFinalized()) {
            // Parser finished OK but didn't trigger OnMessageComplete
            // This is fine for close-delimited responses
        } else if (err != HPE_OK) {
            this->EmitError(*downstream_,
                            Error{ErrorCode::HttpError, llhttp_errno_name(err)});
            this->RequestClose();
            return;
        }
    }

    // Check for HTTP error status (connection closed before OnMessageComplete emitted error)
    // Skip if already finalized (OnMessageComplete already emitted error)
    if (status_code_ >= 300 && !this->IsFinalized()) {
        std::string msg = "HTTP " + std::to_string(status_code_);
        if (!error_body_.empty()) {
            msg += ": " + error_body_;
        }
        this->EmitError(*downstream_, Error{ErrorCode::HttpError, std::move(msg)});
        this->RequestClose();
        return;
    }

    // If suspended, defer OnDone until Resume()
    if (this->IsSuspended()) {
        this->DeferOnDone();
        return;
    }

    // Flush pending data and complete - only close if completion happened
    if (this->CompleteWithFlush(*downstream_, pending_chain_)) {
        this->RequestClose();
    }
}

// llhttp callback implementations

template <Downstream D>
int HttpClient<D>::OnMessageBegin(llhttp_t*) {
    return 0;
}

template <Downstream D>
int HttpClient<D>::OnStatus(llhttp_t* parser, const char*, size_t) {
    auto* self = static_cast<HttpClient*>(parser->data);
    self->status_code_ = static_cast<int>(parser->status_code);
    return 0;
}

template <Downstream D>
int HttpClient<D>::OnHeaderField(llhttp_t*, const char*, size_t) {
    // We don't currently track header fields, but could add if needed
    return 0;
}

template <Downstream D>
int HttpClient<D>::OnHeaderValue(llhttp_t*, const char*, size_t) {
    // We don't currently track header values, but could add if needed
    return 0;
}

template <Downstream D>
int HttpClient<D>::OnHeadersComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpClient*>(parser->data);
    // If status >= 300, we'll capture the body for error message
    if (self->status_code_ >= 300) {
        self->error_body_.clear();
    }
    return 0;
}

template <Downstream D>
int HttpClient<D>::OnBody(llhttp_t* parser, const char* at, size_t len) {
    auto* self = static_cast<HttpClient*>(parser->data);

    // For HTTP errors, capture body for error message
    if (self->status_code_ >= 300) {
        size_t remaining = kMaxErrorBodySize - self->error_body_.size();
        self->error_body_.append(at, std::min(len, remaining));
        // Still respect backpressure even for error responses
        if (self->IsSuspended()) {
            llhttp_pause(parser);
        }
        return 0;
    }

    auto* bytes = reinterpret_cast<const std::byte*>(at);

    // Buffer body data (split into segments if needed)
    if (!self->AppendBodyToPending(bytes, len)) {
        return HPE_USER;  // Overflow - error already emitted
    }

    // If suspended, pause parser and wait for resume
    if (self->IsSuspended()) {
        llhttp_pause(parser);
        return 0;
    }

    // Forward to downstream
    self->downstream_->OnData(self->pending_chain_);

    // Check if downstream suspended us during the OnData call
    if (self->IsSuspended()) {
        llhttp_pause(parser);
        return 0;
    }

    return 0;
}

template <Downstream D>
int HttpClient<D>::OnMessageComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpClient*>(parser->data);
    self->message_complete_ = true;

    if (self->status_code_ >= 300) {
        // HTTP error - emit error with body
        std::string msg = "HTTP " + std::to_string(self->status_code_);
        if (!self->error_body_.empty()) {
            msg += ": " + self->error_body_;
        }
        self->EmitError(*self->downstream_, Error{ErrorCode::HttpError, std::move(msg)});
        self->RequestClose();
        return HPE_USER;  // Stop parsing immediately after error
    } else {
        // Check suspend before completion (even for zero-body responses)
        if (self->IsSuspended()) {
            llhttp_pause(parser);
            return 0;
        }
        // Flush any remaining body data before completing
        if (!self->pending_chain_.Empty()) {
            self->downstream_->OnData(self->pending_chain_);
            // If downstream suspended us, pause parser and defer completion
            if (self->IsSuspended()) {
                llhttp_pause(parser);
                return 0;
            }
            // Check for unconsumed data (downstream should have consumed all)
            if (!self->pending_chain_.Empty()) {
                self->EmitError(*self->downstream_,
                    Error{ErrorCode::ParseError,
                          "Incomplete data at end of HTTP message (" +
                          std::to_string(self->pending_chain_.Size()) + " bytes remaining)"});
                self->RequestClose();
                return HPE_USER;
            }
        }
        // Success - emit done and reset for potential keep-alive
        self->EmitDone(*self->downstream_);
        self->ResetMessageState();
    }

    return 0;
}

}  // namespace dbn_pipe
