// SPDX-License-Identifier: MIT

// tests/instrument_map_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <stdexcept>
#include <vector>

#include <databento/record.hpp>

#include "dbn_pipe/instrument_map.hpp"
#include "dbn_pipe/trading_date.hpp"

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

TEST(InstrumentMapTest, EndDateIsExclusive) {
    // Per databento-python MappingInterval semantics, end_date is exclusive.
    // A query on exactly end_date should return nullopt.
    InstrumentMap map;
    auto start = TradingDate::FromIsoString("2025-01-01");
    auto end = TradingDate::FromIsoString("2025-01-17");

    map.Insert(42, "AAPL", start, end);

    // start_date is inclusive
    auto result_start = map.Resolve(42, start);
    ASSERT_TRUE(result_start.has_value());
    EXPECT_EQ(*result_start, "AAPL");

    // Day before end_date should resolve
    auto day_before_end = TradingDate::FromIsoString("2025-01-16");
    auto result_before = map.Resolve(42, day_before_end);
    ASSERT_TRUE(result_before.has_value());
    EXPECT_EQ(*result_before, "AAPL");

    // end_date itself is exclusive - should NOT resolve
    EXPECT_FALSE(map.Resolve(42, end).has_value());
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

TEST(InstrumentMapTest, OnSymbolMappingMsgPopulatesMap) {
    InstrumentMap map;

    // Create a SymbolMappingMsg record
    std::vector<std::byte> record(sizeof(databento::SymbolMappingMsg));
    std::memset(record.data(), 0, record.size());

    auto* msg = reinterpret_cast<databento::SymbolMappingMsg*>(record.data());
    msg->hd.length = sizeof(databento::SymbolMappingMsg) / databento::RecordHeader::kLengthMultiplier;
    msg->hd.rtype = databento::RType::SymbolMapping;
    msg->hd.instrument_id = 42;

    // Use chrono duration for UnixNanos type
    using NanosDuration = std::chrono::duration<uint64_t, std::nano>;
    msg->start_ts = databento::UnixNanos{NanosDuration{1735689600000000000ULL}};  // 2025-01-01 00:00:00 UTC
    msg->end_ts = databento::UnixNanos{NanosDuration{1738281600000000000ULL}};    // 2025-01-31 00:00:00 UTC

    const char* symbol = "AAPL";
    std::strncpy(msg->stype_out_symbol.data(), symbol, msg->stype_out_symbol.size());

    map.OnSymbolMappingMsg(*msg);

    auto jan15 = TradingDate::FromIsoString("2025-01-15");
    auto result = map.Resolve(42, jan15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "AAPL");
}

TEST(InstrumentMapTest, TimezoneIsConfigurable) {
    // Default timezone
    InstrumentMap default_map;
    EXPECT_EQ(default_map.Timezone(), "America/New_York");

    // Custom timezone
    InstrumentMap chicago_map(nullptr, "America/Chicago");
    EXPECT_EQ(chicago_map.Timezone(), "America/Chicago");

    // UTC timezone
    InstrumentMap utc_map(nullptr, "UTC");
    EXPECT_EQ(utc_map.Timezone(), "UTC");
}

TEST(InstrumentMapTest, InvalidTimezoneThrows) {
    // Invalid timezone should throw at construction
    EXPECT_THROW(
        InstrumentMap(nullptr, "Invalid/Timezone"),
        std::runtime_error);

    EXPECT_THROW(
        InstrumentMap(nullptr, ""),
        std::runtime_error);
}
