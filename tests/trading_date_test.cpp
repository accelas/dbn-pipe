// tests/trading_date_test.cpp
#include <gtest/gtest.h>
#include "src/trading_date.hpp"

using namespace dbn_pipe;

TEST(TradingDateTest, FromIsoStringParsesValidDate) {
    auto date = TradingDate::FromIsoString("2025-01-15");
    EXPECT_EQ(date.Year(), 2025);
    EXPECT_EQ(date.Month(), 1);
    EXPECT_EQ(date.Day(), 15);
}
