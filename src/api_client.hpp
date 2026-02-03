// SPDX-License-Identifier: MIT

// src/api_client.hpp
#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <string>

#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/pipeline.hpp"
#include "dbn_pipe/api_protocol.hpp"
#include "dbn_pipe/dns_resolver.hpp"

namespace dbn_pipe {

// ApiClient<Builder> - High-level client for JSON API requests
//
// Provides a simple callback-based interface for making HTTP requests
// to JSON APIs. Wraps Pipeline<ApiProtocol<Builder>> internally.
//
// Template parameter Builder must satisfy the JsonBuilder concept.
//
// Usage:
//   auto client = ApiClient<MyBuilder>::Create(loop, "api.example.com", builder);
//   client->OnResult([](MyResult r) { ... });
//   client->OnError([](Error e) { ... });
//   client->Request(ApiRequest{.method = "GET", .path = "/v1/data"});
//
// Lifecycle: Single-use per request. Create a new client for each request.
//
// Thread safety: All public methods must be called from event loop thread.
template <JsonBuilder Builder>
class ApiClient : public std::enable_shared_from_this<ApiClient<Builder>> {
    struct PrivateTag {};

public:
    using Result = typename Builder::Result;
    using Protocol = ApiProtocol<Builder>;
    using SinkType = typename Protocol::SinkType;
    using PipelineType = Pipeline<Protocol>;

    // Factory method required for shared_from_this safety.
    static std::shared_ptr<ApiClient> Create(
        IEventLoop& loop,
        std::string host,
        Builder& builder,
        std::string api_key = "",
        uint16_t port = 443
    ) {
        return std::make_shared<ApiClient>(
            PrivateTag{}, loop, std::move(host), builder, std::move(api_key), port);
    }

    ApiClient(PrivateTag, IEventLoop& loop, std::string host, Builder& builder,
              std::string api_key, uint16_t port)
        : loop_(loop)
        , host_(std::move(host))
        , builder_(builder)
        , api_key_(std::move(api_key))
        , port_(port) {}

    ~ApiClient() {
        if (sink_) sink_->Invalidate();
    }

    // =========================================================================
    // Callback registration - call before Request()
    // =========================================================================

    template <typename H>
        requires std::invocable<H, Result>
    void OnResult(H&& h) {
        assert(loop_.IsInEventLoopThread());
        result_handler_ = std::forward<H>(h);
    }

    template <typename H>
        requires std::invocable<H, const Error&>
    void OnError(H&& h) {
        assert(loop_.IsInEventLoopThread());
        error_handler_ = std::forward<H>(h);
    }

    // =========================================================================
    // Request method
    // =========================================================================

    void Request(const ApiRequest& req) {
        assert(loop_.IsInEventLoopThread());
        if (request_sent_) return;  // Single-use
        request_sent_ = true;

        // Resolve hostname
        auto addr = ResolveHostname(host_, port_);
        if (!addr) {
            if (error_handler_) {
                error_handler_(Error{
                    ErrorCode::DnsResolutionFailed,
                    "Failed to resolve hostname: " + host_});
            }
            return;
        }

        // Build the request with host/port from client if not set
        ApiRequest full_req = req;
        if (full_req.host.empty() || full_req.host == "hist.databento.com") {
            full_req.host = host_;
        }
        if (full_req.port == 443 && port_ != 443) {
            full_req.port = port_;
        }
        request_ = full_req;

        BuildPipeline();
        pipeline_->Connect(*addr);
    }

    // Convenience methods for common request types
    void Get(const std::string& path) {
        Request(ApiRequest{.method = "GET", .path = path, .host = host_, .port = port_});
    }

    void Post(const std::string& path,
              std::vector<std::pair<std::string, std::string>> form_params) {
        Request(ApiRequest{
            .method = "POST",
            .path = path,
            .host = host_,
            .port = port_,
            .form_params = std::move(form_params)});
    }

private:
    void BuildPipeline() {
        std::weak_ptr<ApiClient> weak_self = this->shared_from_this();

        // Create sink with callbacks
        sink_ = std::make_shared<SinkType>(
            [weak_self](std::expected<Result, Error> result) {
                if (auto self = weak_self.lock()) {
                    if (result) {
                        if (self->result_handler_) {
                            self->result_handler_(std::move(*result));
                        }
                    } else {
                        if (self->error_handler_) {
                            self->error_handler_(result.error());
                        }
                    }
                }
            });

        // Build chain
        chain_ = Protocol::BuildChain(loop_, *sink_, api_key_);

        // Set builder on chain (must be done before Connect)
        chain_->SetBuilder(builder_);

        // Set TLS SNI hostname
        chain_->SetHost(host_);

        // Set ready callback
        chain_->SetReadyCallback([weak_self]() {
            if (auto self = weak_self.lock()) {
                self->HandleReady();
            }
        });

        // Create pipeline
        pipeline_ = std::make_shared<PipelineType>(
            typename PipelineType::PrivateTag{},
            loop_, chain_, sink_, request_);
    }

    void HandleReady() {
        pipeline_->MarkReady();
        pipeline_->Start();
    }

    IEventLoop& loop_;
    std::string host_;
    Builder& builder_;
    std::string api_key_;
    uint16_t port_;

    bool request_sent_ = false;
    ApiRequest request_;

    std::shared_ptr<SinkType> sink_;
    std::shared_ptr<typename Protocol::ChainType> chain_;
    std::shared_ptr<PipelineType> pipeline_;

    std::function<void(Result)> result_handler_;
    std::function<void(const Error&)> error_handler_;
};

}  // namespace dbn_pipe
