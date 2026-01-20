// src/api/api_pipeline.hpp
#pragma once

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/error.hpp"
#include "lib/stream/tcp_socket.hpp"
#include "src/api/json_parser.hpp"
#include "src/api/url_encode.hpp"
#include "src/http_client.hpp"
#include "src/tls_transport.hpp"

namespace dbn_pipe {

// ApiRequest - describes an HTTP request to be sent to a JSON API
struct ApiRequest {
    std::string method = "GET";
    std::string path;
    std::vector<std::pair<std::string, std::string>> query_params;
    std::vector<std::pair<std::string, std::string>> form_params;  // For POST

    // Build the full HTTP request string
    std::string BuildHttpRequest(const std::string& host, const std::string& api_key) const {
        std::ostringstream out;

        // Request line: METHOD /path?query HTTP/1.1
        out << method << " " << path;

        // Add query string for GET requests
        if (!query_params.empty()) {
            out << "?";
            bool first = true;
            for (const auto& [key, value] : query_params) {
                if (!first) out << "&";
                first = false;
                UrlEncode(out, key);
                out << "=";
                UrlEncode(out, value);
            }
        }

        out << " HTTP/1.1\r\n";

        // Headers
        out << "Host: " << host << "\r\n";

        // Basic auth: base64(api_key + ":")
        out << "Authorization: Basic ";
        Base64Encode(out, api_key + ":");
        out << "\r\n";

        out << "Accept: application/json\r\n";
        out << "Connection: close\r\n";

        // POST body
        std::string body;
        if (method == "POST" && !form_params.empty()) {
            std::ostringstream body_stream;
            bool first = true;
            for (const auto& [key, value] : form_params) {
                if (!first) body_stream << "&";
                first = false;
                UrlEncode(body_stream, key);
                body_stream << "=";
                UrlEncode(body_stream, value);
            }
            body = body_stream.str();

            out << "Content-Type: application/x-www-form-urlencoded\r\n";
            out << "Content-Length: " << body.size() << "\r\n";
        }

        // End of headers
        out << "\r\n";

        // Body (if POST)
        if (!body.empty()) {
            out << body;
        }

        return out.str();
    }
};

// ApiPipelineBase - abstract base class for type erasure
//
// Allows storing different ApiPipeline<Builder> types through a common interface.
// Interface matches the plan from docs/plans/2026-01-20-api-clients.md
class ApiPipelineBase {
public:
    virtual ~ApiPipelineBase() = default;

    // Connect to the server
    virtual void Connect(const sockaddr_storage& addr) = 0;

    // Close the connection
    virtual void Close() = 0;

    // Set callback for when TLS handshake completes and request can be sent
    virtual void SetReadyCallback(std::function<void()> cb) = 0;

