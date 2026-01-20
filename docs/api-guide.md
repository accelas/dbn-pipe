# dbn-pipe API Guide

## TradingDate

Represents a calendar date with timezone-aware conversion from timestamps.

```cpp
#include "src/trading_date.hpp"
using namespace dbn_pipe;

// Parse ISO-8601 string
auto date = TradingDate::FromIsoString("2025-01-15");

// Convert nanoseconds to local date
uint64_t ts = 1736942400000000000ULL;  // 2025-01-15T12:00:00Z
auto ny = TradingDate::FromNanoseconds(ts, "America/New_York");   // 2025-01-15
auto chi = TradingDate::FromNanoseconds(ts, "America/Chicago");   // 2025-01-15

// Access fields
date.Year();        // 2025
date.Month();       // 1
date.Day();         // 15
date.ToIsoString(); // "2025-01-15"

// Compare dates
date1 < date2;
date1 == date2;
```

`FromIsoString` throws `std::invalid_argument` for invalid input.

`FromNanoseconds` handles DST automatically via `std::chrono::zoned_time`.

---

## InstrumentMap

Resolves instrument IDs to symbols with date-interval tracking. Required for OPRA options where IDs recycle daily.

```cpp
#include "src/instrument_map.hpp"
using namespace dbn_pipe;

// Create with storage and timezone
auto storage = std::make_shared<DuckDbStorage>("cache.db");
InstrumentMap map(storage, "America/New_York");

// Insert mapping
auto start = TradingDate::FromIsoString("2025-01-01");
auto end = TradingDate::FromIsoString("2025-01-15");
map.Insert(42, "SPY250117C00500000", start, end);

// Populate from DBN stream
map.OnSymbolMappingMsg(msg);

// Resolve symbol
auto date = TradingDate::FromIsoString("2025-01-10");
auto symbol = map.Resolve(42, date);
if (symbol) {
    // Found: *symbol contains the result
} else {
    // Cache miss: fetch from API, then retry
}

// Inspect state
map.Timezone();  // "America/New_York"
map.Size();      // Number of tracked instrument IDs
```

When `Resolve` returns `std::nullopt`, fetch the mapping from the Databento Symbology API, insert it, then retry.

---

## DuckDbStorage

Persists symbol mappings and download progress. Uses LRU eviction when cache exceeds limit.

```cpp
#include "src/duckdb_storage.hpp"
using namespace dbn_pipe;

// In-memory (default)
DuckDbStorage storage;

// File-backed with 50k entry limit
DuckDbStorage storage("cache.db", 50000);

// Pair with InstrumentMap
auto map = InstrumentMap(
    std::make_shared<DuckDbStorage>("cache.db"),
    "America/New_York"
);
```

### Download Progress

Track partial downloads for resume support:

```cpp
DownloadProgress progress;
progress.sha256_expected = "abc123...";
progress.total_size = 1000000;
progress.completed_ranges = {{0, 100}, {500, 800}};

storage.StoreProgress("job-1", "data.dbn.zst", progress);
auto loaded = storage.LoadProgress("job-1", "data.dbn.zst");
storage.ClearProgress("job-1", "data.dbn.zst");

// Resume incomplete downloads
for (auto& [job_id, filename] : storage.ListIncompleteDownloads()) {
    // Resume...
}
```

When the cache reaches `max_mappings`, the storage evicts the oldest 10% by access time. Set limit to 0 for unlimited.

---

## RetryPolicy

Exponential backoff with jitter for HTTP retries.

```cpp
#include "src/retry_policy.hpp"
using namespace dbn_pipe;

// Default: 3 retries, 1s base, 30s max, 0.5 jitter
RetryPolicy policy;

// Custom
RetryPolicy policy(
    5,       // max_retries
    2000,    // base_delay_ms
    60000,   // max_delay_ms
    0.25     // jitter_factor (0.0-1.0)
);

// Use in retry loop
for (int attempt = 0; policy.ShouldRetry(attempt); ++attempt) {
    auto result = MakeRequest();
    if (result.ok()) return result;
    std::this_thread::sleep_for(policy.GetDelay(attempt));
}
```

---

## Schema and SType

Databento schema types and symbology types.

```cpp
#include "src/schema_utils.hpp"
#include "src/stype.hpp"
using namespace dbn_pipe;

// Schema
Schema schema = Schema::Trades;
SchemaToString(schema);              // "trades"
SchemaToRType(schema);               // RType::Mbp0
SchemaFromString("ohlcv-1d");        // Schema::Ohlcv1D

// SType (symbology)
SType stype = SType::RawSymbol;
STypeToString(stype);                // "raw_symbol"
STypeFromString("parent");           // SType::Parent
```
