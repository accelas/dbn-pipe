# Zero-Copy RecordBatch Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Eliminate memory copies in the record delivery path by using aligned buffer segments with direct typecast access.

**Architecture:** BufferChain owns ref-counted segments allocated with 8-byte alignment. Parser delivers RecordBatch containing RecordRefs that point directly into segment memory. Boundary-crossing records use a per-parser scratch buffer.

**Tech Stack:** C++20, PMR allocators, SO_RCVBUF tuning

---

## Design Principles

### Alignment-Based Zero-Copy

Following databento-cpp's approach:
```cpp
// databento-cpp uses aligned buffers + direct reinterpret_cast
alignas(RecordHeader) std::array<std::byte, kMaxRecordLen> compat_buffer_{};
```

All DBN records require 8-byte alignment (`alignof(TradeMsg) == 8`). By allocating receive buffers with 8-byte alignment, we can use direct `reinterpret_cast` instead of memcpy:

1. **Header peek**: Direct `reinterpret_cast<const RecordHeader*>(data)`
2. **Record access**: Direct `reinterpret_cast<const T*>(data)`

The only case requiring memcpy is **boundary-crossing records** (spanning two buffers) - these must be copied into a contiguous aligned scratch buffer.

**Result**: Zero memcpy for ~99% of records.

---

## Section 1: Data Structures

### Segment

```cpp
// Fixed-size buffer segment with 8-byte alignment
struct Segment {
    static constexpr size_t kSize = 64 * 1024;  // 64KB segments

    alignas(8) std::array<std::byte, kSize> data;
    size_t size = 0;  // Bytes written
    std::atomic<int> ref_count{1};

    void AddRef() { ref_count.fetch_add(1, std::memory_order_relaxed); }
    bool Release() {
        return ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }
};
```

### BufferChain

```cpp
// Chain of segments representing received data
class BufferChain {
public:
    // Add segment to chain (called by socket layer)
    void Append(std::shared_ptr<Segment> seg);

    // Consume bytes from front (called by parser after processing)
    void Consume(size_t bytes);

    // Access data at offset (for parser peeking)
    std::span<const std::byte> PeekAt(size_t offset, size_t len) const;

    // Check if range is contiguous (single segment)
    bool IsContiguous(size_t offset, size_t len) const;

    // Get raw pointer for contiguous access
    const std::byte* DataAt(size_t offset) const;

    // Total unconsumed bytes
    size_t Size() const;

private:
    std::vector<std::shared_ptr<Segment>> segments_;
    size_t consumed_offset_ = 0;  // Bytes consumed from first segment
};
```

### RecordRef

```cpp
// Zero-copy reference to a record in BufferChain
struct RecordRef {
    const std::byte* data;           // Direct pointer (aligned)
    size_t size;                     // Record size from header
    std::shared_ptr<void> keepalive; // Segment ref or scratch buffer

    // Zero-copy typed access via reinterpret_cast
    template <typename T>
    const T& As() const {
        static_assert(alignof(T) <= 8);
        return *reinterpret_cast<const T*>(data);
    }

    const RecordHeader& Header() const {
        return *reinterpret_cast<const RecordHeader*>(data);
    }
};
```

### RecordBatch

```cpp
// Batch of record references for efficient delivery
class RecordBatch {
public:
    explicit RecordBatch(size_t reserve_count = 64) {
        refs_.reserve(reserve_count);
    }

    void Add(RecordRef ref) { refs_.push_back(std::move(ref)); }
    size_t size() const { return refs_.size(); }

    const RecordRef& operator[](size_t i) const { return refs_[i]; }

    // Range-for support
    auto begin() const { return refs_.begin(); }
    auto end() const { return refs_.end(); }

private:
    std::vector<RecordRef> refs_;
};
```

---

## Section 2: Socket Layer

### SO_RCVBUF Tuning

