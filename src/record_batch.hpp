// SPDX-License-Identifier: MIT

// src/record_batch.hpp
#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

#include <databento/record.hpp>

namespace dbn_pipe {

// Zero-copy reference to a record in BufferChain.
// Provides direct pointer access to aligned record data with lifetime management.
//
// The keepalive shared_ptr ensures the underlying buffer (Segment or scratch buffer)
// remains valid for the lifetime of this reference.
//
// Thread safety: Not thread-safe. Access must be externally synchronized.
struct RecordRef {
    const std::byte* data = nullptr;  // Direct pointer (aligned)
    size_t size = 0;                  // Record size from header
    std::shared_ptr<void> keepalive;  // Segment ref or scratch buffer

    // Zero-copy typed access via reinterpret_cast.
    // The caller is responsible for ensuring T matches the actual record type.
    // The data pointer must be properly aligned (which it will be since
    // Segments are 8-byte aligned and DBN records are 8-byte aligned).
    template <typename T>
    const T& As() const {
        static_assert(alignof(T) <= 8, "Record type alignment must not exceed 8 bytes");
        assert(data != nullptr && "Cannot dereference null data pointer");
        assert(size >= sizeof(T) && "Record size must be at least sizeof(T)");
        return *reinterpret_cast<const T*>(data);
    }

    // Access the record header.
    // All DBN records start with a RecordHeader.
    const databento::RecordHeader& Header() const {
        assert(data != nullptr && "Cannot dereference null data pointer");
        assert(size >= sizeof(databento::RecordHeader) && "Record too small for header");
        return *reinterpret_cast<const databento::RecordHeader*>(data);
    }
};

// Batch of record references for efficient delivery.
// Used by the zero-copy pipeline to deliver multiple records in one callback.
//
// Each RecordRef in the batch holds its own keepalive, ensuring the underlying
// buffer segments remain valid as long as any reference exists.
//
// Thread safety: Not thread-safe. All operations must be called from the same thread.
class RecordBatch {
public:
    explicit RecordBatch(size_t reserve_count = 64) {
        refs_.reserve(reserve_count);
    }

    // Add a record reference to the batch
    void Add(RecordRef ref) { refs_.push_back(std::move(ref)); }

    // Clear all record references from the batch
    void Clear() { refs_.clear(); }

    // Number of records in the batch
    size_t size() const { return refs_.size(); }

    // Check if batch contains no records
    bool empty() const { return refs_.empty(); }

    // Access record at index
    const RecordRef& operator[](size_t i) const {
        assert(i < refs_.size() && "index out of bounds");
        return refs_[i];
    }

    // Range-for support
    auto begin() const { return refs_.begin(); }
    auto end() const { return refs_.end(); }

private:
    std::vector<RecordRef> refs_;
};

}  // namespace dbn_pipe
