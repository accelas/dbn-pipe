#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "src/buffer_chain.hpp"

using namespace databento_async;

TEST(SegmentTest, BasicProperties) {
    Segment seg;
    EXPECT_EQ(seg.size, 0);
    EXPECT_EQ(seg.Remaining(), Segment::kSize);
    EXPECT_FALSE(seg.IsFull());

    seg.size = Segment::kSize;
    EXPECT_EQ(seg.Remaining(), 0);
    EXPECT_TRUE(seg.IsFull());
}

TEST(SegmentTest, Alignment) {
    Segment seg;
    // Verify 8-byte alignment for zero-copy record access
    EXPECT_EQ(reinterpret_cast<uintptr_t>(seg.data.data()) % 8, 0);
}

TEST(SegmentTest, WriteSpan) {
    Segment seg;
    auto span = seg.WriteSpan();
    EXPECT_EQ(span.size(), Segment::kSize);
    EXPECT_EQ(span.data(), seg.data.data());

    seg.size = 100;
    span = seg.WriteSpan();
    EXPECT_EQ(span.size(), Segment::kSize - 100);
    EXPECT_EQ(span.data(), seg.data.data() + 100);
}

TEST(BufferChainTest, EmptyChain) {
    BufferChain chain;
    EXPECT_EQ(chain.Size(), 0);
    EXPECT_TRUE(chain.Empty());
    EXPECT_EQ(chain.SegmentCount(), 0);
}

TEST(BufferChainTest, AppendSegment) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    std::memset(seg->data.data(), 0xAB, 100);
    seg->size = 100;

    chain.Append(seg);

    EXPECT_EQ(chain.Size(), 100);
    EXPECT_FALSE(chain.Empty());
    EXPECT_EQ(chain.SegmentCount(), 1);
}

TEST(BufferChainTest, AppendEmptySegment) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    seg->size = 0;  // Empty segment

    chain.Append(seg);

    // Empty segments should be ignored
    EXPECT_EQ(chain.Size(), 0);
    EXPECT_TRUE(chain.Empty());
    EXPECT_EQ(chain.SegmentCount(), 0);
}

TEST(BufferChainTest, AppendMultipleSegments) {
    BufferChain chain;

    auto seg1 = std::make_shared<Segment>();
    seg1->size = 50;
    chain.Append(seg1);

    auto seg2 = std::make_shared<Segment>();
    seg2->size = 75;
    chain.Append(seg2);

    EXPECT_EQ(chain.Size(), 125);
    EXPECT_EQ(chain.SegmentCount(), 2);
}

TEST(BufferChainTest, ConsumePartial) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    seg->size = 100;
    chain.Append(seg);

    chain.Consume(30);

    EXPECT_EQ(chain.Size(), 70);
    EXPECT_EQ(chain.SegmentCount(), 1);  // Still have the segment
}

TEST(BufferChainTest, ConsumeEntireSegment) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    seg->size = 100;
    chain.Append(seg);

    chain.Consume(100);

    EXPECT_EQ(chain.Size(), 0);
    EXPECT_TRUE(chain.Empty());
    EXPECT_EQ(chain.SegmentCount(), 0);  // Segment removed
}

TEST(BufferChainTest, ConsumeAcrossSegments) {
    BufferChain chain;

    auto seg1 = std::make_shared<Segment>();
    seg1->size = 50;
    chain.Append(seg1);

    auto seg2 = std::make_shared<Segment>();
    seg2->size = 75;
    chain.Append(seg2);

    // Consume 60 bytes - removes seg1, consumes 10 from seg2
    chain.Consume(60);

    EXPECT_EQ(chain.Size(), 65);
    EXPECT_EQ(chain.SegmentCount(), 1);  // Only seg2 remains
}

