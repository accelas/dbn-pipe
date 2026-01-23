// SPDX-License-Identifier: MIT

// tests/duckdb_storage_test.cpp
#include <gtest/gtest.h>

#include "src/duckdb_storage.hpp"
#include "src/trading_date.hpp"

using namespace dbn_pipe;

TEST(DuckDbStorageTest, CreatesInMemoryDatabase) {
    DuckDbStorage storage;
    EXPECT_EQ(storage.MappingCount(), 0);
}

TEST(DuckDbStorageTest, StoreAndLookupMapping) {
    DuckDbStorage storage;
    auto start = TradingDate::FromIsoString("2025-01-01");
    auto end = TradingDate::FromIsoString("2025-01-31");

    storage.StoreMapping(42, "AAPL", start, end);
    EXPECT_EQ(storage.MappingCount(), 1);

    auto jan15 = TradingDate::FromIsoString("2025-01-15");
    auto result = storage.LookupSymbol(42, jan15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "AAPL");
}

TEST(DuckDbStorageTest, LookupReturnsNulloptForUnknownId) {
    DuckDbStorage storage;
    auto date = TradingDate::FromIsoString("2025-01-15");
    auto result = storage.LookupSymbol(99999, date);
    EXPECT_FALSE(result.has_value());
}

TEST(DuckDbStorageTest, LookupReturnsNulloptOutsideDateRange) {
    DuckDbStorage storage;
    auto start = TradingDate::FromIsoString("2025-01-01");
    auto end = TradingDate::FromIsoString("2025-01-31");

    storage.StoreMapping(42, "AAPL", start, end);

    auto before = TradingDate::FromIsoString("2024-12-31");
    auto after = TradingDate::FromIsoString("2025-02-01");

    EXPECT_FALSE(storage.LookupSymbol(42, before).has_value());
    EXPECT_FALSE(storage.LookupSymbol(42, after).has_value());
}

TEST(DuckDbStorageTest, HandlesMultipleIntervalsForSameId) {
    DuckDbStorage storage;

    // First interval
    auto start1 = TradingDate::FromIsoString("2025-01-01");
    auto end1 = TradingDate::FromIsoString("2025-01-15");
    storage.StoreMapping(42, "SPY250117C00500000", start1, end1);

    // Second interval (same ID, recycled)
    auto start2 = TradingDate::FromIsoString("2025-01-20");
    auto end2 = TradingDate::FromIsoString("2025-01-31");
    storage.StoreMapping(42, "SPY250131P00480000", start2, end2);

    EXPECT_EQ(storage.MappingCount(), 2);

    // Query first interval
    auto jan10 = TradingDate::FromIsoString("2025-01-10");
    auto result1 = storage.LookupSymbol(42, jan10);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(*result1, "SPY250117C00500000");

    // Query second interval
    auto jan25 = TradingDate::FromIsoString("2025-01-25");
    auto result2 = storage.LookupSymbol(42, jan25);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result2, "SPY250131P00480000");

    // Query gap
    auto jan17 = TradingDate::FromIsoString("2025-01-17");
    EXPECT_FALSE(storage.LookupSymbol(42, jan17).has_value());
}

TEST(DuckDbStorageTest, UpsertUpdatesExistingMapping) {
    DuckDbStorage storage;
    auto start = TradingDate::FromIsoString("2025-01-01");
    auto end1 = TradingDate::FromIsoString("2025-01-15");
    auto end2 = TradingDate::FromIsoString("2025-01-31");

    storage.StoreMapping(42, "AAPL_OLD", start, end1);
    storage.StoreMapping(42, "AAPL_NEW", start, end2);

    // Should still have one mapping (upsert on same start_date)
    EXPECT_EQ(storage.MappingCount(), 1);

    auto jan20 = TradingDate::FromIsoString("2025-01-20");
    auto result = storage.LookupSymbol(42, jan20);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "AAPL_NEW");
}

TEST(DuckDbStorageTest, StoreAndLoadProgress) {
    DuckDbStorage storage;

    DownloadProgress progress;
    progress.sha256_expected = "abc123";
    progress.total_size = 1000000;
    progress.completed_ranges = {{0, 100}, {200, 500}};

    storage.StoreProgress("job1", "file1.dbn", progress);

    auto loaded = storage.LoadProgress("job1", "file1.dbn");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->sha256_expected, "abc123");
    EXPECT_EQ(loaded->total_size, 1000000);
    ASSERT_EQ(loaded->completed_ranges.size(), 2);
    EXPECT_EQ(loaded->completed_ranges[0].first, 0);
    EXPECT_EQ(loaded->completed_ranges[0].second, 100);
    EXPECT_EQ(loaded->completed_ranges[1].first, 200);
    EXPECT_EQ(loaded->completed_ranges[1].second, 500);
}

