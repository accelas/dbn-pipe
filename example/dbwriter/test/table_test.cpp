// SPDX-License-Identifier: MIT

#include "dbwriter/table.hpp"
#include "dbwriter/pg_types.hpp"
#include "dbwriter/types.hpp"
#include <gtest/gtest.h>

namespace dbwriter {
namespace {

TEST(ColumnTest, HasNameAndTypes) {
    using Col = Column<"price", int64_t, pg::BigInt>;

    EXPECT_EQ(Col::name.view(), "price");
    static_assert(std::is_same_v<Col::cpp_type, int64_t>);
}

TEST(TableTest, HasNameAndColumns) {
    constexpr auto table = Table{"trades",
        Column<"id", int64_t, pg::BigInt>{},
        Column<"price", int64_t, pg::BigInt>{},
        Column<"size", int32_t, pg::Integer>{},
    };

    EXPECT_EQ(table.name(), "trades");
    EXPECT_EQ(table.column_count(), 3);
}

TEST(TableTest, ColumnNames) {
    constexpr auto table = Table{"test",
        Column<"a", int64_t, pg::BigInt>{},
        Column<"b", int32_t, pg::Integer>{},
    };

    auto names = table.column_names();
    ASSERT_EQ(names.size(), 2);
    EXPECT_EQ(names[0], "a");
    EXPECT_EQ(names[1], "b");
}

TEST(TableTest, RowType_HasNamedMembers) {
    constexpr auto table = Table{"test",
        Column<"id", int64_t, pg::BigInt>{},
        Column<"name", std::string_view, pg::Text>{},
    };

    using Row = decltype(table)::RowType;

    Row row{};
    row.template get<"id">() = 42;
    row.template get<"name">() = "hello";

    EXPECT_EQ(row.template get<"id">(), 42);
    EXPECT_EQ(row.template get<"name">(), "hello");
}

}  // namespace
}  // namespace dbwriter
