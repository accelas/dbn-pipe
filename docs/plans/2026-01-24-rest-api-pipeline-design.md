# RestApiPipeline Design

## Overview

A coroutine-based REST API pipeline that fetches JSON from URLs and returns parsed results. Designed to compose with dbWriter's BatchWriter on the same `io_context`.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Controller (caller code)                                   │
│  - URL generation / pagination                              │
│  - Rate limiting (timers)                                   │
│  - Result → records extraction                              │
│  - BatchWriter::enqueue()                                   │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ co_await pipeline.fetch(...)
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  RestApiPipeline<Builder>                                   │
│  - Single URL fetch                                         │
│  - JSON parse via Builder                                   │
│  - Returns Result (Builder::Result)                         │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ uses existing infrastructure
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  ApiProtocol / HttpClient / TlsTransport / TcpSocket        │
│  (existing callback-based HTTP stack)                       │
└─────────────────────────────────────────────────────────────┘
```

The pipeline focuses on one thing: fetch URL → parse JSON → return result. The controller handles orchestration (pagination, rate limiting, writing to DB).

## Interface

```cpp
template<JsonBuilder Builder>
class RestApiPipeline {
public:
    using Result = typename Builder::Result;
    using Params = std::vector<std::pair<std::string, std::string>>;

    RestApiPipeline(asio::io_context& ctx,
                    std::string host,
                    uint16_t port = 443);

    void set_api_key(std::string key);

    // Fetch single URL, parse JSON, return result
    // path_template: path with {name} placeholders
    // path_params: substituted into path template
    // query_params: appended as ?key=value&...
    asio::awaitable<std::expected<Result, Error>> fetch(
        std::string path_template,
        Params path_params = {},
        Params query_params = {});
};
```

## Usage Example

```cpp
// Define JSON builder for API response
struct PolygonAggBuilder {
    using Result = PolygonAggResponse;
    // ... SAX parser implementation
};

