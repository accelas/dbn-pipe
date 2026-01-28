// SPDX-License-Identifier: MIT

#include "src/table/status.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe {
namespace {

TEST(StatusTableTest, Name) { EXPECT_EQ(status_table.name(), "status"); }
TEST(StatusTableTest, ColumnCount) { EXPECT_EQ(status_table.column_count(), 13); }
TEST(StatusTableTest, ColumnNames) {
    auto names = status_table.column_names();
    EXPECT_EQ(names[0], "ts_event_ns");
    EXPECT_EQ(names[5], "action");
    EXPECT_EQ(names[8], "is_trading");
    EXPECT_EQ(names[10], "is_short_sell_restricted");
    EXPECT_EQ(names[12], "ts_recv");
}
TEST(StatusTableTest, RowAccess) {
    using Row = decltype(status_table)::RowType;
    Row row{};
    row.template get<"is_trading">() = 'Y';
    row.template get<"action">() = 1;
    EXPECT_EQ(row.template get<"is_trading">(), 'Y');
    EXPECT_EQ(row.template get<"action">(), 1);
}

TEST(OptionsStatusTableTest, Name) { EXPECT_EQ(options_status_table.name(), "status"); }
TEST(OptionsStatusTableTest, ColumnCount) { EXPECT_EQ(options_status_table.column_count(), 14); }
TEST(OptionsStatusTableTest, HasUnderlyingId) {
    auto names = options_status_table.column_names();
    EXPECT_EQ(names[5], "underlying_id");
    EXPECT_EQ(names[6], "action");
}
TEST(OptionsStatusTableTest, RowAccess) {
    using Row = decltype(options_status_table)::RowType;
    Row row{};
    row.template get<"underlying_id">() = 15144;
    row.template get<"is_quoting">() = 'N';
    EXPECT_EQ(row.template get<"underlying_id">(), 15144);
    EXPECT_EQ(row.template get<"is_quoting">(), 'N');
}

}  // namespace
}  // namespace dbn_pipe
