# dbn-pipe API Guide

This guide covers dbn-pipe's utilities beyond the core streaming clients. For live and historical streaming, see [getting-started.md](getting-started.md).

---

## Resolve Symbols

Use `SymbologyClient` to map symbol names to instrument IDs (or vice versa) for a given date range.

```cpp
#include "dbn_pipe/api/symbology_client.hpp"

auto client = dbn_pipe::SymbologyClient::Create(loop, "db-your-api-key");

client->Resolve(
    "GLBX.MDP3",                          // dataset
    {"ESM4", "ESU4"},                      // symbols
    SType::RawSymbol,                      // input type
    SType::InstrumentId,                   // output type
    "2025-01-01",                          // start_date
    "2025-12-31",                          // end_date
    [](auto result) {
        if (result) {
            for (const auto& [symbol, mappings] : result->result) {
                for (const auto& m : mappings) {
                    std::cout << symbol << " -> " << m.symbol
                              << " (" << m.start_date << " to "
                              << m.end_date << ")\n";
                }
            }
        }
    });
```

Retries automatically on transient errors (connection failures, rate limiting, server errors).

---

## Track Symbol Mappings

OPRA options recycle instrument IDs daily. `InstrumentMap` tracks which symbol an ID maps to on a given date, backed by a persistent DuckDB cache.

```cpp
#include "dbn_pipe/instrument_map.hpp"
#include "dbn_pipe/duckdb_storage.hpp"

// Create a persistent cache (or omit path for in-memory)
auto storage = std::make_shared<dbn_pipe::DuckDbStorage>("cache.db", 50000);

// InstrumentMap resolves IDs using trading dates in your timezone
dbn_pipe::InstrumentMap map(storage, "America/New_York");
```

### Populate from a live stream

`SymbolMappingMsg` records arrive at the start of each live session:

```cpp
client->OnRecord([&map](const dbn_pipe::RecordRef& ref) {
    if (ref.Header().rtype == databento::RType::SymbolMapping) {
        map.OnSymbolMappingMsg(ref.As<databento::SymbolMappingMsg>());
    }
});
```

### Populate manually

```cpp
auto start = dbn_pipe::TradingDate::FromIsoString("2025-01-01");
auto end = dbn_pipe::TradingDate::FromIsoString("2025-01-15");
map.Insert(42, "SPY250117C00500000", start, end);
```

### Look up a symbol

```cpp
auto date = dbn_pipe::TradingDate::FromIsoString("2025-01-10");
auto symbol = map.Resolve(42, date);
if (symbol) {
    std::cout << "Instrument 42 is " << *symbol << "\n";
} else {
    // Cache miss — fetch from SymbologyClient, insert, retry
}
```

When the cache reaches `max_mappings`, DuckDbStorage evicts the oldest 10% by access time. Set the limit to 0 for unlimited.

---

## Check Cost and Availability

Use `MetadataClient` to check record counts, cost estimates, and dataset date ranges before downloading.

```cpp
#include "dbn_pipe/api/metadata_client.hpp"

auto client = dbn_pipe::MetadataClient::Create(loop, "db-your-api-key");

// How many records?
client->GetRecordCount(
    "GLBX.MDP3", "ESM4", "trades",
    "2025-01-01", "2025-01-02", "raw_symbol",
    [](auto result) {
        if (result) std::cout << "Records: " << *result << "\n";
    });

// How much will it cost?
client->GetCost(
    "GLBX.MDP3", "ESM4", "trades",
    "2025-01-01", "2025-01-02", "raw_symbol",
    [](auto result) {
        if (result) std::cout << "Cost: $" << *result << "\n";
    });

// What date range is available?
client->GetDatasetRange("GLBX.MDP3", [](auto result) {
    if (result) {
        std::cout << result->start << " to " << result->end << "\n";
    }
});
```

Retries automatically on transient errors.

---

## Handle Retries

`RetryPolicy` provides exponential backoff with jitter for your own retry loops. The metadata and symbology clients use it internally, but you can use it directly for custom API calls.

```cpp
#include "dbn_pipe/retry_policy.hpp"

// Default: 3 retries, 1s initial delay, 30s max, 2x backoff, 0.1 jitter
dbn_pipe::RetryPolicy policy;

// Or customize
dbn_pipe::RetryConfig config{
    .max_retries = 5,
    .initial_delay = std::chrono::milliseconds(2000),
    .max_delay = std::chrono::milliseconds(60000),
    .backoff_multiplier = 2.0,
    .jitter_factor = 0.1
};
dbn_pipe::RetryPolicy policy(config);
```

### Error-aware retry decisions

The policy classifies errors as retryable or not:

