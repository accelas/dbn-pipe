// SPDX-License-Identifier: MIT

// tests/stype_test.cpp
#include <gtest/gtest.h>
#include "dbn_pipe/stype.hpp"

using namespace dbn_pipe;

TEST(STypeTest, STypeFromStringParsesValidTypes) {
    EXPECT_EQ(STypeFromString("raw_symbol"), SType::RawSymbol);
    EXPECT_EQ(STypeFromString("instrument_id"), SType::InstrumentId);
    EXPECT_EQ(STypeFromString("parent"), SType::Parent);
    EXPECT_EQ(STypeFromString("continuous"), SType::Continuous);
    EXPECT_EQ(STypeFromString("smart"), SType::Smart);
}

TEST(STypeTest, STypeFromStringReturnsNulloptForInvalid) {
    EXPECT_EQ(STypeFromString("invalid"), std::nullopt);
    EXPECT_EQ(STypeFromString(""), std::nullopt);
    EXPECT_EQ(STypeFromString("RAW_SYMBOL"), std::nullopt);  // Case sensitive
}

TEST(STypeTest, STypeToStringReturnsCorrectString) {
    EXPECT_EQ(STypeToString(SType::RawSymbol), "raw_symbol");
    EXPECT_EQ(STypeToString(SType::InstrumentId), "instrument_id");
    EXPECT_EQ(STypeToString(SType::Parent), "parent");
    EXPECT_EQ(STypeToString(SType::Continuous), "continuous");
    EXPECT_EQ(STypeToString(SType::Smart), "smart");
}
