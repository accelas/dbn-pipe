// src/buffer_chain.hpp
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace databento_async {

// Forward declaration
class SegmentPool;

// Fixed-size buffer segment with 8-byte alignment for zero-copy record access.
// DBN records require 8-byte alignment (alignof(TradeMsg) == 8).
//
// Thread safety: Not thread-safe. Access must be externally synchronized.
struct Segment {
    static constexpr size_t kSize = 64 * 1024;  // 64KB segments

    alignas(8) std::array<std::byte, kSize> data;
    size_t size = 0;  // Bytes written (valid data)

    // Get writable span for receiving data
    std::span<std::byte> WriteSpan() noexcept {
        return std::span{data.data() + size, kSize - size};
    }

    // Get readable span of valid data
    std::span<const std::byte> ReadSpan() const noexcept {
        return std::span{data.data(), size};
    }

    // Remaining capacity
    size_t Remaining() const noexcept { return kSize - size; }

    // Check if segment is full
    bool IsFull() const noexcept { return size >= kSize; }
};

// Chain of segments representing received data.
// Supports efficient append/consume operations for streaming data.
//
// Thread safety: Not thread-safe. All operations must be called from the same
// thread (typically the reactor thread).
//
// Segment recycling: Optionally supports returning consumed segments to a pool
// via SetRecycleCallback(). When set, consumed segments are passed to the
// callback instead of being dropped.
class BufferChain {
public:
    using RecycleCallback = std::function<void(std::shared_ptr<Segment>)>;

    // Set callback for recycling consumed segments back to a pool.
    // The callback is invoked when a segment is fully consumed.
    void SetRecycleCallback(RecycleCallback cb) {
        recycle_callback_ = std::move(cb);
    }

    // Add segment to chain (called by socket layer after read)
    void Append(std::shared_ptr<Segment> seg) {
        assert((!seg || seg->size <= Segment::kSize) &&
               "Segment size exceeds capacity - possible buffer overflow");
        if (seg && seg->size > 0) {
            total_size_ += seg->size;
            segments_.push_back(std::move(seg));
        }
    }

    // Splice all segments from another chain (transfers ownership).
    // The source chain is left empty after this operation.
    // PRECONDITION: 'other' must not have partially consumed data (consumed_offset_ == 0).
    // Use Consume() to remove partial data before splicing if needed.
    // If this chain has no recycle callback, inherits the callback from 'other'.
    void Splice(BufferChain&& other) {
        if (other.Empty()) return;

        // Assert precondition: splicing a partially-consumed chain would corrupt data
        assert(other.consumed_offset_ == 0 &&
               "Cannot splice a partially-consumed chain - call Consume() first");

        // Inherit recycle callback if we don't have one
        if (!recycle_callback_ && other.recycle_callback_) {
            recycle_callback_ = other.recycle_callback_;
        }

        total_size_ += other.total_size_;
        for (auto& seg : other.segments_) {
            segments_.push_back(std::move(seg));
        }
        other.segments_.clear();
        other.consumed_offset_ = 0;
        other.total_size_ = 0;
    }

    // Consume bytes from front (called by parser after processing records).
    // If bytes == 0, this is a no-op.
    void Consume(size_t bytes) noexcept {
        while (bytes > 0 && !segments_.empty()) {
            auto& front = segments_.front();
            size_t available = front->size - consumed_offset_;

            if (bytes >= available) {
                // Consume entire segment
                bytes -= available;
                total_size_ -= available;
                consumed_offset_ = 0;

                // Recycle or drop the segment
                if (recycle_callback_) {
                    recycle_callback_(std::move(front));
                }
                segments_.pop_front();  // O(1) with deque
            } else {
                // Partial consume
                consumed_offset_ += bytes;
                total_size_ -= bytes;
                bytes = 0;
            }
        }
    }

    // Check if range is contiguous (single segment).
    // offset is relative to unconsumed data start.
    //
    // Edge cases:
    // - len == 0: Returns true if offset is valid (within bounds)
    // - Empty chain: Returns false
    bool IsContiguous(size_t offset, size_t len) const noexcept {
        if (segments_.empty()) return false;

        // len == 0 is contiguous if offset is valid
        if (len == 0) return offset <= total_size_;

        size_t pos = consumed_offset_ + offset;
        size_t seg_idx = 0;

        // Find starting segment
        while (seg_idx < segments_.size() && pos >= segments_[seg_idx]->size) {
            pos -= segments_[seg_idx]->size;
            ++seg_idx;
        }

        if (seg_idx >= segments_.size()) return false;

        // Check if entire range fits in this segment
        return pos + len <= segments_[seg_idx]->size;
    }

    // Get raw pointer for contiguous access (offset relative to unconsumed start).
    // Caller must verify IsContiguous() first or ensure offset is valid.
    const std::byte* DataAt(size_t offset) const noexcept {
        size_t pos = consumed_offset_ + offset;
        size_t seg_idx = 0;

        while (seg_idx < segments_.size() && pos >= segments_[seg_idx]->size) {
            pos -= segments_[seg_idx]->size;
            ++seg_idx;
        }

        assert(seg_idx < segments_.size() && "DataAt: offset out of bounds");
        return segments_[seg_idx]->data.data() + pos;
    }

    // Get segment containing data at offset (for keepalive).
    std::shared_ptr<Segment> GetSegmentAt(size_t offset) const {
        size_t pos = consumed_offset_ + offset;
        size_t seg_idx = 0;

        while (seg_idx < segments_.size() && pos >= segments_[seg_idx]->size) {
            pos -= segments_[seg_idx]->size;
            ++seg_idx;
        }

        assert(seg_idx < segments_.size() && "GetSegmentAt: offset out of bounds");
        return segments_[seg_idx];
    }

    // Copy data to destination buffer (for boundary-crossing records).
    // If len == 0, this is a no-op.
    void CopyTo(size_t offset, size_t len, std::byte* dest) const {
        if (len == 0) return;

        size_t pos = consumed_offset_ + offset;
        size_t seg_idx = 0;

        // Find starting segment
        while (seg_idx < segments_.size() && pos >= segments_[seg_idx]->size) {
            pos -= segments_[seg_idx]->size;
            ++seg_idx;
        }

        // Copy from segments
        size_t copied = 0;
        while (copied < len && seg_idx < segments_.size()) {
            const auto& seg = segments_[seg_idx];
            size_t available = seg->size - pos;
            size_t to_copy = std::min(available, len - copied);

            std::memcpy(dest + copied, seg->data.data() + pos, to_copy);
            copied += to_copy;
            pos = 0;
            ++seg_idx;
        }

        assert(copied == len && "CopyTo: not enough data");
    }

    // Total unconsumed bytes. O(1) - cached value.
    size_t Size() const noexcept { return total_size_; }

    // Check if chain is empty (no unconsumed data).
    bool Empty() const noexcept { return total_size_ == 0; }

    // Number of segments in chain.
    size_t SegmentCount() const noexcept { return segments_.size(); }

    // Contiguous bytes available from offset 0 (in first segment).
    // Returns how many bytes can be accessed with DataAt(0) without crossing segments.
    size_t ContiguousSize() const noexcept {
        if (segments_.empty()) return 0;
        return segments_.front()->size - consumed_offset_;
    }

    // Clear all data. Segments are recycled if callback is set.
    void Clear() {
        if (recycle_callback_) {
            for (auto& seg : segments_) {
                recycle_callback_(std::move(seg));
            }
        }
        segments_.clear();
        consumed_offset_ = 0;
        total_size_ = 0;
    }

    // Check if this chain has been partially consumed (consumed_offset_ > 0).
    // A fresh chain (not partially consumed) can be safely Splice'd.
    bool IsPartiallyConsumed() const noexcept {
        return consumed_offset_ > 0;
    }

    // Compact the chain by removing consumed bytes from the first segment.
    // After this call, consumed_offset_ == 0 and the chain can be Splice'd.
    // If the first segment is shared, creates a new segment to avoid corruption.
    void Compact() {
        if (consumed_offset_ == 0 || segments_.empty()) return;

        auto& first = segments_.front();
        size_t remaining = first->size - consumed_offset_;

        // Check if segment is shared (use_count > 1 means others hold references)
        if (first.use_count() > 1) {
            // Create a new segment with the remaining data
            auto new_seg = std::make_shared<Segment>();
            std::memcpy(new_seg->data.data(),
                        first->data.data() + consumed_offset_,
                        remaining);
            new_seg->size = remaining;
            first = std::move(new_seg);
        } else {
            // Safe to modify in place - move remaining bytes to start
            std::memmove(first->data.data(),
                         first->data.data() + consumed_offset_,
                         remaining);
            first->size = remaining;
        }
        consumed_offset_ = 0;
    }

private:
    std::deque<std::shared_ptr<Segment>> segments_;  // O(1) front removal
    size_t consumed_offset_ = 0;  // Bytes consumed from first segment
    size_t total_size_ = 0;       // Cached total unconsumed bytes
    RecycleCallback recycle_callback_;  // Optional recycling callback
};

// Pool for reusing Segment allocations.
// Reduces allocation overhead for high-throughput scenarios.
//
// Thread safety: Not thread-safe. All operations must be called from the same
// thread. For multi-threaded use, external synchronization is required.
class SegmentPool {
public:
    explicit SegmentPool(size_t max_pool_size = kDefaultMaxPoolSize)
        : max_pool_size_(max_pool_size) {}

    // Acquire a segment from pool or allocate new.
    std::shared_ptr<Segment> Acquire() {
        if (!free_list_.empty()) {
            auto seg = std::move(free_list_.back());
            free_list_.pop_back();
            seg->size = 0;  // Reset for reuse
            return seg;
        }
        return std::make_shared<Segment>();
    }

    // Return segment to pool for reuse.
    // Only pools segments with no external references (use_count == 1).
    // If segment is still referenced (e.g., by RecordRef keepalive), it's
    // not pooled and will deallocate when all references are released.
    void Release(std::shared_ptr<Segment> seg) {
        if (seg && seg.use_count() == 1 && free_list_.size() < max_pool_size_) {
            free_list_.push_back(std::move(seg));
        }
        // else: let it deallocate naturally when all refs are gone
    }

    // Create a recycler callback for use with BufferChain::SetRecycleCallback().
    // Returns a lambda that calls Release() on consumed segments.
    BufferChain::RecycleCallback MakeRecycler() {
        return [this](std::shared_ptr<Segment> seg) {
            Release(std::move(seg));
        };
    }

    // Number of segments currently in pool.
    size_t PoolSize() const noexcept { return free_list_.size(); }

    // Maximum pool capacity.
    size_t MaxPoolSize() const noexcept { return max_pool_size_; }

    // Clear all pooled segments.
    void Clear() { free_list_.clear(); }

    static constexpr size_t kDefaultMaxPoolSize = 32;  // ~2MB pool

private:
    std::vector<std::shared_ptr<Segment>> free_list_;
    size_t max_pool_size_;
};

}  // namespace databento_async
