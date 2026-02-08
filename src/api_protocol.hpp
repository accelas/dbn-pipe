// SPDX-License-Identifier: MIT

// src/api_protocol.hpp
#pragma once

#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/http_client.hpp"
#include "dbn_pipe/stream/http_request_builder.hpp"
#include "dbn_pipe/stream/json_parser.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"
#include "dbn_pipe/stream/sink.hpp"
#include "dbn_pipe/stream/tcp_socket.hpp"
#include "dbn_pipe/stream/tls_transport.hpp"

namespace dbn_pipe {

// ApiRequest - describes an HTTP request to be sent to a JSON API
struct ApiRequest {
    std::string method = "GET";
    std::string path;
    std::string host = "hist.databento.com";
    uint16_t port = 443;
    std::vector<std::pair<std::string, std::string>> path_params;
    std::vector<std::pair<std::string, std::string>> query_params;
    std::vector<std::pair<std::string, std::string>> form_params;  // For POST

    // Build HTTP request string for this request
    // host_header: Host header value (e.g., "hist.databento.com")
    // api_key: API key for Basic auth (encoded as base64(api_key + ":"))
    std::string BuildHttpRequest(const std::string& host_header,
                                 const std::string& api_key) const {
        // Estimate: method + path + query params + headers typically 200-500 bytes
        std::string out;
        out.reserve(512);

        auto builder = HttpRequestBuilder(std::back_inserter(out))
            .Method(method);

        if (path_params.empty()) {
            builder.Path(path);
        } else {
            builder.PathTemplate(path, path_params);
        }

        for (const auto& [key, value] : query_params) {
            builder.QueryParam(key, value);
        }

        builder
            .Host(host_header)
            .BasicAuth(api_key)
            .Header("Accept", "application/json")
            .Header("Connection", "close");

        if (method == "POST" && !form_params.empty()) {
            builder.FormBody(form_params);
        } else {
            builder.Finish();
        }

        return out;
    }
};

// ApiProtocol - Protocol implementation for JSON API requests
//
// Satisfies the Protocol concept. Uses TLS -> HTTP -> JSON parser chain.
//
// Chain: TcpSocket -> TlsTransport -> HttpClient -> JsonParser -> ResultSink
//
// Template parameter Builder must satisfy the JsonBuilder concept.
// The Builder is set via SetBuilder() after BuildChain() but before Connect().
template<JsonBuilder Builder>
struct ApiProtocol {
    using Request = ApiRequest;
    using Result = typename Builder::Result;
    using SinkType = ResultSink<Result>;

    // ChainType wraps the full pipeline including TcpSocket
    // Type-erased wrapper to avoid exposing template parameters
    struct ChainType {
        virtual ~ChainType() = default;

        // Network lifecycle
        virtual void Connect(const sockaddr_storage& addr) = 0;
        virtual void Close() = 0;

        // Ready callback - fires when chain is ready to send request (after TLS handshake)
        virtual void SetReadyCallback(std::function<void()> cb) = 0;

        // Dataset - no-op for API protocol
        virtual void SetDataset(const std::string&) = 0;

        // Host - sets TLS SNI hostname (must be called before Connect)
        virtual void SetHost(const std::string& host) = 0;

        // Builder - must be set before Connect
        virtual void SetBuilder(Builder& builder) = 0;

        // Backpressure
        virtual void Suspend() = 0;
        virtual void Resume() = 0;
        virtual bool IsSuspended() const = 0;

        // Protocol-specific - for sending HTTP request
        virtual void SendRequest(const std::string& host, const std::string& api_key,
                                 const ApiRequest& request) = 0;

        // Get API key (for SendRequest helper)
        virtual const std::string& GetApiKey() const = 0;
    };

    // Concrete implementation of ChainType
    // TcpSocket is the head, wrapping the rest of the chain
    struct ChainImpl : ChainType {
        ChainImpl(IEventLoop& loop, SinkType& sink, const std::string& api_key)
            : loop_(loop)
            , sink_(sink)
            , api_key_(api_key)
        {}

        // Network lifecycle
        void Connect(const sockaddr_storage& addr) override {
            if (!head_) InitializeChain();
            head_->Connect(addr);
        }

