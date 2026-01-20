// tests/metadata_client_test.cpp
#include <gtest/gtest.h>

#include "src/api/metadata_client.hpp"

namespace dbn_pipe {
namespace {

TEST(RecordCountBuilderTest, ParsesInteger) {
    RecordCountBuilder builder;
    builder.OnUint(12345);

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 12345);
}

TEST(RecordCountBuilderTest, ParsesSignedInteger) {
    RecordCountBuilder builder;
    builder.OnInt(12345);

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 12345);
}

TEST(RecordCountBuilderTest, FailsWhenEmpty) {
    RecordCountBuilder builder;
    auto result = builder.Build();
    ASSERT_FALSE(result.has_value());
}

TEST(BillableSizeBuilderTest, ParsesInteger) {
    BillableSizeBuilder builder;
    builder.OnUint(1048576);

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1048576);
}

TEST(BillableSizeBuilderTest, ParsesSignedInteger) {
    BillableSizeBuilder builder;
    builder.OnInt(1048576);

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1048576);
}

TEST(BillableSizeBuilderTest, FailsWhenEmpty) {
    BillableSizeBuilder builder;
    auto result = builder.Build();
    ASSERT_FALSE(result.has_value());
}

TEST(CostBuilderTest, ParsesDouble) {
    CostBuilder builder;
    builder.OnDouble(0.50);

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 0.50);
}

TEST(CostBuilderTest, ParsesIntegerAsDouble) {
    CostBuilder builder;
    builder.OnUint(5);

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 5.0);
}

TEST(CostBuilderTest, FailsWhenEmpty) {
    CostBuilder builder;
    auto result = builder.Build();
    ASSERT_FALSE(result.has_value());
}

TEST(DatasetRangeBuilderTest, ParsesStartEnd) {
    DatasetRangeBuilder builder;

    builder.OnKey("start");
    builder.OnString("2023-01-01");
    builder.OnKey("end");
    builder.OnString("2026-01-20");

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->start, "2023-01-01");
    EXPECT_EQ(result->end, "2026-01-20");
}

TEST(DatasetRangeBuilderTest, FailsWhenMissingStart) {
    DatasetRangeBuilder builder;
    builder.OnKey("end");
    builder.OnString("2026-01-20");

    auto result = builder.Build();
    ASSERT_FALSE(result.has_value());
}

TEST(DatasetRangeBuilderTest, FailsWhenMissingEnd) {
    DatasetRangeBuilder builder;
    builder.OnKey("start");
    builder.OnString("2023-01-01");

    auto result = builder.Build();
    ASSERT_FALSE(result.has_value());
}

TEST(DatasetRangeBuilderTest, PartialSucceedsWithBothFields) {
    DatasetRangeBuilder builder;
    builder.OnKey("start");
    builder.OnString("2023-01-01");
    builder.OnKey("end");
    builder.OnString("2026-01-20");
    builder.OnKey("extra");

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
}

TEST(DatasetRangeBuilderTest, IgnoresUnknownKeys) {
    DatasetRangeBuilder builder;
    builder.OnKey("unknown");
    builder.OnString("ignored");
    builder.OnKey("start");
    builder.OnString("2023-01-01");
    builder.OnKey("another_unknown");
    builder.OnString("also_ignored");
    builder.OnKey("end");
    builder.OnString("2026-01-20");

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->start, "2023-01-01");
    EXPECT_EQ(result->end, "2026-01-20");
}

}  // namespace
}  // namespace dbn_pipe
