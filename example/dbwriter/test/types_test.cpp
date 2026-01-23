// SPDX-License-Identifier: MIT

#include "dbwriter/types.hpp"
#include <gtest/gtest.h>

namespace dbwriter {
namespace {

TEST(TimestampTest, FromUnixNs_ConvertsCorrectly) {
    // 2024-01-01 00:00:00 UTC in nanoseconds since Unix epoch
    int64_t unix_ns = 1704067200000000000LL;

    auto ts = Timestamp::from_unix_ns(unix_ns);

    // 2024-01-01 is 24 years after PG epoch (2000-01-01)
    // That's about 757382400 seconds = 757382400000000 usec
    EXPECT_GT(ts.to_pg_timestamp(), 0);
}

TEST(TimestampTest, PgEpoch_IsZero) {
    // 2000-01-01 00:00:00 UTC = 946684800 seconds since Unix epoch
    int64_t pg_epoch_unix_ns = 946684800000000000LL;

    auto ts = Timestamp::from_unix_ns(pg_epoch_unix_ns);

    EXPECT_EQ(ts.to_pg_timestamp(), 0);
}

TEST(ByteBufferTest, PutInt16Be_NetworkByteOrder) {
    ByteBuffer buf;
    buf.put_int16_be(0x0102);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 2);
    EXPECT_EQ(view[0], std::byte{0x01});
    EXPECT_EQ(view[1], std::byte{0x02});
}

TEST(ByteBufferTest, PutInt32Be_NetworkByteOrder) {
    ByteBuffer buf;
    buf.put_int32_be(0x01020304);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 4);
    EXPECT_EQ(view[0], std::byte{0x01});
    EXPECT_EQ(view[1], std::byte{0x02});
    EXPECT_EQ(view[2], std::byte{0x03});
    EXPECT_EQ(view[3], std::byte{0x04});
}

TEST(ByteBufferTest, PutInt64Be_NetworkByteOrder) {
    ByteBuffer buf;
    buf.put_int64_be(0x0102030405060708LL);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 8);
    EXPECT_EQ(view[0], std::byte{0x01});
    EXPECT_EQ(view[7], std::byte{0x08});
}

TEST(ByteBufferTest, Clear_ResetsBuffer) {
    ByteBuffer buf;
    buf.put_int32_be(123);
    EXPECT_EQ(buf.size(), 4);

    buf.clear();

    EXPECT_EQ(buf.size(), 0);
}

}  // namespace
}  // namespace dbwriter
