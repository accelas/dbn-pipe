// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <memory_resource>

#include "dbn_pipe/stream/segment_allocator.hpp"

using namespace dbn_pipe;

TEST(SegmentAllocatorTest, DefaultConstructUsesSync) {
    SegmentAllocator alloc;
    auto seg = alloc.Allocate();
    ASSERT_NE(seg, nullptr);
    EXPECT_EQ(seg->size, 0);
    // Segment data is 8-byte aligned
    EXPECT_EQ(reinterpret_cast<uintptr_t>(seg->data.data()) % 8, 0);
}

TEST(SegmentAllocatorTest, AllocateFromCustomResource) {
    auto resource = std::make_shared<std::pmr::unsynchronized_pool_resource>();
    SegmentAllocator alloc(resource);
    auto seg = alloc.Allocate();
    ASSERT_NE(seg, nullptr);
    EXPECT_EQ(seg->size, 0);
}

TEST(SegmentAllocatorTest, SegmentKeepsResourceAlive) {
    std::shared_ptr<Segment> seg;
    {
        auto resource = std::make_shared<std::pmr::unsynchronized_pool_resource>();
        std::weak_ptr<std::pmr::memory_resource> weak = resource;
        SegmentAllocator alloc(resource);
        seg = alloc.Allocate();
        resource.reset();  // Drop caller's ref
        alloc = {};        // Drop allocator's ref
        // Resource still alive via segment's control block
        EXPECT_FALSE(weak.expired());
    }
    // seg still valid --- resource freed when seg is destroyed
    EXPECT_EQ(seg->size, 0);
}

TEST(SegmentAllocatorTest, CopySharesResource) {
    SegmentAllocator alloc1;
    SegmentAllocator alloc2 = alloc1;
    auto seg1 = alloc1.Allocate();
    auto seg2 = alloc2.Allocate();
    ASSERT_NE(seg1, nullptr);
    ASSERT_NE(seg2, nullptr);
    // Both allocated --- no crash, resource is shared
}

TEST(SegmentAllocatorTest, MakeRecyclerReturnsToAllocator) {
    SegmentAllocator alloc;
    auto seg = alloc.Allocate();
    // Fill segment
    seg->size = Segment::kSize;

    BufferChain chain;
    chain.SetRecycleCallback(alloc.MakeRecycler());
    chain.Append(std::move(seg));

    // Consume triggers recycle --- segment returns to allocator's free list
    chain.Consume(Segment::kSize);
    EXPECT_EQ(alloc.PoolSize(), 1);

    // Re-acquire should reuse
    auto reused = alloc.Allocate();
    EXPECT_EQ(reused->size, 0);
    EXPECT_EQ(alloc.PoolSize(), 0);
}