asio::awaitable<void> fetch_polygon_data(
    asio::io_context& ctx,
    IDatabase& db,
    const std::vector<std::string>& symbols
) {
    RestApiPipeline<PolygonAggBuilder> api(ctx, "api.polygon.io");
    api.set_api_key(polygon_key);

    BatchWriter writer(ctx, db, polygon_aggs_table, PolygonTransform{});

    for (const auto& symbol : symbols) {
        auto result = co_await api.fetch(
            "/v2/aggs/ticker/{ticker}/range/1/day/{start}/{end}",
            {{"ticker", symbol}, {"start", "2024-01-01"}, {"end", "2024-01-31"}},
            {{"apiKey", polygon_key}});

        if (result) {
            writer.enqueue(result->results);  // Extract records from response
        }

        // Rate limiting: caller controls timing
        asio::steady_timer timer(ctx, std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }

    co_await writer.drain();
}
```

## Implementation

### Bridging Callbacks to Coroutines

The existing HTTP stack uses callbacks. RestApiPipeline bridges to `asio::awaitable`:

```cpp
asio::awaitable<std::expected<Result, Error>> fetch(
    std::string path_template,
    Params path_params,
    Params query_params
) {
    // Create promise/future pair for bridging
    std::promise<std::expected<Result, Error>> promise;
    auto future = promise.get_future();

    // Use existing callback machinery
    do_fetch(std::move(path_template), std::move(path_params),
             std::move(query_params),
             [p = std::move(promise)](auto result) mutable {
                 p.set_value(std::move(result));
             });

    // Wait for completion on io_context
    co_return co_await asio::this_coro::executor;
    // Note: actual implementation uses asio-compatible future waiting
}
```

### Internal Fetch

Reuses existing `ApiProtocol` machinery:

```cpp
template<typename Callback>
void do_fetch(std::string path, Params path_params,
              Params query_params, Callback callback) {
    auto addr = ResolveHostname(host_, port_);
    if (!addr) {
        callback(std::unexpected(Error{
            ErrorCode::DnsResolutionFailed,
            "Failed to resolve: " + host_}));
        return;
    }

    auto builder = std::make_shared<Builder>();

    using Protocol = ApiProtocol<Builder>;
    using SinkType = typename Protocol::SinkType;

    auto pipeline = std::make_shared<std::shared_ptr<Pipeline<Protocol>>>();

    auto sink = std::make_shared<SinkType>(
        [callback = std::move(callback), pipeline, this](auto result) mutable {
            callback(std::move(result));
            ctx_.post([pipeline]() { pipeline->reset(); });
        });

    auto chain = Protocol::BuildChain(loop_, *sink, api_key_);
    chain->SetBuilder(*builder);
    chain->SetHost(host_);

    ApiRequest req{
        .method = "GET",
        .path = std::move(path),
        .host = host_,
        .port = port_,
        .path_params = std::move(path_params),
        .query_params = std::move(query_params),
    };

    *pipeline = std::make_shared<Pipeline<Protocol>>(
        typename Pipeline<Protocol>::PrivateTag{},
        loop_, chain, sink, req);

    std::weak_ptr<Pipeline<Protocol>> weak = *pipeline;
    chain->SetReadyCallback([weak]() {
        if (auto p = weak.lock()) { p->MarkReady(); p->Start(); }
    });

    (*pipeline)->Connect(*addr);
}
```

## Core Library Changes

### HttpRequestBuilder

Add `PathTemplate()` method for path parameter substitution:

```cpp
// In lib/stream/http_request_builder.hpp

HttpRequestBuilder& PathTemplate(
    std::string_view path_template,
    std::span<const std::pair<std::string, std::string>> params
) {
    *out_++ = ' ';

    size_t pos = 0;
    while (pos < path_template.size()) {
        size_t brace = path_template.find('{', pos);
        if (brace == std::string_view::npos) {
            out_ = fmt::format_to(out_, "{}", path_template.substr(pos));
            break;
        }

        out_ = fmt::format_to(out_, "{}", path_template.substr(pos, brace - pos));

        size_t end_brace = path_template.find('}', brace);
        if (end_brace == std::string_view::npos) {
            out_ = fmt::format_to(out_, "{}", path_template.substr(brace));
            break;
        }

        auto name = path_template.substr(brace + 1, end_brace - brace - 1);
        for (const auto& [key, value] : params) {
            if (key == name) {
                out_ = fmt::format_to(out_, "{}", value);
                break;
            }
        }

        pos = end_brace + 1;
    }

    return *this;
}
```

### ApiRequest

Add `path_params` field:

```cpp
// In src/api_protocol.hpp

struct ApiRequest {
    std::string method = "GET";
    std::string path;
    std::string host = "hist.databento.com";
    uint16_t port = 443;
    std::vector<std::pair<std::string, std::string>> path_params;   // NEW
    std::vector<std::pair<std::string, std::string>> query_params;
    std::vector<std::pair<std::string, std::string>> form_params;

    std::string BuildHttpRequest(const std::string& host_header,
                                 const std::string& api_key) const {
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
```

## File Structure

```
lib/stream/
└── http_request_builder.hpp    # Add PathTemplate()

src/
└── api_protocol.hpp            # Add path_params to ApiRequest

example/rest_api/
├── DESIGN.md
├── BUILD.bazel
├── include/rest_api/
│   └── rest_api_pipeline.hpp   # RestApiPipeline<Builder>
└── test/
    └── rest_api_pipeline_test.cpp
```

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Pipeline scope | Single URL fetch | Keep focused; controller handles orchestration |
| Async model | `asio::awaitable` | Composes with BatchWriter on same io_context |
| Rate limiting | Caller responsibility | Different APIs have different limits |
| Pagination | Caller responsibility | Controller extracts next_url from Result |
| Path params | `{name}` template syntax | Simple, readable, no escaping needed |
| Construction | Constructor + setters | Matches BatchWriter pattern |
| Retry logic | Not included | Controller can wrap with retry if needed |

## Dependencies

- `//lib/stream` - HTTP, JSON, TLS infrastructure
- `//example/dbwriter:asio_event_loop` - AsioEventLoop adapter (for internal bridging)
