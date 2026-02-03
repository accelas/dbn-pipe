// SPDX-License-Identifier: MIT

#include "dbn_pipe/table/trades.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe {
namespace {

TEST(TradesTableTest, Name) {
    EXPECT_EQ(trades_table.name(), "trades");
}

TEST(TradesTableTest, ColumnCount) {
    EXPECT_EQ(trades_table.column_count(), 16);
}

TEST(TradesTableTest, ColumnNames) {
    auto names = trades_table.column_names();
    EXPECT_EQ(names[0], "ts_event_ns");
    EXPECT_EQ(names[1], "ts_event");
    EXPECT_EQ(names[4], "instrument_id");
    EXPECT_EQ(names[9], "price");
    EXPECT_EQ(names[15], "sequence");
}

TEST(TradesTableTest, RowAccess) {
    using Row = decltype(trades_table)::RowType;
    Row row{};
    row.template get<"price">() = 1234500000;
    row.template get<"instrument_id">() = 15144;
    row.template get<"action">() = 'T';
    EXPECT_EQ(row.template get<"price">(), 1234500000);
    EXPECT_EQ(row.template get<"instrument_id">(), 15144);
    EXPECT_EQ(row.template get<"action">(), 'T');
}

}  // namespace
}  // namespace dbn_pipe
