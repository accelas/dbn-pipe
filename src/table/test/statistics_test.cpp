// SPDX-License-Identifier: MIT

#include "src/table/statistics.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe {
namespace {

TEST(StatisticsTableTest, Name) { EXPECT_EQ(statistics_table.name(), "statistics"); }
TEST(StatisticsTableTest, ColumnCount) { EXPECT_EQ(statistics_table.column_count(), 14); }
TEST(StatisticsTableTest, ColumnNames) {
    auto names = statistics_table.column_names();
    EXPECT_EQ(names[0], "ts_event_ns");
    EXPECT_EQ(names[5], "stat_type");
    EXPECT_EQ(names[13], "stat_flags");
}
TEST(StatisticsTableTest, RowAccess) {
    using Row = decltype(statistics_table)::RowType;
    Row row{};
    row.template get<"stat_type">() = 1;
    row.template get<"price">() = 100000000;
    EXPECT_EQ(row.template get<"stat_type">(), 1);
    EXPECT_EQ(row.template get<"price">(), 100000000);
}

TEST(OptionsStatisticsTableTest, Name) { EXPECT_EQ(options_statistics_table.name(), "statistics"); }
TEST(OptionsStatisticsTableTest, ColumnCount) { EXPECT_EQ(options_statistics_table.column_count(), 15); }
TEST(OptionsStatisticsTableTest, HasUnderlyingId) {
    auto names = options_statistics_table.column_names();
    EXPECT_EQ(names[5], "underlying_id");
    EXPECT_EQ(names[6], "stat_type");
}
TEST(OptionsStatisticsTableTest, RowAccess) {
    using Row = decltype(options_statistics_table)::RowType;
    Row row{};
    row.template get<"underlying_id">() = 15144;
    row.template get<"stat_type">() = 2;
    EXPECT_EQ(row.template get<"underlying_id">(), 15144);
    EXPECT_EQ(row.template get<"stat_type">(), 2);
}

}  // namespace
}  // namespace dbn_pipe