```cpp
// In TcpSocket::Connect() or SetSocketOptions()
void TuneReceiveBuffer(int fd) {
    // Request 256KB receive buffer (kernel may cap this)
    int rcvbuf = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Log actual value for diagnostics
    socklen_t len = sizeof(rcvbuf);
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len);
    // Note: Linux doubles the value, so 256KB request -> 512KB actual
}
```

### Segment Pool

```cpp
class SegmentPool {
public:
    std::shared_ptr<Segment> Acquire() {
        if (!free_list_.empty()) {
            auto seg = std::move(free_list_.back());
            free_list_.pop_back();
            seg->size = 0;
            seg->ref_count.store(1, std::memory_order_relaxed);
            return seg;
        }
        return std::make_shared<Segment>();
    }

    void Release(std::shared_ptr<Segment> seg) {
        if (free_list_.size() < kMaxPoolSize) {
            free_list_.push_back(std::move(seg));
        }
        // else: let it deallocate
    }

private:
    static constexpr size_t kMaxPoolSize = 32;  // ~2MB pool
    std::vector<std::shared_ptr<Segment>> free_list_;
};
```

### Socket Read Loop

```cpp
void TcpSocket::OnReadable() {
    auto segment = pool_.Acquire();

    ssize_t n = ::read(fd_, segment->data.data(), Segment::kSize);
    if (n > 0) {
        segment->size = static_cast<size_t>(n);
        chain_.Append(std::move(segment));

        if (read_callback_) {
            read_callback_(chain_);  // Pass chain, not raw data
        }
    }
    // ... handle errors, EOF
}
```

---

## Section 3: Parser with Scratch Buffer

### Parser Structure

```cpp
class DbnParser {
public:
    static constexpr size_t kMaxRecordSize = 1024;  // Largest DBN record

    // Parse records from chain, return batch
    RecordBatch Parse(BufferChain& chain);

private:
    // Pre-reserved batch to avoid allocation
    RecordBatch batch_{128};  // Reserve for 128 records

    // Note: No per-parser scratch buffer. Boundary-crossing records get
    // individual malloc-allocated buffers owned by RecordRef::keepalive.
};
```

### Parse Implementation

```cpp
RecordBatch DbnParser::Parse(BufferChain& chain) {
    batch_.Clear();  // Reuse pre-allocated batch

    while (chain.Size() >= sizeof(RecordHeader)) {
        // Direct header access - no memcpy (aligned buffer)
        const auto* header = reinterpret_cast<const RecordHeader*>(
            chain.DataAt(0));

        size_t record_size = header->Size();
        if (chain.Size() < record_size) {
            break;  // Incomplete record, wait for more data
        }

        RecordRef ref;
        ref.size = record_size;

        if (chain.IsContiguous(0, record_size)) {
            // Fast path: record entirely in one segment
            // Direct pointer, zero copy
            ref.data = chain.DataAt(0);
            ref.keepalive = chain.GetSegmentAt(0);  // Keep segment alive
        } else {
            // Slow path: record spans segments, allocate scratch per-record
            auto scratch = std::shared_ptr<std::byte[]>(
                new (std::align_val_t{8}) std::byte[record_size],
                [](std::byte* p) { operator delete[](p, std::align_val_t{8}); }
            );
            chain.CopyTo(0, record_size, scratch.get());
            ref.data = scratch.get();
            ref.keepalive = scratch;
        }

        batch_.Add(std::move(ref));
        chain.Consume(record_size);
    }

    return std::move(batch_);
}
```

### Scratch Buffer Lifetime

For boundary-crossing records, each crossing record gets its own malloc-allocated scratch buffer. The scratch buffer is owned by the RecordRef's `keepalive` shared_ptr, so it lives as long as the RecordRef. This allows multiple boundary-crossing records in a single batch without overwriting each other.

