// SPDX-License-Identifier: MIT

#include "dbn_pipe/table/table.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe {
namespace {

TEST(ColumnTest, HasNameAndTypes) {
    using Col = Column<"price", Int64>;

    EXPECT_EQ(Col::name.view(), "price");
    static_assert(std::is_same_v<Col::cpp_type, int64_t>);
}

TEST(TableTest, HasNameAndColumns) {
    constexpr auto table = Table{"trades",
        Column<"id", Int64>{},
        Column<"price", Int64>{},
        Column<"size", Int32>{},
    };

    EXPECT_EQ(table.name(), "trades");
    EXPECT_EQ(table.column_count(), 3);
}

TEST(TableTest, ColumnNames) {
    constexpr auto table = Table{"test",
        Column<"a", Int64>{},
        Column<"b", Int32>{},
    };

    auto names = table.column_names();
    ASSERT_EQ(names.size(), 2);
    EXPECT_EQ(names[0], "a");
    EXPECT_EQ(names[1], "b");
}

TEST(TableTest, RowType_HasNamedMembers) {
    constexpr auto table = Table{"test",
        Column<"id", Int64>{},
        Column<"name", Text>{},
    };

    using Row = decltype(table)::RowType;

    Row row{};
    row.template get<"id">() = 42;
    row.template get<"name">() = "hello";

    EXPECT_EQ(row.template get<"id">(), 42);
    EXPECT_EQ(row.template get<"name">(), "hello");
}

}  // namespace
}  // namespace dbn_pipe
