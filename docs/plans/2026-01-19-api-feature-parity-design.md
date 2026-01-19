# Databento API Feature Parity Design

**Date:** 2026-01-19
**Status:** Design Complete
**Goal:** Bring dbn-pipe to feature parity with official Databento Python API

## Overview

This design addresses gaps between dbn-pipe and the official Databento Python API, focusing on:
1. Symbol resolution with date-interval tracking (OPRA recycling, corporate actions)
2. HTTP API clients (Symbology, Metadata)
3. Batch downloads with resume support
4. File reading with metadata extraction

## Phases

| Phase | Scope | Priority |
|-------|-------|----------|
| 1 | Core Symbol Resolution (InstrumentMap) | High |
| 2 | HTTP API Clients (Symbology, Metadata) | High |
| 3 | Utilities Migration (RetryPolicy) | Medium |
| 4 | Schema Infrastructure (schema_utils) | Medium |
| 5 | Extended Features (Batch, DBNStore) | Medium |

**Optional (separate subscription):**
- Reference API (corporate actions, adjustment factors, security master)

---

## Phase 1: Core Symbol Resolution

### Problem

Current `SymbolMap` uses simple `map<instrument_id, symbol>` without date tracking. This fails for:
- **OPRA options:** instrument_ids are recycled daily
- **Corporate actions:** Symbol changes (mergers, ticker changes)
- **Contract rollovers:** Same symbol maps to different instrument_ids over time

### Design: InstrumentMap with Date Intervals

```cpp
namespace dbn_pipe {

// Trading date with America/New_York timezone awareness
class TradingDate {
public:
    static TradingDate FromNanoseconds(uint64_t ns_since_epoch);
    static TradingDate FromIsoString(std::string_view iso_date);
    static TradingDate Today();  // Uses America/New_York, not system timezone

    int Year() const;
    int Month() const;
    int Day() const;
    std::string ToIsoString() const;  // "YYYY-MM-DD"

    bool operator<(const TradingDate& other) const;
    bool operator==(const TradingDate& other) const;

private:
    int32_t days_since_epoch_;  // Compact storage
};

// For OPRA options: parse OCC symbol once at insert time
struct OptionAttributes {
    std::string underlying;      // "SPY"
    TradingDate expiration;      // 2025-01-17
    char put_call;               // 'P' or 'C'
    int64_t strike_price;        // Fixed-point (e.g., 48600000 = $486.00)
};

// Single mapping interval
struct MappingInterval {
    TradingDate start_date;
    TradingDate end_date;
    std::string symbol;
    std::optional<OptionAttributes> option_attrs;  // Populated for OPRA
};

// Main symbol resolution class
class InstrumentMap {
public:
    explicit InstrumentMap(std::shared_ptr<IStorage> storage = nullptr);

    // Insert mapping with date range
    void Insert(uint32_t instrument_id, const std::string& symbol,
                const TradingDate& start, const TradingDate& end);

    // Resolve symbol for specific date - O(log n) binary search
    std::optional<std::string> Resolve(uint32_t instrument_id,
                                        const TradingDate& date) const;

    // Get option attributes (for OPRA data)
    std::optional<OptionAttributes> GetOptionAttributes(
        uint32_t instrument_id, const TradingDate& date) const;

    // Populate from DBN stream records
    void OnSymbolMappingMsg(const databento::SymbolMappingMsg& msg);
    void OnInstrumentDefMsg(const databento::InstrumentDefMsg& msg);

    // Bulk population from symbology API response
    void InsertFromSymbologyResponse(const SymbologyResponse& response);

private:
    // Sorted intervals per instrument_id for binary search
    std::unordered_map<uint32_t, std::vector<MappingInterval>> mappings_;
    std::shared_ptr<IStorage> storage_;  // Optional persistence
};

}  // namespace dbn_pipe
```

### Storage Interface (Dependency Injection)

