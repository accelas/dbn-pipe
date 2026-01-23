// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#include "dbwriter/mapper.hpp"
#include "dbwriter/table.hpp"
#include "dbwriter/pg_types.hpp"
#include "dbwriter/types.hpp"
#include <gtest/gtest.h>

namespace dbwriter {
namespace {

constexpr auto test_table = Table{"test",
    Column<"id", int64_t, pg::BigInt>{},
    Column<"value", int32_t, pg::Integer>{},
};

TEST(MapperTest, EncodeRow_WritesFieldCount) {
    auto mapper = make_mapper(test_table);

    typename decltype(test_table)::RowType row;
    row.get<"id">() = 1;
    row.get<"value">() = 100;

    ByteBuffer buf;
    mapper.encode_row(row, buf);

    auto view = buf.view();
    // First 2 bytes: field count (2) in big-endian
    EXPECT_EQ(view[0], std::byte{0x00});
    EXPECT_EQ(view[1], std::byte{0x02});
}

TEST(MapperTest, EncodeRow_WritesAllFields) {
    auto mapper = make_mapper(test_table);

    typename decltype(test_table)::RowType row;
    row.get<"id">() = 1;
    row.get<"value">() = 100;

    ByteBuffer buf;
    mapper.encode_row(row, buf);

    // 2 (count) + 4+8 (id) + 4+4 (value) = 22 bytes
    EXPECT_EQ(buf.size(), 22);
}

TEST(MapperTest, CopyHeader_WritesMagicAndFlags) {
    auto mapper = make_mapper(test_table);

    ByteBuffer buf;
    mapper.write_copy_header(buf);

    auto view = buf.view();
    // Magic: "PGCOPY\n\377\r\n\0" (11 bytes) + flags (4) + ext len (4) = 19 bytes
    EXPECT_EQ(view.size(), 19);
    EXPECT_EQ(view[0], std::byte{'P'});
    EXPECT_EQ(view[1], std::byte{'G'});
    EXPECT_EQ(view[5], std::byte{'Y'});
    EXPECT_EQ(view[6], std::byte{'\n'});
    EXPECT_EQ(view[7], std::byte{0xFF});  // \377
}

TEST(MapperTest, CopyTrailer_WritesMinusOne) {
    auto mapper = make_mapper(test_table);

    ByteBuffer buf;
    mapper.write_copy_trailer(buf);

    auto view = buf.view();
    ASSERT_EQ(view.size(), 2);
    // -1 as int16_t big-endian
    EXPECT_EQ(view[0], std::byte{0xFF});
    EXPECT_EQ(view[1], std::byte{0xFF});
}

}  // namespace
}  // namespace dbwriter