        void Close() override {
            if (head_) head_->Close();
        }

        // Ready callback
        void SetReadyCallback(std::function<void()> cb) override {
            ready_cb_ = std::move(cb);
        }

        // Dataset - no-op for API protocol
        void SetDataset(const std::string&) override {}

        // Host - sets TLS SNI hostname (must be called before Connect)
        void SetHost(const std::string& host) override {
            host_ = host;
        }

        // Builder - must be set before Connect
        void SetBuilder(Builder& builder) override {
            builder_ = &builder;
        }

        // Backpressure - forward to head (TcpSocket)
        void Suspend() override { if (head_) head_->Suspend(); }
        void Resume() override { if (head_) head_->Resume(); }
        bool IsSuspended() const override { return head_ && head_->IsSuspended(); }

        // Get API key
        const std::string& GetApiKey() const override { return api_key_; }

        // Protocol-specific - build and send HTTP request
        void SendRequest(const std::string& host, const std::string& api_key,
                         const ApiRequest& request) override {
            std::string http_request = request.BuildHttpRequest(host, api_key);

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

    private:
        // Initialize the chain (deferred until builder is set)
        void InitializeChain() {
            if (!builder_) {
                std::fprintf(stderr, "ApiProtocol::ChainImpl: Builder not set\n");
                std::terminate();
            }

            // Create JsonParser with callback that delivers to sink
            json_parser_ = JsonParser<Builder>::Create(
                *builder_,
                [this](std::expected<Result, Error> result) {
                    if (result) {
                        sink_.OnResult(std::move(*result));
                    } else {
                        sink_.OnError(result.error());
                    }
                });

            // Create HttpClient with JsonParser as downstream
            http_client_ = HttpClient<JsonParser<Builder>>::Create(loop_, json_parser_);

            // Create TlsTransport with HttpClient as downstream
            tls_ = TlsTransport<HttpClient<JsonParser<Builder>>>::Create(loop_, http_client_);
            tls_->SetHostname(host_);

            // Create TcpSocket with TlsTransport as downstream
            head_ = TcpSocket<TlsTransport<HttpClient<JsonParser<Builder>>>>::Create(loop_, tls_);

            // Set up connect callback to start TLS handshake
            head_->OnConnect([this]() {
                tls_->SetHandshakeCompleteCallback([this]() {
                    if (ready_cb_) ready_cb_();
                });
                tls_->StartHandshake();
            });
        }

        IEventLoop& loop_;
        SinkType& sink_;
        std::string api_key_;
        std::string host_ = "hist.databento.com";
        Builder* builder_ = nullptr;
        std::function<void()> ready_cb_;

        // Pipeline components (created lazily in InitializeChain)
        std::shared_ptr<TcpSocket<TlsTransport<HttpClient<JsonParser<Builder>>>>> head_;
        std::shared_ptr<TlsTransport<HttpClient<JsonParser<Builder>>>> tls_;
        std::shared_ptr<HttpClient<JsonParser<Builder>>> http_client_;
        std::shared_ptr<JsonParser<Builder>> json_parser_;
    };

    // Build the component chain for API protocol
    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop& loop,
        SinkType& sink,
        const std::string& api_key
    ) {
        return std::make_shared<ChainImpl>(loop, sink, api_key);
    }

    // Build the component chain with an explicit allocator
    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop& loop,
        SinkType& sink,
        const std::string& api_key,
        SegmentAllocator* /*alloc*/
    ) {
        // ApiProtocol does not yet use the allocator
        return std::make_shared<ChainImpl>(loop, sink, api_key);
    }

    // Send request - build and send HTTP GET/POST request
    static void SendRequest(std::shared_ptr<ChainType>& chain, const Request& request) {
        if (!chain) return;
        chain->SendRequest(request.host, chain->GetApiKey(), request);
    }

    // Teardown - close the chain
    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) {
            chain->Close();
        }
    }

    // Get gateway hostname from request
    static std::string GetHostname(const Request& request) {
        return request.host;
    }

    // Get gateway port from request
    static uint16_t GetPort(const Request& request) {
        return request.port;
    }
};

}  // namespace dbn_pipe
