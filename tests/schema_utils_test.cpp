// tests/schema_utils_test.cpp
#include <gtest/gtest.h>
#include "src/schema_utils.hpp"

using namespace dbn_pipe;

TEST(SchemaUtilsTest, SchemaFromStringParsesValidSchema) {
    EXPECT_EQ(SchemaFromString("trades"), Schema::Trades);
    EXPECT_EQ(SchemaFromString("mbp-1"), Schema::Mbp1);
    EXPECT_EQ(SchemaFromString("ohlcv-1d"), Schema::Ohlcv1D);
    EXPECT_EQ(SchemaFromString("definition"), Schema::Definition);
}

TEST(SchemaUtilsTest, SchemaFromStringReturnsNulloptForInvalid) {
    EXPECT_FALSE(SchemaFromString("invalid").has_value());
    EXPECT_FALSE(SchemaFromString("").has_value());
}

TEST(SchemaUtilsTest, SchemaToStringReturnsCorrectString) {
    EXPECT_EQ(SchemaToString(Schema::Trades), "trades");
    EXPECT_EQ(SchemaToString(Schema::Mbp1), "mbp-1");
    EXPECT_EQ(SchemaToString(Schema::Ohlcv1D), "ohlcv-1d");
}

TEST(SchemaUtilsTest, SchemaToRTypeMapsCorrectly) {
    EXPECT_EQ(SchemaToRType(Schema::Mbo), databento::RType::Mbo);
    EXPECT_EQ(SchemaToRType(Schema::Mbp1), databento::RType::Mbp1);
    EXPECT_EQ(SchemaToRType(Schema::Trades), databento::RType::Mbp0);
    EXPECT_EQ(SchemaToRType(Schema::Definition), databento::RType::InstrumentDef);
    EXPECT_EQ(SchemaToRType(Schema::Ohlcv1D), databento::RType::Ohlcv1D);
}

TEST(SchemaUtilsTest, DatasetToSchemaNameConverts) {
    EXPECT_EQ(DatasetToSchemaName("OPRA.PILLAR"), "opra_pillar");
    EXPECT_EQ(DatasetToSchemaName("XNAS.ITCH"), "xnas_itch");
    EXPECT_EQ(DatasetToSchemaName("EQUS.MINI"), "equs_mini");
}
