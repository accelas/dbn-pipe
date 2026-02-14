// SPDX-License-Identifier: MIT

#include "dbn_pipe/pg/byte_buffer.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe::pg {
namespace {

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
}  // namespace dbn_pipe::pg