TEST(BufferChainTest, IsContiguousSingleSegment) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    seg->size = 100;
    chain.Append(seg);

    EXPECT_TRUE(chain.IsContiguous(0, 50));
    EXPECT_TRUE(chain.IsContiguous(0, 100));
    EXPECT_TRUE(chain.IsContiguous(50, 50));
    EXPECT_FALSE(chain.IsContiguous(0, 101));  // Beyond segment
    EXPECT_FALSE(chain.IsContiguous(50, 51));  // Beyond segment
}

TEST(BufferChainTest, IsContiguousAfterPartialConsume) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    seg->size = 100;
    chain.Append(seg);

    chain.Consume(30);

    // Now have 70 bytes remaining
    EXPECT_TRUE(chain.IsContiguous(0, 70));
    EXPECT_TRUE(chain.IsContiguous(0, 50));
    EXPECT_FALSE(chain.IsContiguous(0, 71));
}

TEST(BufferChainTest, IsContiguousMultipleSegments) {
    BufferChain chain;

    auto seg1 = std::make_shared<Segment>();
    seg1->size = 50;
    chain.Append(seg1);

    auto seg2 = std::make_shared<Segment>();
    seg2->size = 75;
    chain.Append(seg2);

    // Data in seg1 only
    EXPECT_TRUE(chain.IsContiguous(0, 50));
    EXPECT_TRUE(chain.IsContiguous(0, 30));

    // Spans both segments - not contiguous
    EXPECT_FALSE(chain.IsContiguous(0, 60));
    EXPECT_FALSE(chain.IsContiguous(40, 20));

    // Data in seg2 only (offset >= 50)
    EXPECT_TRUE(chain.IsContiguous(50, 75));
    EXPECT_TRUE(chain.IsContiguous(50, 50));
}

TEST(BufferChainTest, DataAt) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    std::memset(seg->data.data(), 0x00, Segment::kSize);
    seg->data[0] = std::byte{0xAA};
    seg->data[50] = std::byte{0xBB};
    seg->size = 100;
    chain.Append(seg);

    EXPECT_EQ(*chain.DataAt(0), std::byte{0xAA});
    EXPECT_EQ(*chain.DataAt(50), std::byte{0xBB});
}

TEST(BufferChainTest, DataAtAfterConsume) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    for (size_t i = 0; i < 100; ++i) {
        seg->data[i] = std::byte{static_cast<uint8_t>(i)};
    }
    seg->size = 100;
    chain.Append(seg);

    chain.Consume(30);

    // DataAt(0) should now point to byte 30 of original data
    EXPECT_EQ(*chain.DataAt(0), std::byte{30});
    EXPECT_EQ(*chain.DataAt(10), std::byte{40});
}

TEST(BufferChainTest, DataAtAcrossSegments) {
    BufferChain chain;

    auto seg1 = std::make_shared<Segment>();
    seg1->data[25] = std::byte{0xAA};
    seg1->size = 50;
    chain.Append(seg1);

    auto seg2 = std::make_shared<Segment>();
    seg2->data[0] = std::byte{0xBB};
    seg2->data[25] = std::byte{0xCC};
    seg2->size = 75;
    chain.Append(seg2);

    EXPECT_EQ(*chain.DataAt(25), std::byte{0xAA});  // In seg1
    EXPECT_EQ(*chain.DataAt(50), std::byte{0xBB});  // Start of seg2
    EXPECT_EQ(*chain.DataAt(75), std::byte{0xCC});  // In seg2
}

TEST(BufferChainTest, GetSegmentAt) {
    BufferChain chain;

    auto seg1 = std::make_shared<Segment>();
    seg1->size = 50;
    chain.Append(seg1);

    auto seg2 = std::make_shared<Segment>();
    seg2->size = 75;
    chain.Append(seg2);

    EXPECT_EQ(chain.GetSegmentAt(0), seg1);
    EXPECT_EQ(chain.GetSegmentAt(49), seg1);
    EXPECT_EQ(chain.GetSegmentAt(50), seg2);
    EXPECT_EQ(chain.GetSegmentAt(100), seg2);
}

