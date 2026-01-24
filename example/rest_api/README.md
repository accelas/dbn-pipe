# RestApiPipeline

Coroutine-based REST API pipeline that composes with dbWriter's BatchWriter.

## Usage

```cpp
#include "rest_api/rest_api_pipeline.hpp"
#include "dbwriter/batch_writer.hpp"

// Define JSON builder for your API response
struct MyResponseBuilder {
    using Result = MyResponse;
    // SAX parser implementation...
};

asio::awaitable<void> fetch_data(asio::io_context& ctx, IDatabase& db) {
    rest_api::RestApiPipeline<MyResponseBuilder> api(ctx, "api.example.com");
    api.set_api_key("your_key");

    dbwriter::BatchWriter writer(ctx, db, my_table, MyTransform{});

    for (const auto& id : ids) {
        auto result = co_await api.fetch(
            "/v1/data/{id}",
            {{"id", id}},
            {{"format", "json"}});

        if (result) {
            writer.enqueue(result->records);
        }

        // Rate limiting
        asio::steady_timer timer(ctx, std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }

    co_await writer.drain();
}
```

## API

### Constructor

```cpp
RestApiPipeline(asio::io_context& ctx, std::string host, uint16_t port = 443);
```

### Methods

- `set_api_key(std::string key)` - Set API key for Basic auth
- `fetch(path_template, path_params, query_params)` - Returns `asio::awaitable<std::expected<Result, Error>>`

### Path Parameters

Use `{name}` syntax in path_template:

```cpp
api.fetch("/v2/ticker/{symbol}/range/{start}/{end}",
          {{"symbol", "AAPL"}, {"start", "2024-01-01"}, {"end", "2024-01-31"}},
          {{"apiKey", key}});
```
