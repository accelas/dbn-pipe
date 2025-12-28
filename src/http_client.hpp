// src/http_client.hpp
#pragma once

#include <llhttp.h>

#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include "error.hpp"
#include "pipeline.hpp"
#include "reactor.hpp"
#include "tls_socket.hpp"

namespace databento_async {

// HttpClient parses HTTP responses using llhttp.
// Sits between TlsSocket (upstream) and ZstdDecompressor (downstream).
//
// Template parameter D must satisfy the Downstream concept.
template <Downstream D>
class HttpClient : public PipelineComponent<HttpClient<D>>,
                   public Suspendable,
                   public std::enable_shared_from_this<HttpClient<D>> {
public:
    // Factory method for shared_from_this safety
    static std::shared_ptr<HttpClient> Create(Reactor& reactor,
                                               std::shared_ptr<D> downstream) {
        struct MakeSharedEnabler : public HttpClient {
            MakeSharedEnabler(Reactor& r, std::shared_ptr<D> ds)
                : HttpClient(r, std::move(ds)) {}
        };
        return std::make_shared<MakeSharedEnabler>(reactor, std::move(downstream));
    }

    ~HttpClient() = default;

    // Upstream interface: receive decrypted HTTP response data from TlsSocket
    void Read(std::pmr::vector<std::byte> data);

    // Forward errors from upstream
    void OnError(const Error& e);

    // Handle EOF from upstream
    void OnDone();

    // Suspendable interface
    void Suspend() override {
        this->suspended_ = true;
    }

    void Resume() override {
        this->suspended_ = false;
        // Process any pending body data
        if (!pending_body_.empty()) {
            auto body = std::move(pending_body_);
            pending_body_ = std::pmr::vector<std::byte>{&body_pool_};
            downstream_->Read(std::move(body));
        }
    }

    // Pipeline interface
    void Close() { this->RequestClose(); }

    // Send data upstream (for HTTP requests)
    void Write(std::pmr::vector<std::byte> data);

    // Set upstream for control flow
    void SetUpstream(Suspendable* up) { upstream_ = up; }

    // Required by PipelineComponent
    void DisableWatchers() {
        // No direct epoll watchers; HTTP operates on parsed data
    }

    void DoClose();

    // Reset state for HTTP keep-alive
    void ResetMessageState();

    // Accessor for testing
    int StatusCode() const { return status_code_; }
    bool IsMessageComplete() const { return message_complete_; }

private:
    // Private constructor - use Create() factory method
    HttpClient(Reactor& reactor, std::shared_ptr<D> downstream);

    // llhttp callbacks (static, use parser->data to get this pointer)
    static int OnMessageBegin(llhttp_t* parser);
    static int OnStatus(llhttp_t* parser, const char* at, size_t len);
    static int OnHeaderField(llhttp_t* parser, const char* at, size_t len);
    static int OnHeaderValue(llhttp_t* parser, const char* at, size_t len);
    static int OnHeadersComplete(llhttp_t* parser);
    static int OnBody(llhttp_t* parser, const char* at, size_t len);
    static int OnMessageComplete(llhttp_t* parser);

    // Downstream component
    std::shared_ptr<D> downstream_;

    // Upstream for backpressure control
    Suspendable* upstream_ = nullptr;

    // llhttp state
    llhttp_t parser_;
    llhttp_settings_t settings_;

    // HTTP response state
    int status_code_ = 0;
    bool message_complete_ = false;

    // Note: suspended_ is inherited from Suspendable base class

    // PMR pools for body chunks
    std::pmr::unsynchronized_pool_resource body_pool_;

    // Pending body data when suspended
    std::pmr::vector<std::byte> pending_body_{&body_pool_};

    // Error body for HTTP errors (status >= 300)
    std::string error_body_;

    // Buffer size constants
    static constexpr size_t kMaxBufferedBody = 16 * 1024 * 1024;  // 16MB
    static constexpr size_t kMaxErrorBodySize = 4096;
};

// Implementation - must be in header due to template

template <Downstream D>
HttpClient<D>::HttpClient(Reactor& reactor, std::shared_ptr<D> downstream)
    : PipelineComponent<HttpClient<D>>(reactor), downstream_(std::move(downstream)) {
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
}

template <Downstream D>
void HttpClient<D>::DoClose() {
    downstream_.reset();
}

template <Downstream D>
void HttpClient<D>::ResetMessageState() {
    status_code_ = 0;
    message_complete_ = false;
    error_body_.clear();
    pending_body_.clear();
    this->ResetFinalized();

    // Re-initialize parser for next message (keep-alive)
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

template <Downstream D>
void HttpClient<D>::Read(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;

    auto err = llhttp_execute(
        &parser_,
        reinterpret_cast<const char*>(data.data()),
        data.size());

    if (err == HPE_PAUSED) {
        // Parser was paused due to backpressure, resume it for next call
        llhttp_resume(&parser_);
        return;
    }

    if (err != HPE_OK) {
        this->EmitError(*downstream_,
                        Error{ErrorCode::HttpError, llhttp_errno_name(err)});
        this->RequestClose();
    }
}

template <Downstream D>
void HttpClient<D>::Write(std::pmr::vector<std::byte> data) {
    auto guard = this->TryGuard();
    if (!guard) return;
    // Pass through to upstream (TLS layer)
    // This will be wired up when integrated with TlsSocket
}

template <Downstream D>
void HttpClient<D>::OnError(const Error& e) {
    this->EmitError(*downstream_, e);
    this->RequestClose();
}

template <Downstream D>
void HttpClient<D>::OnDone() {
    auto guard = this->TryGuard();
    if (!guard) return;

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

    this->EmitDone(*downstream_);
    this->RequestClose();
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
    // If status >= 400, we'll capture the body for error message
    if (self->status_code_ >= 400) {
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
        return 0;
    }

    // If suspended, buffer the body data
    if (self->suspended_) {
        if (self->pending_body_.size() + len > kMaxBufferedBody) {
            // Buffer overflow - pause parser
            llhttp_pause(parser);
            return 0;
        }
        auto* bytes = reinterpret_cast<const std::byte*>(at);
        self->pending_body_.insert(self->pending_body_.end(), bytes, bytes + len);
        llhttp_pause(parser);
        return 0;
    }

    // Forward body chunk to downstream
    std::pmr::vector<std::byte> chunk{&self->body_pool_};
    auto* bytes = reinterpret_cast<const std::byte*>(at);
    chunk.assign(bytes, bytes + len);
    self->downstream_->Read(std::move(chunk));

    // Check if downstream suspended us during the Read call
    if (self->suspended_) {
        llhttp_pause(parser);
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
    } else {
        // Success - emit done
        self->EmitDone(*self->downstream_);
    }

    // Reset for potential keep-alive
    self->ResetMessageState();
    return 0;
}

}  // namespace databento_async
