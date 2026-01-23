# dbWriter Design

PostgreSQL COPY sink with object mapping for streaming market data.

## Overview

Stream `RecordBatch` from dbn-pipe into PostgreSQL using binary COPY protocol with compile-time object mapping.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              dbn-pipe                                       │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                  │
│  │ TcpSocket    │───▶│ DbnParser    │───▶│ RecordBatch  │                  │
│  └──────────────┘    └──────────────┘    └──────┬───────┘                  │
└──────────────────────────────────────────────────┼──────────────────────────┘
                                                   │
                                                   ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                           dbWriter library                                   │
│                                                                              │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌───────────┐ │
│  │ Transform<T> │───▶│  Row<Table>  │───▶│ Mapper<Table>│───▶│CopyWriter │ │
│  │              │    │              │    │  ::to_binary │    │           │ │
│  │ TradeMsg +   │    │ All columns  │    │              │    │ libpq     │ │
│  │ InstrumentMap│    │ populated    │    │ Binary       │    │ async     │ │
│  └──────────────┘    └──────────────┘    │ buffer       │    │ COPY      │ │
│                                          └──────────────┘    └───────────┘ │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                      AsioEventLoop : IEventLoop                      │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Key Layers

- **Transform** - Converts DBN record → Row (handles derived columns, lookups)
- **Table/Row/Mapper** - Compile-time schema, binary serialization
- **CopyWriter** - Async binary COPY over libpq
- **AsioEventLoop** - ASIO-based IEventLoop implementation

## Table/Row Schema Design

Table as source of truth with compile-time schema definition:

```cpp
// Column type maps to PostgreSQL type + binary encoding
template <fixed_string Name, typename CppType, typename PgType = DefaultPgType<CppType>>
struct Column {
    static constexpr auto name = Name;
    using cpp_type = CppType;
    using pg_type = PgType;
};

// Table definition - single source of truth
constexpr auto trades_table = Table{"trades",
    Column<"ts_event_ns",   int64_t,   pg::BigInt>{},
    Column<"ts_event",      Timestamp, pg::Timestamptz>{},
    Column<"rtype",         int16_t,   pg::SmallInt>{},
    Column<"publisher_id",  int16_t,   pg::SmallInt>{},
    Column<"instrument_id", int32_t,   pg::Integer>{},
    Column<"underlying_id", int32_t,   pg::Integer>{},
    Column<"action",        char,      pg::Char>{},
    Column<"side",          char,      pg::Char>{},
    Column<"depth",         int16_t,   pg::SmallInt>{},
    Column<"price",         int64_t,   pg::BigInt>{},
    Column<"size",          int64_t,   pg::BigInt>{},
    Column<"flags",         int16_t,   pg::SmallInt>{},
    Column<"ts_recv_ns",    int64_t,   pg::BigInt>{},
    Column<"ts_recv",       Timestamp, pg::Timestamptz>{},
    Column<"ts_in_delta",   int32_t,   pg::Integer>{},
    Column<"sequence",      int64_t,   pg::BigInt>{},
};

// Row type derived from Table - struct with named members
using TradesRow = decltype(trades_table)::RowType;

// Mapper derived from Table - knows binary encoding
auto mapper = trades_table.mapper();
mapper.to_binary(row, buffer);  // Writes PG binary COPY format
```

## Transform Layer

Transform converts DBN record → Row, handling derived columns:

```cpp
template <typename Record, typename Table>
class Transform {
public:
    using RowType = typename Table::RowType;

    Transform(const InstrumentMap& instruments)
        : instruments_(instruments) {}

    RowType operator()(const Record& rec) const;

private:
    const InstrumentMap& instruments_;
};

// Specialization for TradeMsg → TradesRow
template <>
TradesRow Transform<TradeMsg, trades_table>::operator()(const TradeMsg& msg) const {
    return TradesRow{
        .ts_event_ns   = msg.hd.ts_event,
        .ts_event      = ns_to_timestamp(msg.hd.ts_event),  // derived
        .rtype         = msg.hd.rtype,
        .publisher_id  = msg.hd.publisher_id,
        .instrument_id = msg.hd.instrument_id,
        .underlying_id = instruments_.underlying(msg.hd.instrument_id),  // lookup
        .action        = msg.action,
        .side          = msg.side,
        .depth         = msg.depth,
        .price         = msg.price,
        .size          = msg.size,
        .flags         = msg.flags,
        .ts_recv_ns    = msg.ts_recv,
        .ts_recv       = ns_to_timestamp(msg.ts_recv),  // derived
        .ts_in_delta   = msg.ts_in_delta,
        .sequence      = msg.sequence,
    };
}
```