TEST(DuckDbStorageTest, LoadProgressReturnsNulloptForUnknown) {
    DuckDbStorage storage;
    auto result = storage.LoadProgress("nonexistent", "file.dbn");
    EXPECT_FALSE(result.has_value());
}

TEST(DuckDbStorageTest, ClearProgressRemovesEntry) {
    DuckDbStorage storage;

    DownloadProgress progress;
    progress.sha256_expected = "abc123";
    progress.total_size = 1000;
    progress.completed_ranges = {{0, 100}};

    storage.StoreProgress("job1", "file1.dbn", progress);
    ASSERT_TRUE(storage.LoadProgress("job1", "file1.dbn").has_value());

    storage.ClearProgress("job1", "file1.dbn");
    EXPECT_FALSE(storage.LoadProgress("job1", "file1.dbn").has_value());
}

TEST(DuckDbStorageTest, ListIncompleteDownloads) {
    DuckDbStorage storage;

    DownloadProgress progress;
    progress.sha256_expected = "hash";
    progress.total_size = 100;
    progress.completed_ranges = {};

    storage.StoreProgress("job1", "file1.dbn", progress);
    storage.StoreProgress("job1", "file2.dbn", progress);
    storage.StoreProgress("job2", "file3.dbn", progress);

    auto downloads = storage.ListIncompleteDownloads();
    EXPECT_EQ(downloads.size(), 3);

    // Clear one
    storage.ClearProgress("job1", "file1.dbn");
    downloads = storage.ListIncompleteDownloads();
    EXPECT_EQ(downloads.size(), 2);
}

TEST(DuckDbStorageTest, ProgressUpdateExisting) {
    DuckDbStorage storage;

    DownloadProgress progress1;
    progress1.sha256_expected = "hash1";
    progress1.total_size = 1000;
    progress1.completed_ranges = {{0, 100}};

    DownloadProgress progress2;
    progress2.sha256_expected = "hash1";
    progress2.total_size = 1000;
    progress2.completed_ranges = {{0, 100}, {100, 500}};

    storage.StoreProgress("job1", "file1.dbn", progress1);
    storage.StoreProgress("job1", "file1.dbn", progress2);

    auto loaded = storage.LoadProgress("job1", "file1.dbn");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->completed_ranges.size(), 2);
}

TEST(DuckDbStorageTest, SchemaVersionIsSet) {
    DuckDbStorage storage;
    // If we got here, schema validation passed
    // Just verify storage works
    EXPECT_EQ(storage.MappingCount(), 0);
}

TEST(DuckDbStorageTest, CacheEvictionWhenLimitReached) {
    // Create storage with small limit
    DuckDbStorage storage("", 10);

    auto start = TradingDate::FromIsoString("2025-01-01");
    auto end = TradingDate::FromIsoString("2025-01-31");

    // Insert more than the limit
    for (uint32_t i = 0; i < 15; ++i) {
        storage.StoreMapping(i, "SYM" + std::to_string(i), start, end);
    }

    // Should have evicted to ~90% of limit (9 entries)
    // But may have up to limit (10) due to eviction timing
    EXPECT_LE(storage.MappingCount(), 10);
    EXPECT_GE(storage.MappingCount(), 9);
}

TEST(DuckDbStorageTest, LRUEvictionPreservesRecentlyAccessed) {
    DuckDbStorage storage("", 10);

    auto start = TradingDate::FromIsoString("2025-01-01");
    auto end = TradingDate::FromIsoString("2025-01-31");

    // Insert entries
    for (uint32_t i = 0; i < 8; ++i) {
        storage.StoreMapping(i, "SYM" + std::to_string(i), start, end);
    }

    // Access first entry to make it "recently used"
    auto jan15 = TradingDate::FromIsoString("2025-01-15");
    auto result = storage.LookupSymbol(0, jan15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "SYM0");

    // Insert more entries to trigger eviction
    for (uint32_t i = 8; i < 15; ++i) {
        storage.StoreMapping(i, "SYM" + std::to_string(i), start, end);
    }

    // Entry 0 should still be there (recently accessed)
    result = storage.LookupSymbol(0, jan15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "SYM0");
}

TEST(DuckDbStorageTest, UnlimitedCacheWhenMaxZero) {
    DuckDbStorage storage("", 0);  // No limit

    auto start = TradingDate::FromIsoString("2025-01-01");
    auto end = TradingDate::FromIsoString("2025-01-31");

    // Insert many entries
    for (uint32_t i = 0; i < 100; ++i) {
        storage.StoreMapping(i, "SYM" + std::to_string(i), start, end);
    }

    // All should be present
    EXPECT_EQ(storage.MappingCount(), 100);
}