```cpp
if (result) {
    // Success
} else if (policy.ShouldRetry(result.error())) {
    // Retryable: ConnectionFailed, ServerError, RateLimited, TlsHandshakeFailed
    policy.RecordAttempt();
    auto delay = policy.GetNextDelay(result.error());  // Respects retry_after header
    ScheduleRetry(delay);
} else {
    // Non-retryable: Unauthorized, ValidationError, NotFound, ParseError
    std::cerr << "Fatal: " << result.error().message << "\n";
}
```

---

## Convert Timestamps to Trading Dates

`TradingDate` converts nanosecond timestamps to calendar dates in a given timezone. This matters for exchanges like CME where the trading day rolls over at 5 PM CT, not midnight UTC.

```cpp
#include "dbn_pipe/trading_date.hpp"

// From ISO string
auto date = dbn_pipe::TradingDate::FromIsoString("2025-01-15");

// From nanosecond timestamp (handles DST automatically)
uint64_t ts = 1736942400000000000ULL;  // 2025-01-15T12:00:00Z
auto ny = dbn_pipe::TradingDate::FromNanoseconds(ts, "America/New_York");
auto chi = dbn_pipe::TradingDate::FromNanoseconds(ts, "America/Chicago");

// Access fields
date.Year();        // 2025
date.Month();       // 1
date.Day();         // 15
date.ToIsoString(); // "2025-01-15"

// Compare
date1 < date2;
date1 == date2;
```

`FromIsoString` throws `std::invalid_argument` for invalid input. `FromNanoseconds` handles DST via `std::chrono::zoned_time`.

---

## Track Download Progress

`DuckDbStorage` can track partial downloads so you can resume interrupted transfers:

```cpp
dbn_pipe::DownloadProgress progress;
progress.sha256_expected = "abc123...";
progress.total_size = 1000000;
progress.completed_ranges = {{0, 100}, {500, 800}};

storage.StoreProgress("job-1", "data.dbn.zst", progress);
auto loaded = storage.LoadProgress("job-1", "data.dbn.zst");
storage.ClearProgress("job-1", "data.dbn.zst");

// Find all incomplete downloads
for (auto& [job_id, filename] : storage.ListIncompleteDownloads()) {
    // Resume...
}
```

---

## Define Table Schemas

The table definitions framework lets you declare database schemas at compile time. Definitions are format-agnostic — the same table works with PostgreSQL, Arrow, CSV, or any backend that implements the `ColumnBackend` concept.

### Built-in tables

dbn-pipe ships with table definitions for all major Databento schemas:

| Header | Tables | Description |
|--------|--------|-------------|
| `src/table/trades.hpp` | `trades_table` | Trade records (16 columns) |
| `src/table/mbp1.hpp` | `mbp1_table`, `cmbp1_table` | Top-of-book quotes: equity (21 cols), consolidated options (20 cols) |
| `src/table/ohlcv.hpp` | `ohlcv_1s_table`, `ohlcv_1m_table`, `ohlcv_1h_table`, `ohlcv_1d_table` | OHLCV bars at various intervals (9 cols each) |
| `src/table/ohlcv.hpp` | `options_ohlcv_1s_table`, ... | Options OHLCV bars (10 cols — adds `underlying_id`) |
| `src/table/statistics.hpp` | `statistics_table`, `options_statistics_table` | Settlement, open interest, etc. (14/15 cols) |
| `src/table/definitions.hpp` | `definitions_table`, `options_definitions_table` | Instrument definitions (43/53 cols) |
| `src/table/status.hpp` | `status_table`, `options_status_table` | Trading status (13/14 cols) |

### Defining a custom table

```cpp
#include "dbn_pipe/table/table.hpp"

using namespace dbn_pipe;

constexpr auto my_table = Table{"my_trades",
    Column<"ts_event",      Timestamp>{},
    Column<"instrument_id", Int32>{},
    Column<"price",         Int64>{},
    Column<"size",          Int64>{},
    Column<"side",          Char>{}
};
```

### Working with rows

```cpp
using MyRow = decltype(my_table)::RowType;

MyRow row{};
row.get<"price">() = 1234500000;
row.get<"instrument_id">() = 15144;
row.get<"side">() = 'B';

// Structured bindings via tuple
auto& [ts, id, price, size, side] = row.as_tuple();
```

### Table metadata

```cpp
my_table.name();          // "my_trades"
my_table.column_count();  // 5
my_table.column_names();  // {"ts_event", "instrument_id", "price", "size", "side"}
```

### Logical types

Each column has a logical type that maps to a C++ type:

