# Changelog

All notable changes to dbn-pipe are documented here.

## Unreleased

### Breaking Changes

- **Reactor removed** — `Reactor` has been replaced by `EpollEventLoop` + `Event` + `Timer`. Update `#include "src/reactor.hpp"` to `#include "src/stream.hpp"` and replace `Reactor` with `EpollEventLoop` in all code. (#70)
- **Event class removed** — The standalone `Event` class was replaced by `IEventLoop::Register()` returning `IEventHandle`. (#71)
- **User callbacks deferred** — Record and error callbacks are now deferred to prevent use-after-free when callbacks destroy the pipeline. If your code relied on synchronous callback execution order, verify behavior after upgrading. (#75)
- **RecordRef replaces DbnRecord** — Per-record callbacks now receive `const RecordRef&` instead of `DbnRecord`. (#32)
- **License changed** — Switched from Apache-2.0 to MIT. (#69)

### Added

- **Format-agnostic table definitions** — Compile-time `Table`, `Column`, `Row` types with `ColumnBackend` concept for pluggable serialization (PostgreSQL, Arrow, CSV, etc.). Includes pre-built definitions for trades, mbp1, cmbp1, ohlcv, statistics, definitions, and status schemas. (#77)
- **REST API Pipeline** — `RestApiPipeline` with coroutine-based `fetch()`, `PathTemplate` for URL path parameters (`{name}` syntax), and integration with dbWriter's `BatchWriter`. (#74)
- **dbWriter library** — PostgreSQL COPY sink with compile-time object mapping, `BatchWriter` with backpressure, `SchemaValidator`, and `AsioEventLoop` adapter. (#67)
- **ApiClient\<Builder\>** — High-level wrapper for JSON API requests with automatic retry. (#66)
- **MetadataClient** — Query record counts, cost estimates, and dataset date ranges. (#52)
- **SymbologyClient** — Resolve symbols to instrument IDs with date ranges. (#52)
- **InstrumentMap** — Date-interval symbol resolution with DuckDB-backed cache. (#51)
- **RetryPolicy** — Exponential backoff with jitter and error-aware retry decisions. (#51)
- **TradingDate** — Timezone-aware date conversion from nanosecond timestamps. (#51)
- **DuckDbStorage** — Persistent cache for symbol mappings and download progress with LRU eviction. (#51)
- **Timer class** — Portable periodic and one-shot timers on any `IEventLoop`. (#70)
- **Event class** — Portable fd monitoring on any `IEventLoop`. (#70)
- **EpollEventLoop::Wake()** — Cross-thread wakeup for `Defer()`. (#70)
- **HttpRequestBuilder** — Fluent builder for HTTP requests with `PathTemplate` support. (#64)
- **Pipeline\<Protocol\>** — Unified template replacing separate LivePipeline/HistoricalPipeline classes. (#62)
- **RecordBatch** — Batch callback API for efficient bulk record delivery. (#7)
- **BufferChain** — Zero-copy buffer management with segment pooling. (#8)
- **Schema/SType utilities** — Enum conversions for Databento schemas and symbology types. (#51)
- **stype_out support** — `HistoricalRequest` accepts `stype_out` for `SymbolMappingMsg`. (#60)

### Fixed

- **EPOLLET race condition** — Fixed race in ASIO async waits and libpq I/O where edge-triggered events could be missed. (#76)
- **Use-after-free in callbacks** — User callbacks are now deferred to run after internal state transitions complete. (#75)
- **Reactor Run/Stop race** — Prevented hang when `Stop()` was called before `Run()`. (#46)
- **LSG greeting format** — Support new `lsg_version=X.Y.Z` greeting from live gateway. (#44)
- **Decompressor buffer limits** — Increased to 256MB for large OPRA option chains. (#43)
- **DBN version conversions** — Typed structs for all v1/v2 to v3 message conversions. (#39)

### Changed

- **ostringstream replaced with fmt::format** — Reduced allocations in hot paths. (#68)
- **HTTP client error handling** — Deduplicated error handling patterns. (#72)
- **BufferChain helpers** — Added `Compact()` and `Splice()` for simpler `CramAuth::OnData`. (#73)
