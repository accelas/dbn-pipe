// tests/instrument_map_test.cpp
#include <gtest/gtest.h>
#include "src/instrument_map.hpp"
#include "src/trading_date.hpp"

using namespace dbn_pipe;

TEST(InstrumentMapTest, ResolveReturnsNulloptForUnknownId) {
    InstrumentMap map;
    auto date = TradingDate::FromIsoString("2025-01-15");
    auto result = map.Resolve(12345, date);
    EXPECT_FALSE(result.has_value());
}

TEST(InstrumentMapTest, InsertAndResolveWithinDateRange) {
    InstrumentMap map;
    auto start = TradingDate::FromIsoString("2025-01-01");
    auto end = TradingDate::FromIsoString("2025-01-31");

    map.Insert(42, "AAPL", start, end);

    auto mid_date = TradingDate::FromIsoString("2025-01-15");
    auto result = map.Resolve(42, mid_date);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "AAPL");
}

TEST(InstrumentMapTest, ResolveReturnsNulloptOutsideDateRange) {
    InstrumentMap map;
    auto start = TradingDate::FromIsoString("2025-01-01");
    auto end = TradingDate::FromIsoString("2025-01-31");

    map.Insert(42, "AAPL", start, end);

    auto before = TradingDate::FromIsoString("2024-12-31");
    auto after = TradingDate::FromIsoString("2025-02-01");

    EXPECT_FALSE(map.Resolve(42, before).has_value());
    EXPECT_FALSE(map.Resolve(42, after).has_value());
}

TEST(InstrumentMapTest, HandlesMultipleIntervalsForSameId) {
    // OPRA recycling: same instrument_id used for different contracts on different days
    InstrumentMap map;

    // First contract: Jan 1-15
    auto start1 = TradingDate::FromIsoString("2025-01-01");
    auto end1 = TradingDate::FromIsoString("2025-01-15");
    map.Insert(42, "SPY250117C00500000", start1, end1);

    // Second contract (same ID, recycled): Jan 20-31
    auto start2 = TradingDate::FromIsoString("2025-01-20");
    auto end2 = TradingDate::FromIsoString("2025-01-31");
    map.Insert(42, "SPY250131P00480000", start2, end2);

    // Query first interval
    auto jan10 = TradingDate::FromIsoString("2025-01-10");
    auto result1 = map.Resolve(42, jan10);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(*result1, "SPY250117C00500000");

    // Query second interval
    auto jan25 = TradingDate::FromIsoString("2025-01-25");
    auto result2 = map.Resolve(42, jan25);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result2, "SPY250131P00480000");

    // Query gap between intervals
    auto jan17 = TradingDate::FromIsoString("2025-01-17");
    EXPECT_FALSE(map.Resolve(42, jan17).has_value());
}
