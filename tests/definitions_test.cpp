// SPDX-License-Identifier: MIT

#include "dbn_pipe/table/definitions.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe {
namespace {

TEST(DefinitionsTableTest, Name) { EXPECT_EQ(definitions_table.name(), "definitions"); }
TEST(DefinitionsTableTest, ColumnCount) { EXPECT_EQ(definitions_table.column_count(), 43); }
TEST(DefinitionsTableTest, ColumnNames) {
    auto names = definitions_table.column_names();
    EXPECT_EQ(names[0], "ts_event_ns");
    EXPECT_EQ(names[5], "raw_symbol");
    EXPECT_EQ(names[42], "ts_recv");
}
TEST(DefinitionsTableTest, RowAccess) {
    using Row = decltype(definitions_table)::RowType;
    Row row{};
    row.template get<"raw_symbol">() = "SPY";
    row.template get<"instrument_id">() = 15144;
    EXPECT_EQ(row.template get<"raw_symbol">(), "SPY");
    EXPECT_EQ(row.template get<"instrument_id">(), 15144);
}

TEST(OptionsDefinitionsTableTest, Name) { EXPECT_EQ(options_definitions_table.name(), "options_definitions"); }
TEST(OptionsDefinitionsTableTest, ColumnCount) { EXPECT_EQ(options_definitions_table.column_count(), 53); }
TEST(OptionsDefinitionsTableTest, OptionsSpecificColumns) {
    auto names = options_definitions_table.column_names();
    EXPECT_EQ(names[9], "underlying_id");
    EXPECT_EQ(names[10], "underlying");
    EXPECT_EQ(names[11], "strike_price");
    EXPECT_EQ(names[12], "instrument_class");
    EXPECT_EQ(names[13], "expiration_ns");
    EXPECT_EQ(names[14], "expiration");
}
TEST(OptionsDefinitionsTableTest, RowAccess) {
    using Row = decltype(options_definitions_table)::RowType;
    Row row{};
    row.template get<"strike_price">() = 486000000000LL;
    row.template get<"instrument_class">() = 'C';
    row.template get<"underlying">() = "SPY";
    EXPECT_EQ(row.template get<"strike_price">(), 486000000000LL);
    EXPECT_EQ(row.template get<"instrument_class">(), 'C');
    EXPECT_EQ(row.template get<"underlying">(), "SPY");
}

}  // namespace
}  // namespace dbn_pipe
