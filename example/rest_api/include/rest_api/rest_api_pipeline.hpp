// SPDX-License-Identifier: MIT

// example/rest_api/include/rest_api/rest_api_pipeline.hpp
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "dbwriter/asio_event_loop.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/stream/json_parser.hpp"
#include "dbn_pipe/stream/pipeline.hpp"
#include "dbn_pipe/api_protocol.hpp"
#include "dbn_pipe/dns_resolver.hpp"

namespace rest_api {

// RestApiPipeline - Coroutine-based REST API fetcher
//
// Fetches JSON from REST APIs and returns parsed results as an awaitable.
// Designed to compose with dbwriter::BatchWriter on the same io_context.
//
// Usage:
//   RestApiPipeline<MyBuilder> api(ctx, "api.example.com");
//   api.set_api_key("my_key");
//   auto result = co_await api.fetch("/v1/data/{id}", {{"id", "123"}}, {{"format", "json"}});
//
// Template parameter Builder must satisfy the JsonBuilder concept.
template<dbn_pipe::JsonBuilder Builder>
class RestApiPipeline {
public:
    using Result = typename Builder::Result;
    using Params = std::vector<std::pair<std::string, std::string>>;

    RestApiPipeline(asio::io_context& ctx,
                    std::string host,
                    uint16_t port = 443)
        : ctx_(ctx)
        , loop_(ctx)
        , host_(std::move(host))
        , port_(port) {}

    void set_api_key(std::string key) { api_key_ = std::move(key); }

    // Fetch single URL, parse JSON, return result
    // path_template: path with {name} placeholders (e.g., "/v1/data/{id}")
    // path_params: substituted into path template (e.g., {{"id", "123"}})
    // query_params: appended as ?key=value&... (e.g., {{"format", "json"}})
    asio::awaitable<std::expected<Result, dbn_pipe::Error>> fetch(
        std::string path_template,
        Params path_params = {},
        Params query_params = {}
    ) {
        // Use a completion token to bridge callback to coroutine
        auto result = co_await asio::async_initiate<
            decltype(asio::use_awaitable),
            void(std::expected<Result, dbn_pipe::Error>)
        >(
            [this, path = std::move(path_template),
             pp = std::move(path_params),
             qp = std::move(query_params)]
            (auto handler) mutable {
                do_fetch(std::move(path), std::move(pp), std::move(qp),
                         std::move(handler));
            },
            asio::use_awaitable
        );

        co_return result;
    }

private:
    template<typename Handler>
    void do_fetch(std::string path, Params path_params,
                  Params query_params, Handler handler) {
        auto addr = dbn_pipe::ResolveHostname(host_, port_);
        if (!addr) {
            asio::post(ctx_, [h = std::move(handler)]() mutable {
                h(std::unexpected(dbn_pipe::Error{
                    dbn_pipe::ErrorCode::DnsResolutionFailed,
                    "Failed to resolve hostname"}));
            });
            return;
        }

        // Create builder (shared to outlive callback chain)
        auto builder = std::make_shared<Builder>();

        using Protocol = dbn_pipe::ApiProtocol<Builder>;
        using SinkType = typename Protocol::SinkType;
        using PipelineType = dbn_pipe::Pipeline<Protocol>;

        // Pipeline holder prevents destruction during callback
        auto pipeline_holder = std::make_shared<std::shared_ptr<PipelineType>>();

        // Handler wrapper to ensure single invocation
        auto handler_ptr = std::make_shared<Handler>(std::move(handler));
        auto called = std::make_shared<bool>(false);

        auto sink = std::make_shared<SinkType>(
            [this, handler_ptr, called, pipeline_holder, builder](auto result) mutable {
                if (*called) return;
                *called = true;
                // Move handler out and invoke
                auto h = std::move(*handler_ptr);
                // Prevent destruction on own call stack
                asio::post(ctx_, [pipeline_holder]() { pipeline_holder->reset(); });
                h(std::move(result));
            });

        auto chain = Protocol::BuildChain(loop_, *sink, api_key_);
        chain->SetBuilder(*builder);
        chain->SetHost(host_);

        dbn_pipe::ApiRequest req{
            .method = "GET",
            .path = std::move(path),
            .host = host_,
            .port = port_,
            .path_params = std::move(path_params),
            .query_params = std::move(query_params),
        };

        *pipeline_holder = std::make_shared<PipelineType>(
            typename PipelineType::PrivateTag{},
            loop_, chain, sink, req);

        std::weak_ptr<PipelineType> weak_pipeline = *pipeline_holder;
        chain->SetReadyCallback([weak_pipeline]() {
            if (auto p = weak_pipeline.lock()) {
                p->MarkReady();
                p->Start();
            }
        });

        (*pipeline_holder)->Connect(*addr);
    }

    asio::io_context& ctx_;
    dbwriter::AsioEventLoop loop_;
    std::string host_;
    uint16_t port_;
    std::string api_key_;
};

}  // namespace rest_api