```cpp
// Abstract storage interface
class IStorage {
public:
    virtual ~IStorage() = default;

    // Symbol map operations
    virtual void StoreMapping(uint32_t instrument_id, const std::string& symbol,
                              const TradingDate& start, const TradingDate& end) = 0;
    virtual std::optional<std::string> LookupSymbol(uint32_t instrument_id,
                                                     const TradingDate& date) = 0;

    // Download progress operations (Phase 5)
    virtual void StoreProgress(const std::string& job_id, const std::string& filename,
                               const DownloadProgress& progress) = 0;
    virtual std::optional<DownloadProgress> LoadProgress(const std::string& job_id,
                                                          const std::string& filename) = 0;
    virtual void ClearProgress(const std::string& job_id, const std::string& filename) = 0;
    virtual std::vector<std::pair<std::string, std::string>> ListIncompleteDownloads() = 0;
};

// Default: no persistence (all writes pass through, all reads miss)
class NoOpStorage : public IStorage { /* ... */ };

// Optional: DuckDB persistence with indexing
class DuckDbStorage : public IStorage {
public:
    explicit DuckDbStorage(const std::filesystem::path& db_path);
    // Tables: symbol_mappings, download_progress
};
```

### Files
- `src/instrument_map.hpp` (~250 lines)
- `src/trading_date.hpp` (~100 lines)
- `src/storage.hpp` (~80 lines)
- `src/duckdb_storage.hpp` (~200 lines, optional)

---

## Phase 2: HTTP API Clients

### Architecture

Reuse existing pipeline components with streaming JSON parser:

```
TcpSocket → TlsTransport → HttpClient → JsonStreamParser → ResponseBuilder<T>
```

**HttpClient enhancements:**
- Status code handling (4xx/5xx errors)
- Retry coordination with RetryPolicy
- Retry-After header support
- Payload-agnostic (streams body to downstream)

### JSON Streaming

Large responses (e.g., OPRA symbology with millions of contracts) require streaming:

```cpp
// SAX-style JSON parser using rapidjson
template <typename Handler>
class JsonStreamParser {
public:
    // Called with body chunks from HttpClient
    void OnRead(std::span<const uint8_t> data);
    void OnComplete();

    // Handler receives SAX events:
    // OnKey(), OnString(), OnNumber(), OnStartObject(), OnEndObject(), etc.
};

// ResponseBuilder concept - one per response type
template <typename T>
class ResponseBuilder {
public:
    void OnKey(std::string_view key);
    void OnString(std::string_view value);
    void OnNumber(double value);
    void OnStartObject();
    void OnEndObject();
    void OnStartArray();
    void OnEndArray();

    T Build();
};
```

### Symbology API Client

```cpp
struct SymbologyRequest {
    std::string dataset;
    std::vector<std::string> symbols;  // Up to 2,000
    SType stype_in = SType::RawSymbol;
    SType stype_out = SType::InstrumentId;
    std::string start_date;  // ISO format
    std::string end_date;
};

struct SymbolInterval {
    std::string start_date;  // d0
    std::string end_date;    // d1
    std::string symbol;      // s
};

struct SymbologyResponse {
    std::map<std::string, std::vector<SymbolInterval>> result;
    std::vector<std::string> partial;
    std::vector<std::string> not_found;
};

class SymbologyClient {
public:
    void Resolve(const SymbologyRequest& request,
                 std::function<void(std::expected<SymbologyResponse, std::string>)> callback);
};
```

### Metadata API Client

```cpp
struct DataQuery {
    std::string dataset;
    std::vector<std::string> symbols;
    Schema schema = Schema::Trades;
    std::string start;
    std::string end;
    SType stype_in = SType::RawSymbol;
    std::optional<uint64_t> limit;
};

struct DatasetRange {
    std::string start_date;
    std::string end_date;
};

class MetadataClient {
public:
    // Discovery (GET)
    void ListPublishers(std::function<void(std::expected<std::vector<PublisherInfo>, std::string>)>);
    void ListDatasets(std::optional<std::string> start, std::optional<std::string> end,
                      std::function<void(std::expected<std::vector<std::string>, std::string>)>);
    void ListSchemas(const std::string& dataset,
                     std::function<void(std::expected<std::vector<std::string>, std::string>)>);
    void GetDatasetRange(const std::string& dataset,
                         std::function<void(std::expected<DatasetRange, std::string>)>);

    // Cost estimation (POST)
    void GetRecordCount(const DataQuery& query,
                        std::function<void(std::expected<uint64_t, std::string>)>);
    void GetBillableSize(const DataQuery& query,
                         std::function<void(std::expected<uint64_t, std::string>)>);
    void GetCost(const DataQuery& query,
                 std::function<void(std::expected<double, std::string>)>);
};
```

### Generic JSON API Caller

All JSON APIs reuse same infrastructure:

