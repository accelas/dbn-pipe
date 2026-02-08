// SPDX-License-Identifier: MIT

// lib/stream/segment_allocator.hpp
#pragma once

#include <memory>
#include <memory_resource>
#include <vector>

#include "dbn_pipe/stream/buffer_chain.hpp"

namespace dbn_pipe {

/// Allocator for Segment objects backed by a PMR memory resource.
///
/// Wraps a shared_ptr<memory_resource> so that outstanding Segments keep
/// the resource alive (the allocator stored in shared_ptr's control block
/// holds a copy of the shared_ptr).
///
/// Includes a free list for segment recycling, similar to the old SegmentPool.
///
/// Thread safety: The free list is NOT thread-safe --- all Allocate/Release
/// calls must happen on the event loop thread. The underlying memory_resource
/// controls deallocation thread safety (synchronized_pool_resource is safe
/// for cross-thread deallocation via shared_ptr destructor).
class SegmentAllocator {
public:
    /// Construct with default synchronized_pool_resource.
    SegmentAllocator()
        : resource_(std::make_shared<std::pmr::synchronized_pool_resource>()) {}

    /// Construct with caller-provided resource.
    explicit SegmentAllocator(std::shared_ptr<std::pmr::memory_resource> resource)
        : resource_(std::move(resource)) {}

    /// Allocate a Segment. Reuses from free list if available, otherwise
    /// allocates from the PMR via allocate_shared.
    std::shared_ptr<Segment> Allocate() {
        if (!free_list_.empty()) {
            auto seg = std::move(free_list_.back());
            free_list_.pop_back();
            seg->size = 0;
            return seg;
        }
        return std::allocate_shared<Segment>(PmrAllocator{resource_});
    }

    /// Return a segment to the free list. Only recycles if this is the
    /// last external reference (use_count == 1).
    void Release(std::shared_ptr<Segment> seg) {
        if (seg && seg.use_count() == 1 && free_list_.size() < max_pool_size_) {
            free_list_.push_back(std::move(seg));
        }
    }

    /// Create a recycler callback for BufferChain::SetRecycleCallback().
    BufferChain::RecycleCallback MakeRecycler() {
        return [this](std::shared_ptr<Segment> seg) {
            Release(std::move(seg));
        };
    }

    /// Number of segments in free list.
    size_t PoolSize() const noexcept { return free_list_.size(); }

    /// Set maximum free list size.
    void SetMaxPoolSize(size_t n) { max_pool_size_ = n; }

    /// Get shared pointer to the underlying memory resource.
    std::shared_ptr<std::pmr::memory_resource> GetResourcePtr() const {
        return resource_;
    }

    static constexpr size_t kDefaultMaxPoolSize = 32;

private:
    /// Custom allocator that captures shared ownership of the memory resource.
    /// Stored in shared_ptr's control block, extending resource lifetime.
    /// Templated to satisfy std::allocator_traits rebind requirements.
    template <typename T>
    struct PmrAllocatorImpl {
        using value_type = T;

        std::shared_ptr<std::pmr::memory_resource> resource;

        explicit PmrAllocatorImpl(std::shared_ptr<std::pmr::memory_resource> r)
            : resource(std::move(r)) {}

        template <typename U>
        PmrAllocatorImpl(const PmrAllocatorImpl<U>& other) noexcept
            : resource(other.resource) {}

        T* allocate(size_t n) {
            return static_cast<T*>(
                resource->allocate(n * sizeof(T), alignof(T)));
        }

        void deallocate(T* p, size_t n) noexcept {
            resource->deallocate(p, n * sizeof(T), alignof(T));
        }

        template <typename U>
        bool operator==(const PmrAllocatorImpl<U>& other) const noexcept {
            return resource == other.resource;
        }
    };

    using PmrAllocator = PmrAllocatorImpl<Segment>;

    std::shared_ptr<std::pmr::memory_resource> resource_;
    std::vector<std::shared_ptr<Segment>> free_list_;
    size_t max_pool_size_ = kDefaultMaxPoolSize;
};

}  // namespace dbn_pipe