    // Send the HTTP request
    virtual void SendRequest(const std::string& http_request) = 0;
};

// ApiPipeline - orchestrates the network stack for JSON API requests
//
// Chains: TcpSocket -> TlsTransport -> HttpClient -> JsonParser<Builder>
//
// Template parameter Builder must satisfy the JsonBuilder concept.
//
// Usage:
//   auto pipeline = ApiPipeline<MyBuilder>::Create(loop, host, builder, callback);
//   pipeline->SetReadyCallback([&]() { pipeline->SendRequest(request.BuildHttpRequest(...)); });
//   pipeline->Connect(addr);  // Triggers TLS handshake, then ready callback
//
// Thread safety: Not thread-safe. All methods must be called from the event loop thread.
template <JsonBuilder Builder>
class ApiPipeline : public ApiPipelineBase,
                    public std::enable_shared_from_this<ApiPipeline<Builder>> {
public:
    using Result = typename Builder::Result;
    using Callback = std::function<void(std::expected<Result, Error>)>;

    // Factory method - creates pipeline and wires up callbacks safely
    static std::shared_ptr<ApiPipeline> Create(
        IEventLoop& loop,
        const std::string& host,
        Builder& builder,
        Callback on_complete) {
        auto pipeline = std::shared_ptr<ApiPipeline>(
            new ApiPipeline(loop, host, builder, std::move(on_complete)));
        pipeline->Init();  // Wire callbacks after shared_ptr is valid
        return pipeline;
    }

    void Connect(const sockaddr_storage& addr) override {
        if (complete_) return;
        socket_->Connect(addr);
    }

    void Close() override {
        if (socket_) {
            socket_->Close();
        }
    }

    void SetReadyCallback(std::function<void()> cb) override {
        ready_cb_ = std::move(cb);
    }

    void SendRequest(const std::string& http_request) override {
        if (complete_) return;

        // Convert to BufferChain
        BufferChain chain;
        size_t offset = 0;
        while (offset < http_request.size()) {
            auto seg = std::make_shared<Segment>();
            size_t len = std::min(http_request.size() - offset, Segment::kSize);
            std::memcpy(seg->data.data(), http_request.data() + offset, len);
            seg->size = len;
            chain.Append(std::move(seg));
            offset += len;
        }

        // Write through TLS transport
        tls_->Write(std::move(chain));
    }

    bool IsComplete() const { return complete_; }

private:
    ApiPipeline(
        IEventLoop& loop,
        const std::string& host,
        Builder& builder,
        Callback on_complete)
        : loop_(loop),
          host_(host),
          builder_(builder),
          on_complete_(std::move(on_complete)) {
        // Components created in Init() after shared_ptr is valid
    }

    // Two-phase initialization: create components after shared_ptr is valid
    void Init() {
        // Use weak_ptr in callbacks to avoid reference cycles
        std::weak_ptr<ApiPipeline> weak_self = this->shared_from_this();

        // Build the pipeline bottom-up (from sink to source):
        // JsonParser <- HttpClient <- TlsTransport <- TcpSocket

        // Create JsonParser (sink)
        json_parser_ = JsonParser<Builder>::Create(
            builder_,
            [weak_self](std::expected<Result, Error> result) {
                if (auto self = weak_self.lock()) {
                    self->Complete(std::move(result));
                }
            });

        // Create HttpClient with JsonParser as downstream
        http_client_ = HttpClient<JsonParser<Builder>>::Create(loop_, json_parser_);

        // Create TlsTransport with HttpClient as downstream
        tls_ = TlsTransport<HttpClient<JsonParser<Builder>>>::Create(loop_, http_client_);
        tls_->SetHostname(host_);

        // Create TcpSocket with TlsTransport as downstream
        socket_ = TcpSocket<TlsTransport<HttpClient<JsonParser<Builder>>>>::Create(loop_, tls_);

        // Set up connect callback to start TLS handshake
        socket_->OnConnect([weak_self]() {
            if (auto self = weak_self.lock()) {
                // Capture another weak_ptr for the inner callback
                std::weak_ptr<ApiPipeline> weak_inner = self;
                self->tls_->SetHandshakeCompleteCallback([weak_inner]() {
                    if (auto inner = weak_inner.lock()) {
                        if (inner->ready_cb_) {
                            inner->ready_cb_();
                        }
                    }
                });
                self->tls_->StartHandshake();
            }
        });
    }

    void Complete(std::expected<Result, Error> result) {
        if (complete_) return;
        complete_ = true;

        // Clear callbacks to break any remaining reference cycles
        ready_cb_ = nullptr;

        if (on_complete_) {
            auto cb = std::move(on_complete_);
            on_complete_ = nullptr;
            cb(std::move(result));
        }
    }

    IEventLoop& loop_;
    std::string host_;
    Builder& builder_;
    Callback on_complete_;
    std::function<void()> ready_cb_;

    // Pipeline components (created in Init())
    std::shared_ptr<TcpSocket<TlsTransport<HttpClient<JsonParser<Builder>>>>> socket_;
    std::shared_ptr<TlsTransport<HttpClient<JsonParser<Builder>>>> tls_;
    std::shared_ptr<HttpClient<JsonParser<Builder>>> http_client_;
    std::shared_ptr<JsonParser<Builder>> json_parser_;

    bool complete_ = false;
};

}  // namespace dbn_pipe