TEST(BufferChainTest, CopyToContiguous) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    for (size_t i = 0; i < 100; ++i) {
        seg->data[i] = std::byte{static_cast<uint8_t>(i)};
    }
    seg->size = 100;
    chain.Append(seg);

    std::array<std::byte, 50> dest{};
    chain.CopyTo(10, 50, dest.data());

    for (size_t i = 0; i < 50; ++i) {
        EXPECT_EQ(dest[i], std::byte{static_cast<uint8_t>(i + 10)});
    }
}

TEST(BufferChainTest, CopyToAcrossSegments) {
    BufferChain chain;

    auto seg1 = std::make_shared<Segment>();
    for (size_t i = 0; i < 50; ++i) {
        seg1->data[i] = std::byte{static_cast<uint8_t>(i)};
    }
    seg1->size = 50;
    chain.Append(seg1);

    auto seg2 = std::make_shared<Segment>();
    for (size_t i = 0; i < 75; ++i) {
        seg2->data[i] = std::byte{static_cast<uint8_t>(50 + i)};
    }
    seg2->size = 75;
    chain.Append(seg2);

    // Copy 40 bytes starting at offset 30 (spans both segments)
    std::array<std::byte, 40> dest{};
    chain.CopyTo(30, 40, dest.data());

    for (size_t i = 0; i < 40; ++i) {
        EXPECT_EQ(dest[i], std::byte{static_cast<uint8_t>(30 + i)});
    }
}

TEST(BufferChainTest, CopyToAfterConsume) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    for (size_t i = 0; i < 100; ++i) {
        seg->data[i] = std::byte{static_cast<uint8_t>(i)};
    }
    seg->size = 100;
    chain.Append(seg);

    chain.Consume(20);

    std::array<std::byte, 30> dest{};
    chain.CopyTo(0, 30, dest.data());

    // Should start from byte 20 of original data
    for (size_t i = 0; i < 30; ++i) {
        EXPECT_EQ(dest[i], std::byte{static_cast<uint8_t>(20 + i)});
    }
}

TEST(BufferChainTest, Clear) {
    BufferChain chain;

    auto seg1 = std::make_shared<Segment>();
    seg1->size = 50;
    chain.Append(seg1);

    auto seg2 = std::make_shared<Segment>();
    seg2->size = 75;
    chain.Append(seg2);

    chain.Consume(30);

    chain.Clear();

    EXPECT_EQ(chain.Size(), 0);
    EXPECT_TRUE(chain.Empty());
    EXPECT_EQ(chain.SegmentCount(), 0);
}

TEST(BufferChainTest, AlignmentPreserved) {
    // Verify that DataAt returns 8-byte aligned pointers for aligned offsets
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    seg->size = 1000;
    chain.Append(seg);

    // Segment data is 8-byte aligned, so offsets that are multiples of 8
    // should give aligned pointers
    for (size_t offset = 0; offset < 100; offset += 8) {
        const std::byte* ptr = chain.DataAt(offset);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 8, 0)
            << "Offset " << offset << " not aligned";
    }
}

// SegmentPool tests

TEST(SegmentPoolTest, AcquireNewSegment) {
    SegmentPool pool;
    EXPECT_EQ(pool.PoolSize(), 0);

    auto seg = pool.Acquire();
    EXPECT_NE(seg, nullptr);
    EXPECT_EQ(seg->size, 0);
    EXPECT_EQ(pool.PoolSize(), 0);  // Pool still empty, we created new
}

TEST(SegmentPoolTest, AcquireFromPool) {
    SegmentPool pool;

    auto seg1 = pool.Acquire();
    seg1->size = 100;  // Mark that we used it
    pool.Release(seg1);
    EXPECT_EQ(pool.PoolSize(), 1);

    auto seg2 = pool.Acquire();
    EXPECT_EQ(seg2, seg1);  // Same segment reused
    EXPECT_EQ(seg2->size, 0);  // Size reset on acquire
    EXPECT_EQ(pool.PoolSize(), 0);  // Pool empty after acquire
}