```cpp
struct ApiRequest {
    std::string method;  // "GET" or "POST"
    std::string path;    // e.g., "/v0/symbology.resolve"
    std::vector<std::pair<std::string, std::string>> params;
};

template <typename T>
void CallJsonApi(
    IApiClient& api,
    const ApiRequest& request,
    ResponseBuilder<T>& builder,
    std::function<void(std::expected<T, std::string>)> callback);
```

### Files
- `src/http_client.hpp` - Enhanced with error handling + retry (~300 lines)
- `src/json_stream_parser.hpp` - SAX wrapper (~150 lines)
- `src/symbology_client.hpp` (~200 lines)
- `src/metadata_client.hpp` (~250 lines)
- `src/api_request.hpp` - Generic API caller (~100 lines)

### Dependencies
- rapidjson (header-only, SAX parsing)

---

## Phase 3: Utilities Migration

### RetryPolicy

Migrate from mango-data to dbn-pipe. HTTP retry with exponential backoff and jitter:

```cpp
namespace dbn_pipe {

struct RetryConfig {
    uint32_t max_retries = 5;
    std::chrono::milliseconds initial_delay{1000};
    std::chrono::milliseconds max_delay{60000};
    double backoff_multiplier = 2.0;
    double jitter_factor = 0.1;  // +/- 10%
};

class RetryPolicy {
public:
    explicit RetryPolicy(IEventLoop& loop, RetryConfig config = {});

    bool ShouldRetry() const { return attempts_ < config_.max_retries; }

    // Schedule retry with exponential backoff, respects Retry-After header
    void ScheduleRetry(std::optional<std::chrono::seconds> retry_after,
                       std::function<void()> on_retry);

    void Reset() { attempts_ = 0; }

private:
    IEventLoop& loop_;
    RetryConfig config_;
    uint32_t attempts_ = 0;
};

}  // namespace dbn_pipe
```

### Files
- `src/retry_policy.hpp` (~80 lines)

---

## Phase 4: Schema Infrastructure

### Schema Utilities

```cpp
namespace dbn_pipe {

// Schema enum (matches Databento schemas)
enum class Schema {
    Mbo, Mbp1, Mbp10, Trades, Tbbo,
    Ohlcv1S, Ohlcv1M, Ohlcv1H, Ohlcv1D,
    Definition, Statistics, Status, Imbalance,
    Cbbo, Cbbo1S, Cbbo1M, Tcbbo, Bbo1S, Bbo1M
};

// Schema ↔ string conversion
std::optional<Schema> SchemaFromString(std::string_view s);
std::string_view SchemaToString(Schema schema);

// Schema → RType mapping (returns nullopt for unknown)
std::optional<databento::RType> SchemaToRType(Schema schema);

// Dataset → schema name prefix (e.g., "OPRA.PILLAR" → "opra_pillar")
std::string DatasetToSchemaName(const std::string& dataset);

}  // namespace dbn_pipe
```

### Files
- `src/schema_utils.hpp` (~80 lines)

---

## Phase 5: Extended Features

### Batch API Client

For large historical queries with server-side processing:

```cpp
enum class JobState { Queued, Processing, Done, Expired };
enum class SplitDuration { Day, Week, Month, None };
enum class Encoding { Dbn, Csv, Json };
enum class Compression { None, Zstd };

struct BatchJobRequest {
    std::string dataset;
    std::vector<std::string> symbols;
    Schema schema;
    std::string start;
    std::string end;
    Encoding encoding = Encoding::Dbn;
    Compression compression = Compression::Zstd;
    SType stype_in = SType::RawSymbol;
    SType stype_out = SType::InstrumentId;
    SplitDuration split_duration = SplitDuration::Day;
    std::optional<uint64_t> split_size_bytes;
    std::optional<uint64_t> limit;
};

struct BatchJob {
    std::string job_id;
    JobState state;
    uint64_t ts_received;
    Schema schema;
    Encoding encoding;
    Compression compression;
};

struct BatchFile {
    std::string filename;
    std::string sha256_hash;
    uint64_t size;
    std::string https_url;
};

class BatchClient {
public:
    BatchClient(IApiClient& api, const ApiConfig& config,
                std::shared_ptr<IStorage> storage = nullptr);

    void SubmitJob(const BatchJobRequest& request,
                   std::function<void(std::expected<BatchJob, std::string>)> callback);

    void ListJobs(std::optional<JobState> state_filter,
                  std::function<void(std::expected<std::vector<BatchJob>, std::string>)> callback);

    void ListFiles(const std::string& job_id,
                   std::function<void(std::expected<std::vector<BatchFile>, std::string>)> callback);

    // Download with resume support and SHA256 validation
    void Download(const BatchFile& file, const std::filesystem::path& output_dir,
                  std::function<void(std::expected<std::filesystem::path, std::string>)> callback);
};
```

