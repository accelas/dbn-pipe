// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <memory_resource>
#include <string>

#include "dbn_pipe/stream/segment_allocator.hpp"
#include "dbn_pipe/stream/buffer_chain.hpp"

using namespace dbn_pipe;

// Memory resource that counts allocations and delegates to new_delete_resource
class CountingResource : public std::pmr::memory_resource {
public:
    std::atomic<size_t> allocations{0};
    std::atomic<size_t> deallocations{0};
    std::atomic<size_t> bytes_allocated{0};

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        ++allocations;
        bytes_allocated += bytes;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }
};

TEST(PmrIntegrationTest, SegmentAllocationsGoThroughCustomResource) {
    auto counting = std::make_shared<CountingResource>();
    SegmentAllocator alloc(counting);

    // Allocate several segments
    auto seg1 = alloc.Allocate();
    auto seg2 = alloc.Allocate();
    auto seg3 = alloc.Allocate();

    EXPECT_GT(counting->allocations.load(), 0);
    EXPECT_GT(counting->bytes_allocated.load(), 0);

    // Use segments in a BufferChain
    seg1->size = 100;
    seg2->size = 200;

    BufferChain chain;
    chain.SetRecycleCallback(alloc.MakeRecycler());
    chain.Append(std::move(seg1));
    chain.Append(std::move(seg2));

    // Consume triggers recycle
    chain.Consume(100);
    EXPECT_EQ(alloc.PoolSize(), 1);  // First segment recycled

    // Allocate reuses from pool (no new PMR allocation)
    size_t allocs_before = counting->allocations.load();
    auto reused = alloc.Allocate();
    EXPECT_EQ(counting->allocations.load(), allocs_before);  // No new allocation
}

TEST(PmrIntegrationTest, ResourceOutlivesSegments) {
    auto counting = std::make_shared<CountingResource>();
    std::shared_ptr<Segment> seg;

    {
        SegmentAllocator alloc(counting);
        seg = alloc.Allocate();
        // alloc destroyed here, but resource stays alive via seg's control block
    }

    EXPECT_EQ(seg->size, 0);  // Still valid

    // Drop the segment --- deallocation goes through the resource
    size_t deallocs_before = counting->deallocations.load();
    seg.reset();
    EXPECT_GT(counting->deallocations.load(), deallocs_before);
}

TEST(PmrIntegrationTest, AppendBytesUsesAllocator) {
    auto counting = std::make_shared<CountingResource>();
    SegmentAllocator alloc(counting);

    BufferChain chain;
    std::string data(1000, 'x');
    chain.AppendBytes(data.data(), data.size(), alloc);

    EXPECT_GT(counting->allocations.load(), 0);
    EXPECT_EQ(chain.Size(), 1000);
}
