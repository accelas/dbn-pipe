// SPDX-License-Identifier: MIT

// src/record_batch.hpp
#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

#include <databento/record.hpp>

namespace dbn_pipe {

/// Zero-copy reference to a single record in a BufferChain.
///
/// Provides direct pointer access to aligned record data with lifetime
/// management. The @c keepalive shared_ptr ensures the underlying buffer
/// (Segment or scratch buffer) remains valid for the lifetime of this
/// reference.
///
/// Thread safety: Not thread-safe. Access must be externally synchronized.
struct RecordRef {
    const std::byte* data = nullptr;  ///< Direct pointer to record data (8-byte aligned).
    size_t size = 0;                  ///< Record size in bytes (from the record header).
    std::shared_ptr<void> keepalive;  ///< Shared ownership of the underlying Segment or scratch buffer.

    /// Zero-copy typed access via reinterpret_cast.
    ///
    /// The caller is responsible for ensuring @p T matches the actual record
    /// type. The data pointer is guaranteed to be 8-byte aligned because
    /// Segments and DBN records share that alignment requirement.
    /// @tparam T  Concrete record type (e.g., databento::MboMsg).
    /// @return Const reference to the record reinterpreted as @p T.
    template <typename T>
    const T& As() const {
        static_assert(alignof(T) <= 8, "Record type alignment must not exceed 8 bytes");
        assert(data != nullptr && "Cannot dereference null data pointer");
        assert(size >= sizeof(T) && "Record size must be at least sizeof(T)");
        return *reinterpret_cast<const T*>(data);
    }

    /// Access the record header.
    /// All DBN records start with a databento::RecordHeader.
    /// @return Const reference to the record header.
    const databento::RecordHeader& Header() const {
        assert(data != nullptr && "Cannot dereference null data pointer");
        assert(size >= sizeof(databento::RecordHeader) && "Record too small for header");
        return *reinterpret_cast<const databento::RecordHeader*>(data);
    }
};

/// Batch of record references for efficient delivery.
///
/// Used by the zero-copy pipeline to deliver multiple records in a single
/// callback. Each RecordRef in the batch holds its own keepalive, ensuring
/// the underlying buffer segments remain valid as long as any reference
/// exists.
///
/// Thread safety: Not thread-safe. All operations must be called from the
/// same thread.
class RecordBatch {
public:
    /// Construct an empty batch with pre-allocated capacity.
    /// @param reserve_count  Number of RecordRef slots to reserve (default 64).
    explicit RecordBatch(size_t reserve_count = 64) {
        refs_.reserve(reserve_count);
    }

    /// Append a record reference to the batch.
    /// @param ref  Record reference to add (moved in).
    void Add(RecordRef ref) { refs_.push_back(std::move(ref)); }

    /// Remove all record references from the batch.
    void Clear() { refs_.clear(); }

    /// @return Number of records currently in the batch.
    size_t size() const { return refs_.size(); }

    /// @return true if the batch contains no records.
    bool empty() const { return refs_.empty(); }

    /// Access the record reference at the given index.
    /// @param i  Zero-based index (must be less than size()).
    /// @return Const reference to the RecordRef at position @p i.
    const RecordRef& operator[](size_t i) const {
        assert(i < refs_.size() && "index out of bounds");
        return refs_[i];
    }

    /// @return Iterator to the first RecordRef (range-for support).
    auto begin() const { return refs_.begin(); }
    /// @return Past-the-end iterator (range-for support).
    auto end() const { return refs_.end(); }

private:
    std::vector<RecordRef> refs_;
};

}  // namespace dbn_pipe