TEST(SegmentPoolTest, ReleaseToPool) {
    SegmentPool pool;

    auto seg = pool.Acquire();
    pool.Release(seg);

    EXPECT_EQ(pool.PoolSize(), 1);
}

TEST(SegmentPoolTest, ReleaseNullptr) {
    SegmentPool pool;
    pool.Release(nullptr);
    EXPECT_EQ(pool.PoolSize(), 0);  // Nothing added
}

TEST(SegmentPoolTest, PoolSizeLimit) {
    SegmentPool pool(3);  // Max 3 segments
    EXPECT_EQ(pool.MaxPoolSize(), 3);

    std::vector<std::shared_ptr<Segment>> segments;
    for (int i = 0; i < 5; ++i) {
        segments.push_back(pool.Acquire());
    }

    // Release all 5
    for (auto& seg : segments) {
        pool.Release(seg);
    }

    // Only 3 should be in pool
    EXPECT_EQ(pool.PoolSize(), 3);
}

TEST(SegmentPoolTest, Clear) {
    SegmentPool pool;

    // Acquire multiple segments, then release all
    std::vector<std::shared_ptr<Segment>> segs;
    for (int i = 0; i < 5; ++i) {
        segs.push_back(pool.Acquire());
    }
    for (auto& seg : segs) {
        pool.Release(seg);
    }
    EXPECT_EQ(pool.PoolSize(), 5);

    pool.Clear();
    EXPECT_EQ(pool.PoolSize(), 0);
}

TEST(SegmentPoolTest, AcquiredSegmentsAreAligned) {
    SegmentPool pool;

    for (int i = 0; i < 10; ++i) {
        auto seg = pool.Acquire();
        EXPECT_EQ(reinterpret_cast<uintptr_t>(seg->data.data()) % 8, 0);
        pool.Release(seg);
    }
}

TEST(SegmentPoolTest, MakeRecycler) {
    SegmentPool pool;
    BufferChain chain;
    chain.SetRecycleCallback(pool.MakeRecycler());

    // Acquire and fill a segment
    auto seg = pool.Acquire();
    seg->size = 100;
    chain.Append(seg);
    EXPECT_EQ(pool.PoolSize(), 0);  // Segment is in chain, not pool

    // Consume fully - segment should be recycled back to pool
    chain.Consume(100);
    EXPECT_EQ(pool.PoolSize(), 1);
    EXPECT_EQ(chain.Size(), 0);
}

TEST(SegmentPoolTest, PartialConsumeDoesNotRecycle) {
    SegmentPool pool;
    BufferChain chain;
    chain.SetRecycleCallback(pool.MakeRecycler());

    // Acquire and fill a segment
    auto seg = pool.Acquire();
    seg->size = 100;
    chain.Append(seg);
    EXPECT_EQ(pool.PoolSize(), 0);

    // Partial consume - segment should NOT be recycled
    chain.Consume(50);
    EXPECT_EQ(pool.PoolSize(), 0);  // Still not recycled
    EXPECT_EQ(chain.Size(), 50);    // 50 bytes remaining

    // Consume remaining - NOW it should recycle
    chain.Consume(50);
    EXPECT_EQ(pool.PoolSize(), 1);  // Recycled
    EXPECT_EQ(chain.Size(), 0);
}

// Edge case tests

TEST(BufferChainTest, ConsumeZero) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    seg->size = 100;
    chain.Append(seg);

    // Consume(0) should be a no-op
    chain.Consume(0);
    EXPECT_EQ(chain.Size(), 100);
    EXPECT_EQ(chain.SegmentCount(), 1);
}

TEST(BufferChainTest, CopyToZeroLength) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    seg->size = 100;
    chain.Append(seg);

    // CopyTo with len == 0 should be a no-op (no crash)
    std::byte dest[1];
    chain.CopyTo(0, 0, dest);  // Should not crash
    chain.CopyTo(50, 0, dest); // Should not crash
}