## CopyWriter & Binary Protocol

CopyWriter handles async libpq binary COPY:

```cpp
// PostgreSQL binary COPY format
// Header: "PGCOPY\n\377\r\n\0" + flags(4) + extension(4)
// Row: field_count(2) + [field_len(4) + data]...
// Trailer: -1(2)

class CopyWriter {
public:
    CopyWriter(PGconn* conn, std::string_view table,
               std::span<const std::string_view> columns);

    asio::awaitable<void> start();
    asio::awaitable<void> write_row(std::span<const std::byte> binary_row);
    asio::awaitable<void> finish();
    asio::awaitable<void> abort();

private:
    asio::awaitable<void> send_data(std::span<const std::byte> data);
    asio::awaitable<void> wait_writable();

    PGconn* conn_;
    asio::posix::stream_descriptor socket_;
};

// Binary encoder per PostgreSQL type
namespace pg {
    struct BigInt {
        static void encode(int64_t val, ByteBuffer& buf) {
            buf.put_int32(8);              // field length
            buf.put_int64_be(val);         // network byte order
        }
    };

    struct Timestamptz {
        static void encode(Timestamp val, ByteBuffer& buf) {
            buf.put_int32(8);
            int64_t pg_usec = val.to_pg_timestamp();  // PG epoch is 2000-01-01
            buf.put_int64_be(pg_usec);
        }
    };
}
```

## AsioEventLoop Integration

AsioEventLoop implements dbn-pipe's IEventLoop interface:

```cpp
class AsioEventLoop : public IEventLoop {
public:
    explicit AsioEventLoop(asio::io_context& ctx);

    void Run() override { ctx_.run(); }
    void Stop() override { ctx_.stop(); }

    void RegisterRead(int fd, std::function<void()> cb) override;
    void RegisterWrite(int fd, std::function<void()> cb) override;
    void Defer(std::function<void()> cb) override {
        asio::post(ctx_, std::move(cb));
    }

    template <typename Awaitable>
    void Spawn(Awaitable&& coro) {
        asio::co_spawn(ctx_, std::forward<Awaitable>(coro), asio::detached);
    }

private:
    asio::io_context& ctx_;
};
```

Data flow with coroutines:

```cpp
client->OnRecord([&](RecordBatch&& batch) {
    if (pending_batches_.size() > kMaxPending) {
        pipeline_.Suspend();
    }
    event_loop.Spawn(process_batch(std::move(batch)));
});

asio::awaitable<void> process_batch(RecordBatch batch) {
    co_await copy_writer.start();
    for (const auto& ref : batch) {
        auto row = transform_(ref.As<TradeMsg>());
        mapper_.to_binary(row, buffer_);
        co_await copy_writer.write_row(buffer_.view());
        buffer_.clear();
    }
    co_await copy_writer.finish();

    if (pending_batches_.size() < kResumePending) {
        pipeline_.Resume();
    }
}
```

## Error Handling & Backpressure

```cpp
enum class WriteError {
    ConnectionLost,
    CopyFailed,
    SerializationError,
    Timeout,
};

struct BackpressureConfig {
    // Byte-based (primary)
    size_t max_pending_bytes = 1 * 1024 * 1024 * 1024;  // 1GB

    // Batch-based (secondary)
    size_t high_water_mark = 256;
    size_t low_water_mark = 64;
};
```

Backpressure flow:

