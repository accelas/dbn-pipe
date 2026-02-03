// SPDX-License-Identifier: MIT

#include "dbn_pipe/table/mbp1.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe {
namespace {

TEST(Mbp1TableTest, Name) { EXPECT_EQ(mbp1_table.name(), "mbp1"); }
TEST(Mbp1TableTest, ColumnCount) { EXPECT_EQ(mbp1_table.column_count(), 21); }
TEST(Mbp1TableTest, ColumnNames) {
    auto names = mbp1_table.column_names();
    EXPECT_EQ(names[0], "ts_event_ns");
    EXPECT_EQ(names[15], "bid_px");
    EXPECT_EQ(names[20], "ask_ct");
}
TEST(Mbp1TableTest, RowAccess) {
    using Row = decltype(mbp1_table)::RowType;
    Row row{};
    row.template get<"bid_px">() = 100000000;
    row.template get<"ask_px">() = 100010000;
    EXPECT_EQ(row.template get<"bid_px">(), 100000000);
    EXPECT_EQ(row.template get<"ask_px">(), 100010000);
}

TEST(Cmbp1TableTest, Name) { EXPECT_EQ(cmbp1_table.name(), "cmbp1"); }
TEST(Cmbp1TableTest, ColumnCount) { EXPECT_EQ(cmbp1_table.column_count(), 20); }
TEST(Cmbp1TableTest, ColumnNames) {
    auto names = cmbp1_table.column_names();
    EXPECT_EQ(names[5], "underlying_id");
    EXPECT_EQ(names[18], "bid_pb");
    EXPECT_EQ(names[19], "ask_pb");
}
TEST(Cmbp1TableTest, RowAccess) {
    using Row = decltype(cmbp1_table)::RowType;
    Row row{};
    row.template get<"underlying_id">() = 15144;
    row.template get<"bid_pb">() = 22;
    EXPECT_EQ(row.template get<"underlying_id">(), 15144);
    EXPECT_EQ(row.template get<"bid_pb">(), 22);
}

}  // namespace
}  // namespace dbn_pipe
