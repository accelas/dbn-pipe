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

TEST(TradingDateTest, FromNanosecondsConvertsToNewYorkDate) {
    // 2025-01-15 12:00:00 UTC = 2025-01-15 07:00:00 EST (same day)
    uint64_t noon_utc = 1736942400000000000ULL;  // 2025-01-15T12:00:00Z
    auto date = TradingDate::FromNanoseconds(noon_utc, "America/New_York");
    EXPECT_EQ(date.ToIsoString(), "2025-01-15");
}

TEST(TradingDateTest, FromNanosecondsHandlesMidnightEdge) {
    // 2025-01-15 04:00:00 UTC = 2025-01-14 23:00:00 EST (previous day!)
    uint64_t early_utc = 1736913600000000000ULL;  // 2025-01-15T04:00:00Z
    auto date = TradingDate::FromNanoseconds(early_utc, "America/New_York");
    EXPECT_EQ(date.ToIsoString(), "2025-01-14");
}

TEST(TradingDateTest, FromNanosecondsHandlesDST) {
    // 2024-06-03 04:30:00 UTC = 2024-06-03 00:30:00 EDT (same day in EDT, UTC-4)
    // This would be wrong with fixed EST offset (-5), which would give 2024-06-02
    uint64_t summer_utc = 1717389000000000000ULL;  // 2024-06-03T04:30:00Z
    auto date = TradingDate::FromNanoseconds(summer_utc, "America/New_York");
    EXPECT_EQ(date.ToIsoString(), "2024-06-03");  // Correctly handles EDT
}

TEST(TradingDateTest, FromNanosecondsWithDifferentTimezones) {
    // Same UTC timestamp, different timezones
    uint64_t utc_ts = 1736942400000000000ULL;  // 2025-01-15T12:00:00Z

    auto ny_date = TradingDate::FromNanoseconds(utc_ts, "America/New_York");
    auto utc_date = TradingDate::FromNanoseconds(utc_ts, "UTC");
    auto chicago_date = TradingDate::FromNanoseconds(utc_ts, "America/Chicago");

    EXPECT_EQ(ny_date.ToIsoString(), "2025-01-15");      // 07:00 EST
    EXPECT_EQ(utc_date.ToIsoString(), "2025-01-15");     // 12:00 UTC
    EXPECT_EQ(chicago_date.ToIsoString(), "2025-01-15"); // 06:00 CST
}

TEST(TradingDateTest, FromIsoStringThrowsOnInvalidFormat) {
    // Wrong length
    EXPECT_THROW(TradingDate::FromIsoString("2025-1-15"), std::invalid_argument);
    EXPECT_THROW(TradingDate::FromIsoString("25-01-15"), std::invalid_argument);
    EXPECT_THROW(TradingDate::FromIsoString(""), std::invalid_argument);

    // Wrong separators
    EXPECT_THROW(TradingDate::FromIsoString("2025/01/15"), std::invalid_argument);
    EXPECT_THROW(TradingDate::FromIsoString("2025.01.15"), std::invalid_argument);
    EXPECT_THROW(TradingDate::FromIsoString("20250115"), std::invalid_argument);

    // Non-digit characters
    EXPECT_THROW(TradingDate::FromIsoString("202X-01-15"), std::invalid_argument);
    EXPECT_THROW(TradingDate::FromIsoString("2025-0a-15"), std::invalid_argument);
    EXPECT_THROW(TradingDate::FromIsoString("abcd-ef-gh"), std::invalid_argument);
}

TEST(TradingDateTest, FromIsoStringThrowsOnInvalidCalendarDate) {
    // Invalid month
    EXPECT_THROW(TradingDate::FromIsoString("2025-00-15"), std::invalid_argument);
    EXPECT_THROW(TradingDate::FromIsoString("2025-13-15"), std::invalid_argument);

    // Invalid day
    EXPECT_THROW(TradingDate::FromIsoString("2025-01-00"), std::invalid_argument);
    EXPECT_THROW(TradingDate::FromIsoString("2025-01-32"), std::invalid_argument);

    // Invalid day for month
    EXPECT_THROW(TradingDate::FromIsoString("2025-02-30"), std::invalid_argument);
    EXPECT_THROW(TradingDate::FromIsoString("2025-04-31"), std::invalid_argument);

    // Leap year edge case
    EXPECT_THROW(TradingDate::FromIsoString("2025-02-29"), std::invalid_argument);  // Not a leap year
    EXPECT_NO_THROW(TradingDate::FromIsoString("2024-02-29"));  // Leap year - valid
}