TEST(BufferChainTest, IsContiguousZeroLength) {
    BufferChain chain;

    // Empty chain - even len == 0 should return false
    EXPECT_FALSE(chain.IsContiguous(0, 0));

    auto seg = std::make_shared<Segment>();
    seg->size = 100;
    chain.Append(seg);

    // len == 0 with valid offset should return true
    EXPECT_TRUE(chain.IsContiguous(0, 0));
    EXPECT_TRUE(chain.IsContiguous(50, 0));
    EXPECT_TRUE(chain.IsContiguous(100, 0));  // At boundary

    // len == 0 with invalid offset should return false
    EXPECT_FALSE(chain.IsContiguous(101, 0));
}

TEST(BufferChainTest, DataAtSegmentBoundary) {
    BufferChain chain;

    auto seg1 = std::make_shared<Segment>();
    seg1->data[49] = std::byte{0xAA};
    seg1->size = 50;
    chain.Append(seg1);

    auto seg2 = std::make_shared<Segment>();
    seg2->data[0] = std::byte{0xBB};
    seg2->size = 50;
    chain.Append(seg2);

    // Last byte of first segment
    EXPECT_EQ(*chain.DataAt(49), std::byte{0xAA});
    // First byte of second segment (exactly at boundary)
    EXPECT_EQ(*chain.DataAt(50), std::byte{0xBB});
}

TEST(BufferChainTest, MultipleConsecutiveConsumes) {
    BufferChain chain;

    auto seg = std::make_shared<Segment>();
    for (size_t i = 0; i < 100; ++i) {
        seg->data[i] = std::byte{static_cast<uint8_t>(i)};
    }
    seg->size = 100;
    chain.Append(seg);

    // Multiple small consumes
    chain.Consume(10);
    EXPECT_EQ(chain.Size(), 90);
    EXPECT_EQ(*chain.DataAt(0), std::byte{10});

    chain.Consume(20);
    EXPECT_EQ(chain.Size(), 70);
    EXPECT_EQ(*chain.DataAt(0), std::byte{30});

    chain.Consume(30);
    EXPECT_EQ(chain.Size(), 40);
    EXPECT_EQ(*chain.DataAt(0), std::byte{60});

    chain.Consume(40);
    EXPECT_EQ(chain.Size(), 0);
    EXPECT_TRUE(chain.Empty());
}

TEST(BufferChainTest, RecycleCallbackOnClear) {
    SegmentPool pool;
    BufferChain chain;
    chain.SetRecycleCallback(pool.MakeRecycler());

    // Add multiple segments
    for (int i = 0; i < 5; ++i) {
        auto seg = pool.Acquire();
        seg->size = 100;
        chain.Append(seg);
    }
    EXPECT_EQ(chain.SegmentCount(), 5);
    EXPECT_EQ(pool.PoolSize(), 0);

    // Clear should recycle all segments
    chain.Clear();
    EXPECT_EQ(chain.Size(), 0);
    EXPECT_EQ(chain.SegmentCount(), 0);
    EXPECT_EQ(pool.PoolSize(), 5);
}

TEST(BufferChainTest, SizeIsCached) {
    BufferChain chain;

    // Size should be 0 initially
    EXPECT_EQ(chain.Size(), 0);

    auto seg1 = std::make_shared<Segment>();
    seg1->size = 100;
    chain.Append(seg1);
    EXPECT_EQ(chain.Size(), 100);

    auto seg2 = std::make_shared<Segment>();
    seg2->size = 200;
    chain.Append(seg2);
    EXPECT_EQ(chain.Size(), 300);

    chain.Consume(50);
    EXPECT_EQ(chain.Size(), 250);

    chain.Consume(50);  // Finishes first segment
    EXPECT_EQ(chain.Size(), 200);

    chain.Clear();
    EXPECT_EQ(chain.Size(), 0);
}
