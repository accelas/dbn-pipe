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