```
dbn-pipe Pipeline                    dbWriter
      │                                  │
      │  RecordBatch                     │
      ├─────────────────────────────────▶│ pending_queue_.push()
      │                                  │
      │                                  │ if (queue.size() > HIGH_WATER)
      │◀─────── Suspend() ───────────────┤
      │                                  │
      │  [pipeline stops reading]        │ [coroutine writes to PG]
      │                                  │
      │                                  │ if (queue.size() < LOW_WATER)
      │◀─────── Resume() ────────────────┤
      │                                  │
      │  [pipeline resumes]              │
```

## Schema Validation

Validate compiled schema against database on startup:

```cpp
class SchemaValidator {
public:
    enum class Mode {
        Strict,     // Fail if any mismatch
        Warn,       // Log warnings, continue
        Bootstrap,  // Create table if missing
    };

    template <typename Table>
    asio::awaitable<std::vector<Mismatch>> validate(
            IDatabase& db, const Table& table, Mode mode);

    template <typename Table>
    asio::awaitable<void> ensure_table(IDatabase& db, const Table& table);
};
```

## Testing Strategy

Mockable interfaces for full coverage:

```cpp
class IDatabase {
public:
    virtual ~IDatabase() = default;
    virtual asio::awaitable<QueryResult> query(std::string_view sql) = 0;
    virtual asio::awaitable<void> execute(std::string_view sql) = 0;
    virtual std::unique_ptr<ICopyWriter> begin_copy(
        std::string_view table, std::span<const std::string_view> columns) = 0;
};

class ICopyWriter {
public:
    virtual ~ICopyWriter() = default;
    virtual asio::awaitable<void> start() = 0;
    virtual asio::awaitable<void> write_row(std::span<const std::byte> data) = 0;
    virtual asio::awaitable<void> finish() = 0;
    virtual asio::awaitable<void> abort() = 0;
};

class IInstrumentMap {
public:
    virtual ~IInstrumentMap() = default;
    virtual std::optional<uint32_t> underlying(uint32_t instrument_id) const = 0;
};
```

Coverage matrix:

| Component | Mock | Covered Scenarios |
|-----------|------|-------------------|
| Transform | IInstrumentMap | Lookup success, failure, edge cases |
| Mapper | None (pure) | All PG types, null handling, overflow |
| BatchWriter | ICopyWriter, IDatabase | Success, retry, abort, backpressure |
| SchemaValidator | IDatabase | Match, mismatch, missing, bootstrap |
| CopyWriter | None (integration) | Real PG in testcontainer |

## File Structure

```
example/dbwriter/
├── DESIGN.md
├── BUILD.bazel
├── include/dbwriter/
│   ├── database.hpp         # IDatabase, IStatement, IRow
│   ├── postgres.hpp         # PostgresDatabase, PostgresCopyWriter
│   ├── table.hpp            # Table, Column, RowType generation
│   ├── mapper.hpp           # Binary encoding per PG type
│   ├── transform.hpp        # Transform<Record, Table>
│   ├── batch_writer.hpp     # BatchWriter with backpressure
│   ├── schema_validator.hpp # Startup validation
│   └── asio_event_loop.hpp  # IEventLoop for ASIO
├── src/
│   ├── postgres.cpp
│   ├── batch_writer.cpp
│   └── schema_validator.cpp
└── test/
    ├── mock_database.hpp
    ├── transform_test.cpp
    ├── mapper_test.cpp
    ├── batch_writer_test.cpp
    └── schema_validator_test.cpp
```

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Architecture | Separate library | Extract later as standalone |
| Schema source | C++ Table definition | Single source of truth |
| Derived columns | Transform layer | Testable, separated concerns |
| Binary format | libpq COPY binary | Performance |
| Async model | ASIO coroutines | Clean async code |
| Threading | Single PG connection | Simplicity, ordered writes |
| Backpressure | Queue with high/low water | 1GB / 256 batches default |
| io_uring | Not used (epoll) | Single socket, minimal benefit |
| Validation | Startup schema check | Fail fast on mismatch |
| Testing | Mockable interfaces | Full coverage |
