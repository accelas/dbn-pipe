// SPDX-License-Identifier: MIT

#include "dbn_pipe/table/ohlcv.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe {
namespace {

TEST(OhlcvTableTest, Names) {
    EXPECT_EQ(ohlcv_1s_table.name(), "ohlcv_1s");
    EXPECT_EQ(ohlcv_1m_table.name(), "ohlcv_1m");
    EXPECT_EQ(ohlcv_1h_table.name(), "ohlcv_1h");
    EXPECT_EQ(ohlcv_1d_table.name(), "ohlcv_1d");
}
TEST(OhlcvTableTest, ColumnCount) {
    EXPECT_EQ(ohlcv_1s_table.column_count(), 9);
    EXPECT_EQ(ohlcv_1m_table.column_count(), 9);
    EXPECT_EQ(ohlcv_1h_table.column_count(), 9);
    EXPECT_EQ(ohlcv_1d_table.column_count(), 9);
}
TEST(OhlcvTableTest, ColumnNames) {
    auto names = ohlcv_1d_table.column_names();
    EXPECT_EQ(names[0], "ts_event_ns");
    EXPECT_EQ(names[4], "open");
    EXPECT_EQ(names[8], "volume");
}
TEST(OhlcvTableTest, RowAccess) {
    using Row = decltype(ohlcv_1d_table)::RowType;
    Row row{};
    row.template get<"open">() = 100000000;
    row.template get<"close">() = 101000000;
    row.template get<"volume">() = 50000000;
    EXPECT_EQ(row.template get<"open">(), 100000000);
    EXPECT_EQ(row.template get<"close">(), 101000000);
    EXPECT_EQ(row.template get<"volume">(), 50000000);
}

TEST(OptionsOhlcvTableTest, Names) {
    EXPECT_EQ(options_ohlcv_1s_table.name(), "options_ohlcv_1s");
    EXPECT_EQ(options_ohlcv_1m_table.name(), "options_ohlcv_1m");
    EXPECT_EQ(options_ohlcv_1h_table.name(), "options_ohlcv_1h");
    EXPECT_EQ(options_ohlcv_1d_table.name(), "options_ohlcv_1d");
}
TEST(OptionsOhlcvTableTest, ColumnCount) {
    EXPECT_EQ(options_ohlcv_1s_table.column_count(), 10);
    EXPECT_EQ(options_ohlcv_1m_table.column_count(), 10);
    EXPECT_EQ(options_ohlcv_1h_table.column_count(), 10);
    EXPECT_EQ(options_ohlcv_1d_table.column_count(), 10);
}
TEST(OptionsOhlcvTableTest, HasUnderlyingId) {
    auto names = options_ohlcv_1d_table.column_names();
    EXPECT_EQ(names[4], "underlying_id");
}
TEST(OptionsOhlcvTableTest, RowAccess) {
    using Row = decltype(options_ohlcv_1d_table)::RowType;
    Row row{};
    row.template get<"underlying_id">() = 15144;
    row.template get<"close">() = 5000000;
    EXPECT_EQ(row.template get<"underlying_id">(), 15144);
    EXPECT_EQ(row.template get<"close">(), 5000000);
}

}  // namespace
}  // namespace dbn_pipe
