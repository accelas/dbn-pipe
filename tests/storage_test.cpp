// SPDX-License-Identifier: MIT

// tests/storage_test.cpp
#include <gtest/gtest.h>
#include "dbn_pipe/storage.hpp"
#include "dbn_pipe/trading_date.hpp"

using namespace dbn_pipe;

TEST(NoOpStorageTest, LookupAlwaysReturnsNullopt) {
    NoOpStorage storage;
    auto date = TradingDate::FromIsoString("2025-01-15");
    auto result = storage.LookupSymbol(12345, date);
    EXPECT_FALSE(result.has_value());
}

TEST(NoOpStorageTest, StoreDoesNotThrow) {
    NoOpStorage storage;
    auto start = TradingDate::FromIsoString("2025-01-01");
    auto end = TradingDate::FromIsoString("2025-12-31");
    // Should not throw
    storage.StoreMapping(12345, "AAPL", start, end);
}
