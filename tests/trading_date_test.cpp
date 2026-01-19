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

TEST(TradingDateTest, ToIsoStringFormatsCorrectly) {
    auto date = TradingDate::FromIsoString("2025-01-15");
    EXPECT_EQ(date.ToIsoString(), "2025-01-15");
}

TEST(TradingDateTest, LessThanComparison) {
    auto earlier = TradingDate::FromIsoString("2025-01-14");
    auto later = TradingDate::FromIsoString("2025-01-15");
    EXPECT_TRUE(earlier < later);
    EXPECT_FALSE(later < earlier);
    EXPECT_FALSE(earlier < earlier);
}

TEST(TradingDateTest, EqualityComparison) {
    auto date1 = TradingDate::FromIsoString("2025-01-15");
    auto date2 = TradingDate::FromIsoString("2025-01-15");
    auto date3 = TradingDate::FromIsoString("2025-01-16");
    EXPECT_TRUE(date1 == date2);
    EXPECT_FALSE(date1 == date3);
}

TEST(TradingDateTest, LessThanOrEqualComparison) {
    auto earlier = TradingDate::FromIsoString("2025-01-14");
    auto later = TradingDate::FromIsoString("2025-01-15");
    EXPECT_TRUE(earlier <= later);
    EXPECT_TRUE(earlier <= earlier);
    EXPECT_FALSE(later <= earlier);
}