### Download Progress Tracking

Uses shared IStorage (DuckDB) for persistence:

```cpp
struct DownloadProgress {
    std::string sha256_expected;
    uint64_t total_size;
    std::vector<std::pair<uint64_t, uint64_t>> completed_ranges;

    uint64_t BytesCompleted() const;
    std::optional<std::pair<uint64_t, uint64_t>> NextNeededRange(uint64_t chunk_size) const;
    bool IsComplete() const;
};
```

Features:
- HTTP Range header for resume
- SHA256 checksum validation
- Survives across sessions (via DuckDB)
- Query incomplete downloads

### DBNStore (File Reader)

```cpp
struct DbnMetadata {
    uint8_t version;
    std::string dataset;
    std::optional<Schema> schema;  // nullopt if mixed
    SType stype_in;
    SType stype_out;
    uint64_t start;
    uint64_t end;
    Compression compression;
    std::vector<SymbolMapping> mappings;
};

class DbnStore {
public:
    // Factory methods
    static std::expected<DbnStore, DbnError> FromFile(const std::filesystem::path& path);
    static std::expected<DbnStore, DbnError> FromBytes(std::span<const uint8_t> data);

    const DbnMetadata& Metadata() const;

    // Populate InstrumentMap from embedded mappings
    void PopulateMap(InstrumentMap& map) const;

    // Record iteration
    enum class ErrorPolicy { Strict, SkipInvalid, BestEffort };
    void SetErrorPolicy(ErrorPolicy policy);
    void SetWarningCallback(std::function<void(DbnErrorCode, std::string_view)> cb);

    template <typename Callback>
    std::expected<uint64_t, DbnError> Replay(Callback&& callback);

    // Range-based iteration
    Iterator begin();
    Iterator end();

    // Re-export
    std::expected<void, DbnError> ToFile(const std::filesystem::path& path,
                                          Compression compression = Compression::Zstd) const;
};
```

### Error Handling

```cpp
enum class DbnErrorCode {
    // File I/O
    FileNotFound, PermissionDenied, ReadError,
    // Format
    InvalidMagic, UnsupportedVersion, CorruptedHeader, InvalidMetadata,
    // Decompression
    DecompressionError,
    // Record parsing
    InvalidRecord, UnknownRType,
    // Truncation
    TruncatedFile,
};

struct DbnError {
    DbnErrorCode code;
    std::string message;
    std::optional<uint64_t> offset;  // Byte offset where error occurred

    // Factory methods
    static DbnError FileNotFound(const std::filesystem::path& path);
    static DbnError InvalidMagic(std::span<const uint8_t, 4> got);
    static DbnError DecompressionError(const std::string& zstd_msg);
};
```

### Files
- `src/batch_client.hpp` (~200 lines)
- `src/dbn_store.hpp` (~300 lines)
- `src/dbn_error.hpp` (~100 lines)

---

## Out of Scope

### Reference API (Separate Subscription)

Corporate actions, adjustment factors, and security master require a separate Databento Reference subscription. Not included in core dbn-pipe.

If needed later, uses same JSON API infrastructure:

```cpp
class ReferenceClient {
    void GetCorporateActions(...);
    void GetAdjustmentFactors(...);
    void GetSecurityMaster(...);
};
```

---

## Implementation Order

1. **Phase 1** - InstrumentMap (foundation for symbol resolution)
2. **Phase 3** - RetryPolicy (needed by Phase 2)
3. **Phase 4** - Schema utils (needed by Phase 2 and 5)
4. **Phase 2** - HTTP API clients (Symbology, Metadata)
5. **Phase 5** - Batch API, DBNStore

Each phase is independently testable and deployable.

---

## Summary

| Phase | New Files | Lines (est.) | Dependencies |
|-------|-----------|--------------|--------------|
| 1 | 4 | ~630 | DuckDB (optional) |
| 2 | 5 | ~1000 | rapidjson, Phase 3 |
| 3 | 1 | ~80 | - |
| 4 | 1 | ~80 | - |
| 5 | 3 | ~600 | Phase 1, 2, 3, 4 |

**Total:** ~2400 lines of new code across 14 files.