| Logical Type | C++ Type | Typical Use |
|-------------|----------|-------------|
| `Int64` | `int64_t` | Prices, timestamps (ns), sizes |
| `Int32` | `int32_t` | Instrument IDs, deltas |
| `Int16` | `int16_t` | RType, publisher ID, flags |
| `Char` | `char` | Side, action |
| `Timestamp` | `int64_t` | Unix nanoseconds (rendered as timestamp by backends) |
| `Text` | `std::string_view` | Symbol names, exchange codes |
| `Bool` | `bool` | Boolean flags |
| `Float64` | `double` | Floating-point values |

### Implementing a backend

To serialize rows into a specific format, implement the `ColumnBackend` concept — a type that provides `encode<LogicalType>(value)` for each logical type. See `example/dbwriter/include/dbwriter/pg_types.hpp` for a PostgreSQL binary COPY backend.

---

## Build REST API Pipelines

`RestApiPipeline` is a coroutine-based HTTP client for REST API calls. It pairs with dbWriter's `BatchWriter` for fetch-and-store workflows.

```cpp
#include "rest_api/rest_api_pipeline.hpp"

asio::awaitable<void> fetch_data(asio::io_context& ctx) {
    rest_api::RestApiPipeline<MyResponseBuilder> api(ctx, "api.example.com");
    api.set_api_key("your_key");

    auto result = co_await api.fetch(
        "/v1/data/{symbol}/range/{start}/{end}",
        {{"symbol", "AAPL"}, {"start", "2024-01-01"}, {"end", "2024-01-31"}},
        {{"format", "json"}});

    if (result) {
        // result->records contains parsed response
    }
}
```

### Path parameters

Use `{name}` placeholders in the URL path. Parameters are substituted from the second argument:

```cpp
api.fetch("/v2/ticker/{symbol}/range/{start}/{end}",
          {{"symbol", "AAPL"}, {"start", "2024-01-01"}, {"end", "2024-01-31"}},
          {{"apiKey", key}});
// Produces: /v2/ticker/AAPL/range/2024-01-01/2024-01-31?apiKey=...
```

### Fetch-and-store pattern

```cpp
dbwriter::BatchWriter writer(ctx, db, my_table, MyTransform{});

for (const auto& id : ids) {
    auto result = co_await api.fetch("/v1/data/{id}", {{"id", id}}, {});
    if (result) {
        writer.enqueue(result->records);
    }

    // Rate limiting
    asio::steady_timer timer(ctx, std::chrono::milliseconds(200));
    co_await timer.async_wait(asio::use_awaitable);
}

co_await writer.drain();
```

For the full REST API Pipeline reference, see [example/rest_api/README.md](../example/rest_api/README.md).

---

## Write to PostgreSQL

The dbWriter library streams `RecordBatch` data into PostgreSQL using binary COPY protocol. Key components:

- **Table/Row/Mapper** — Compile-time schema with binary serialization
- **Transform** — Converts DBN records to rows (handles derived columns, lookups)
- **CopyWriter** — Async binary COPY over libpq
- **BatchWriter** — Queues batches with backpressure (suspends/resumes the pipeline automatically)
- **SchemaValidator** — Validates compiled schema against the database on startup

```cpp
// Backpressure is automatic: BatchWriter calls Suspend() when the
// queue exceeds high_water_mark and Resume() when it drains below
// low_water_mark.

client->OnRecord([&](dbn_pipe::RecordBatch&& batch) {
    event_loop.Spawn(process_batch(std::move(batch)));
});

asio::awaitable<void> process_batch(dbn_pipe::RecordBatch batch) {
    co_await copy_writer.start();
    for (const auto& ref : batch) {
        auto row = transform(ref.As<databento::TradeMsg>());
        mapper.to_binary(row, buffer);
        co_await copy_writer.write_row(buffer.view());
        buffer.clear();
    }
    co_await copy_writer.finish();
}
```

For architecture details, schema definition, and testing strategy, see [example/dbwriter/DESIGN.md](../example/dbwriter/DESIGN.md).

---

## Schema and SType Utilities

Convert between Databento schema names, enums, and record types:

```cpp
#include "dbn_pipe/schema_utils.hpp"
#include "dbn_pipe/stype.hpp"

// Schema
dbn_pipe::Schema schema = dbn_pipe::Schema::Trades;
dbn_pipe::SchemaToString(schema);       // "trades"
dbn_pipe::SchemaToRType(schema);        // RType::Mbp0
dbn_pipe::SchemaFromString("ohlcv-1d"); // Schema::Ohlcv1D

// SType (symbology type)
dbn_pipe::SType stype = dbn_pipe::SType::RawSymbol;
dbn_pipe::STypeToString(stype);         // "raw_symbol"
dbn_pipe::STypeFromString("parent");    // SType::Parent
```
