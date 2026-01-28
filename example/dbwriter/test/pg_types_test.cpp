// SPDX-License-Identifier: MIT

#include "dbwriter/pg_types.hpp"
#include "dbwriter/types.hpp"
#include <gtest/gtest.h>

namespace dbwriter::pg {
namespace {

TEST(BigIntTest, Encode_WritesLengthAndValue) {
    ByteBuffer buf;
    BigInt::encode(0x0102030405060708LL, buf);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 12);  // 4 (len) + 8 (data)

    // Length = 8 in network byte order
    EXPECT_EQ(view[0], std::byte{0x00});
    EXPECT_EQ(view[1], std::byte{0x00});
    EXPECT_EQ(view[2], std::byte{0x00});
    EXPECT_EQ(view[3], std::byte{0x08});

    // Value in network byte order
    EXPECT_EQ(view[4], std::byte{0x01});
    EXPECT_EQ(view[11], std::byte{0x08});
}

TEST(IntegerTest, Encode_WritesLengthAndValue) {
    ByteBuffer buf;
    Integer::encode(0x01020304, buf);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 8);  // 4 (len) + 4 (data)

    // Length = 4
    EXPECT_EQ(view[3], std::byte{0x04});
    // Value
    EXPECT_EQ(view[4], std::byte{0x01});
}

TEST(SmallIntTest, Encode_WritesLengthAndValue) {
    ByteBuffer buf;
    SmallInt::encode(0x0102, buf);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 6);  // 4 (len) + 2 (data)

    // Length = 2
    EXPECT_EQ(view[3], std::byte{0x02});
}

TEST(CharTest, Encode_WritesSingleByte) {
    ByteBuffer buf;
    Char::encode('A', buf);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 5);  // 4 (len) + 1 (data)

    // Length = 1
    EXPECT_EQ(view[3], std::byte{0x01});
    // Value
    EXPECT_EQ(view[4], std::byte{'A'});
}

TEST(TimestamptzTest, Encode_WritesLegacyPgTimestamp) {
    ByteBuffer buf;
    Timestamp ts{123456789LL};  // microseconds since PG epoch
    Timestamptz::encode(ts, buf);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 12);  // 4 (len) + 8 (data)

    // Length = 8
    EXPECT_EQ(view[3], std::byte{0x08});
}

TEST(TimestamptzTest, Encode_WritesFromUnixNanoseconds) {
    ByteBuffer buf;
    // PG epoch in unix nanoseconds: 2000-01-01 00:00:00 UTC
    int64_t pg_epoch_ns = 946684800000000000LL;
    Timestamptz::encode(pg_epoch_ns, buf);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 12);  // 4 (len) + 8 (data)

    // Length = 8
    EXPECT_EQ(view[3], std::byte{0x08});

    // Value should be 0 (PG epoch maps to 0 microseconds since PG epoch)
    EXPECT_EQ(view[4], std::byte{0x00});
    EXPECT_EQ(view[5], std::byte{0x00});
    EXPECT_EQ(view[6], std::byte{0x00});
    EXPECT_EQ(view[7], std::byte{0x00});
    EXPECT_EQ(view[8], std::byte{0x00});
    EXPECT_EQ(view[9], std::byte{0x00});
    EXPECT_EQ(view[10], std::byte{0x00});
    EXPECT_EQ(view[11], std::byte{0x00});
}

TEST(NullTest, Encode_WritesMinusOne) {
    ByteBuffer buf;
    Null::encode(buf);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 4);

    // -1 in network byte order = 0xFFFFFFFF
    EXPECT_EQ(view[0], std::byte{0xFF});
    EXPECT_EQ(view[1], std::byte{0xFF});
    EXPECT_EQ(view[2], std::byte{0xFF});
    EXPECT_EQ(view[3], std::byte{0xFF});
}

}  // namespace
}  // namespace dbwriter::pg