```cpp
// For boundary-crossing records, allocate scratch per-record
auto scratch = std::shared_ptr<std::byte[]>(
    new (std::align_val_t{8}) std::byte[record_size],
    [](std::byte* p) { operator delete[](p, std::align_val_t{8}); }
);
chain.CopyTo(0, record_size, scratch.get());
ref.data = scratch.get();
ref.keepalive = scratch;
```

---

## Section 4: Consumer API

### RecordBatch Iteration

```cpp
// In DatabentoManager::HandleBatch
void HandleBatch(RecordBatch&& batch) {
    for (const auto& ref : batch) {
        // Zero-copy header access
        const auto& header = ref.Header();

        switch (header.rtype) {
            case RType::Mbp1: {
                // Zero-copy record access via reinterpret_cast
                const auto& msg = ref.As<Mbp1Msg>();
                // Process msg...
                break;
            }
            case RType::Trade: {
                const auto& msg = ref.As<TradeMsg>();
                // Process msg...
                break;
            }
            // ...
        }
    }
}
```

### Symbol Map Update

```cpp
case RType::SymbolMapping: {
    const auto& msg = ref.As<SymbolMappingMsg>();
    symbol_map_.Insert(msg.hd.instrument_id,
                       std::string(msg.STypeOutSymbol()));
    break;
}
```

---

## Section 5: mango-data Integration

### Record Conversion

```cpp
void DatabentoManager::HandleBatch(RecordBatch&& batch) {
    std::vector<Record> converted;
    converted.reserve(batch.size());

    for (const auto& ref : batch) {
        const auto& header = ref.Header();

        Record rec;
        bool ok = false;

        switch (header.rtype) {
            case RType::Mbp1: {
                // Zero-copy access, then convert to internal format
                rec = ConvertMbp1Msg(ref.As<Mbp1Msg>());
                ok = true;
                break;
            }
            // ... other types
        }

        if (ok) {
            rec.symbol = symbol_map_.Find(rec.instrument_id);
            converted.push_back(std::move(rec));
        }
    }

    if (db_writer_ && !converted.empty()) {
        db_writer_->SubmitBatch(std::move(converted));
    }
}
```

---

## Performance Characteristics

| Metric | Before | After |
|--------|--------|-------|
| Copies per record | 2 (header + body) | 0 (fast path) |
| Allocations per batch | N records | 0 (pre-reserved) |
| Boundary-crossing cost | N/A | 1 copy to scratch |
| Expected boundary rate | N/A | <1% of records |

---

## Implementation Tasks

### Task 1: Segment and BufferChain
- Implement `Segment` struct with aligned storage
- Implement `BufferChain` with append/consume/peek operations
- Unit tests for chain operations

### Task 2: SegmentPool
- Implement pooled segment allocation
- Thread-safety for pool (if needed)
- Unit tests for acquire/release

### Task 3: Socket Layer Integration
- Update `TcpSocket` to use `SegmentPool`
- Add SO_RCVBUF tuning
- Pass `BufferChain&` to read callback

### Task 4: RecordRef and RecordBatch
- Implement `RecordRef` with typed access
- Implement `RecordBatch` with pre-reservation
- Unit tests for typed access

### Task 5: DbnParser Update
- Add scratch buffer for boundary-crossing
- Update parse loop for zero-copy refs
- Unit tests for boundary cases

### Task 6: Pipeline Integration
- Update `OnRead` handler to use new parser
- Update `HandleRecordBatch` signature
- Integration tests

### Task 7: mango-data Update
- Update `HandleBatch` to use `RecordRef::As<T>()`
- Remove memcpy-based dispatch
- Verify build and tests

---

## Open Questions

1. **Pool sizing**: 32 segments = ~2MB. May need tuning based on actual usage patterns.

2. ~~**Scratch buffer ownership**~~: RESOLVED - Using per-record malloc allocation with aligned storage. Each boundary-crossing record gets its own buffer owned by `RecordRef::keepalive`.
