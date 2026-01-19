# Databento API Feature Parity Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bring dbn-pipe to feature parity with official Databento Python API for symbol resolution, HTTP APIs, and file handling.

**Architecture:** Date-interval symbol tracking via InstrumentMap, streaming JSON parsing for large API responses, optional DuckDB persistence via dependency injection, TDD throughout.

**Tech Stack:** C++23, Bazel, GoogleTest, rapidjson (SAX), DuckDB (optional), databento-cpp headers

---

## Phase 1: Core Symbol Resolution

### Task 1.1: TradingDate Class

**Files:**
- Create: `src/trading_date.hpp`
- Test: `tests/trading_date_test.cpp`
- Modify: `src/BUILD.bazel` (add library)
- Modify: `tests/BUILD.bazel` (add test)

**Step 1: Write the failing test for FromIsoString**

```cpp
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
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:trading_date_test --test_output=errors`
Expected: BUILD ERROR - file not found

**Step 3: Create minimal header and BUILD rule**

```cpp
// src/trading_date.hpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dbn_pipe {

class TradingDate {
public:
    static TradingDate FromIsoString(std::string_view iso_date) {
        // Parse "YYYY-MM-DD"
        int year = 0, month = 0, day = 0;
        if (iso_date.size() >= 10) {
            year = (iso_date[0] - '0') * 1000 + (iso_date[1] - '0') * 100 +
                   (iso_date[2] - '0') * 10 + (iso_date[3] - '0');
            month = (iso_date[5] - '0') * 10 + (iso_date[6] - '0');
            day = (iso_date[8] - '0') * 10 + (iso_date[9] - '0');
        }
        return TradingDate(year, month, day);
    }

    int Year() const { return year_; }
    int Month() const { return month_; }
    int Day() const { return day_; }

private:
    TradingDate(int year, int month, int day)
        : year_(year), month_(month), day_(day) {}

    int16_t year_;
    int8_t month_;
    int8_t day_;
};

}  // namespace dbn_pipe
```

Add to `src/BUILD.bazel`:
```python
cc_library(
    name = "trading_date",
    hdrs = ["trading_date.hpp"],
    visibility = ["//visibility:public"],
)
```

Add to `tests/BUILD.bazel`:
```python
cc_test(
    name = "trading_date_test",
    srcs = ["trading_date_test.cpp"],
    deps = [
        "//src:trading_date",
        "@googletest//:gtest_main",
    ],
)
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:trading_date_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add src/trading_date.hpp src/BUILD.bazel tests/trading_date_test.cpp tests/BUILD.bazel
git commit -m "feat: add TradingDate with FromIsoString parsing"
```

---

### Task 1.2: TradingDate ToIsoString and Comparison

**Files:**
- Modify: `src/trading_date.hpp`
- Modify: `tests/trading_date_test.cpp`

**Step 1: Write the failing tests**

```cpp
// Add to tests/trading_date_test.cpp

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
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:trading_date_test --test_output=errors`
Expected: FAIL - ToIsoString not defined

**Step 3: Add methods to TradingDate**

```cpp
// Add to TradingDate class in src/trading_date.hpp

    std::string ToIsoString() const {
        char buf[11];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year_, month_, day_);
        return std::string(buf);
    }

    bool operator<(const TradingDate& other) const {
        if (year_ != other.year_) return year_ < other.year_;
        if (month_ != other.month_) return month_ < other.month_;
        return day_ < other.day_;
    }

    bool operator==(const TradingDate& other) const {
        return year_ == other.year_ && month_ == other.month_ && day_ == other.day_;
    }

    bool operator<=(const TradingDate& other) const {
        return *this < other || *this == other;
    }

    bool operator>(const TradingDate& other) const {
        return other < *this;
    }

    bool operator>=(const TradingDate& other) const {
        return other <= *this;
    }

    bool operator!=(const TradingDate& other) const {
        return !(*this == other);
    }
```

Add include at top:
```cpp
#include <cstdio>
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:trading_date_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add src/trading_date.hpp tests/trading_date_test.cpp
git commit -m "feat: add TradingDate comparison operators and ToIsoString"
```

---

### Task 1.3: TradingDate FromNanoseconds (America/New_York)

**Files:**
- Modify: `src/trading_date.hpp`
- Modify: `tests/trading_date_test.cpp`

**Step 1: Write the failing test**

```cpp
// Add to tests/trading_date_test.cpp

TEST(TradingDateTest, FromNanosecondsConvertsToNewYorkDate) {
    // 2025-01-15 12:00:00 UTC = 2025-01-15 07:00:00 EST (same day)
    uint64_t noon_utc = 1736942400000000000ULL;  // 2025-01-15T12:00:00Z
    auto date = TradingDate::FromNanoseconds(noon_utc);
    EXPECT_EQ(date.ToIsoString(), "2025-01-15");
}

TEST(TradingDateTest, FromNanosecondsHandlesMidnightEdge) {
    // 2025-01-15 04:00:00 UTC = 2025-01-14 23:00:00 EST (previous day!)
    uint64_t early_utc = 1736913600000000000ULL;  // 2025-01-15T04:00:00Z
    auto date = TradingDate::FromNanoseconds(early_utc);
    EXPECT_EQ(date.ToIsoString(), "2025-01-14");
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:trading_date_test --test_output=errors`
Expected: FAIL - FromNanoseconds not defined

**Step 3: Implement FromNanoseconds with EST offset**

```cpp
// Add to TradingDate class in src/trading_date.hpp

    static TradingDate FromNanoseconds(uint64_t ns_since_epoch) {
        // Convert to seconds
        int64_t seconds = static_cast<int64_t>(ns_since_epoch / 1000000000ULL);

        // Apply EST offset (-5 hours = -18000 seconds)
        // Note: This is simplified - doesn't handle DST. For production,
        // consider using date library or system tzdb.
        constexpr int64_t kEstOffsetSeconds = -5 * 3600;
        seconds += kEstOffsetSeconds;

        // Convert seconds since epoch to date components
        // Using a simplified calculation (days since 1970-01-01)
        int64_t days = seconds / 86400;
        if (seconds < 0 && seconds % 86400 != 0) {
            days--;  // Floor division for negative
        }

        return FromDaysSinceEpoch(static_cast<int32_t>(days));
    }

private:
    static TradingDate FromDaysSinceEpoch(int32_t days) {
        // Convert days since 1970-01-01 to year/month/day
        // Algorithm from Howard Hinnant's date library
        days += 719468;  // Shift to March 1, 0000
        int32_t era = (days >= 0 ? days : days - 146096) / 146097;
        int32_t doe = days - era * 146097;
        int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int32_t y = yoe + era * 400;
        int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int32_t mp = (5 * doy + 2) / 153;
        int32_t d = doy - (153 * mp + 2) / 5 + 1;
        int32_t m = mp + (mp < 10 ? 3 : -9);
        y += (m <= 2);
        return TradingDate(y, m, d);
    }
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:trading_date_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add src/trading_date.hpp tests/trading_date_test.cpp
git commit -m "feat: add TradingDate::FromNanoseconds with EST timezone"
```

---

### Task 1.4: IStorage Interface and NoOpStorage

**Files:**
- Create: `src/storage.hpp`
- Test: `tests/storage_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

```cpp
// tests/storage_test.cpp
#include <gtest/gtest.h>
#include "src/storage.hpp"
#include "src/trading_date.hpp"

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
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:storage_test --test_output=errors`
Expected: BUILD ERROR - file not found

**Step 3: Create storage interface and NoOpStorage**

```cpp
// src/storage.hpp
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "trading_date.hpp"

namespace dbn_pipe {

// Forward declaration for Phase 5
struct DownloadProgress;

// Abstract storage interface for symbol mappings and download progress
class IStorage {
public:
    virtual ~IStorage() = default;

    // Symbol map operations
    virtual void StoreMapping(uint32_t instrument_id, const std::string& symbol,
                              const TradingDate& start, const TradingDate& end) = 0;
    virtual std::optional<std::string> LookupSymbol(uint32_t instrument_id,
                                                     const TradingDate& date) = 0;

    // Download progress operations (Phase 5 - stub for now)
    virtual void StoreProgress(const std::string& job_id, const std::string& filename,
                               const DownloadProgress& progress) = 0;
    virtual std::optional<DownloadProgress> LoadProgress(const std::string& job_id,
                                                          const std::string& filename) = 0;
    virtual void ClearProgress(const std::string& job_id, const std::string& filename) = 0;
    virtual std::vector<std::pair<std::string, std::string>> ListIncompleteDownloads() = 0;
};

// Stub for Phase 5
struct DownloadProgress {
    std::string sha256_expected;
    uint64_t total_size = 0;
    std::vector<std::pair<uint64_t, uint64_t>> completed_ranges;
};

// No-op implementation - default when no persistence needed
class NoOpStorage : public IStorage {
public:
    void StoreMapping(uint32_t, const std::string&,
                      const TradingDate&, const TradingDate&) override {}

    std::optional<std::string> LookupSymbol(uint32_t, const TradingDate&) override {
        return std::nullopt;
    }

    void StoreProgress(const std::string&, const std::string&,
                       const DownloadProgress&) override {}

    std::optional<DownloadProgress> LoadProgress(const std::string&,
                                                  const std::string&) override {
        return std::nullopt;
    }

    void ClearProgress(const std::string&, const std::string&) override {}

    std::vector<std::pair<std::string, std::string>> ListIncompleteDownloads() override {
        return {};
    }
};

}  // namespace dbn_pipe
```

Add to `src/BUILD.bazel`:
```python
cc_library(
    name = "storage",
    hdrs = ["storage.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":trading_date",
    ],
)
```

Add to `tests/BUILD.bazel`:
```python
cc_test(
    name = "storage_test",
    srcs = ["storage_test.cpp"],
    deps = [
        "//src:storage",
        "//src:trading_date",
        "@googletest//:gtest_main",
    ],
)
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:storage_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add src/storage.hpp src/BUILD.bazel tests/storage_test.cpp tests/BUILD.bazel
git commit -m "feat: add IStorage interface and NoOpStorage implementation"
```

---

### Task 1.5: InstrumentMap Basic Insert and Resolve

**Files:**
- Create: `src/instrument_map.hpp`
- Test: `tests/instrument_map_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

```cpp
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
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:instrument_map_test --test_output=errors`
Expected: BUILD ERROR - file not found

**Step 3: Create InstrumentMap with basic functionality**

```cpp
// src/instrument_map.hpp
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage.hpp"
#include "trading_date.hpp"

namespace dbn_pipe {

// Single mapping interval
struct MappingInterval {
    TradingDate start_date;
    TradingDate end_date;
    std::string symbol;
};

// InstrumentMap with date-interval tracking for symbol resolution
class InstrumentMap {
public:
    explicit InstrumentMap(std::shared_ptr<IStorage> storage = nullptr)
        : storage_(storage ? storage : std::make_shared<NoOpStorage>()) {}

    // Insert mapping with date range
    void Insert(uint32_t instrument_id, const std::string& symbol,
                const TradingDate& start, const TradingDate& end) {
        auto& intervals = mappings_[instrument_id];
        intervals.push_back({start, end, symbol});

        // Keep sorted by start_date for binary search
        std::sort(intervals.begin(), intervals.end(),
                  [](const MappingInterval& a, const MappingInterval& b) {
                      return a.start_date < b.start_date;
                  });

        // Persist to storage
        storage_->StoreMapping(instrument_id, symbol, start, end);
    }

    // Resolve symbol for specific date - O(log n) binary search
    std::optional<std::string> Resolve(uint32_t instrument_id,
                                        const TradingDate& date) const {
        auto it = mappings_.find(instrument_id);
        if (it == mappings_.end()) {
            // Try storage fallback
            return storage_->LookupSymbol(instrument_id, date);
        }

        const auto& intervals = it->second;

        // Binary search for interval containing date
        auto interval_it = std::lower_bound(
            intervals.begin(), intervals.end(), date,
            [](const MappingInterval& interval, const TradingDate& d) {
                return interval.end_date < d;
            });

        if (interval_it != intervals.end() &&
            interval_it->start_date <= date && date <= interval_it->end_date) {
            return interval_it->symbol;
        }

        return std::nullopt;
    }

    // Clear all mappings
    void Clear() {
        mappings_.clear();
    }

    // Get number of instrument_ids tracked
    std::size_t Size() const {
        return mappings_.size();
    }

private:
    std::unordered_map<uint32_t, std::vector<MappingInterval>> mappings_;
    std::shared_ptr<IStorage> storage_;
};

}  // namespace dbn_pipe
```

Add to `src/BUILD.bazel`:
```python
cc_library(
    name = "instrument_map",
    hdrs = ["instrument_map.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":storage",
        ":trading_date",
    ],
)
```

Add to `tests/BUILD.bazel`:
```python
cc_test(
    name = "instrument_map_test",
    srcs = ["instrument_map_test.cpp"],
    deps = [
        "//src:instrument_map",
        "//src:trading_date",
        "@googletest//:gtest_main",
    ],
)
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:instrument_map_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add src/instrument_map.hpp src/BUILD.bazel tests/instrument_map_test.cpp tests/BUILD.bazel
git commit -m "feat: add InstrumentMap with date-interval tracking"
```

---

### Task 1.6: InstrumentMap Multiple Intervals (OPRA Recycling)

**Files:**
- Modify: `tests/instrument_map_test.cpp`

**Step 1: Write the failing test**

```cpp
// Add to tests/instrument_map_test.cpp

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
```

**Step 2: Run test to verify it passes (already implemented)**

Run: `bazel test //tests:instrument_map_test --test_output=errors`
Expected: PASS (binary search handles this)

**Step 3: Commit test**

```bash
git add tests/instrument_map_test.cpp
git commit -m "test: add InstrumentMap multiple interval test for OPRA recycling"
```

---

### Task 1.7: InstrumentMap OnSymbolMappingMsg

**Files:**
- Modify: `src/instrument_map.hpp`
- Modify: `tests/instrument_map_test.cpp`
- Modify: `src/BUILD.bazel` (add databento dependency)

**Step 1: Write the failing test**

```cpp
// Add to tests/instrument_map_test.cpp
#include <cstring>
#include <databento/record.hpp>

TEST(InstrumentMapTest, OnSymbolMappingMsgPopulatesMap) {
    InstrumentMap map;

    // Create a SymbolMappingMsg record
    std::vector<std::byte> record(sizeof(databento::SymbolMappingMsg));
    std::memset(record.data(), 0, record.size());

    auto* msg = reinterpret_cast<databento::SymbolMappingMsg*>(record.data());
    msg->hd.length = sizeof(databento::SymbolMappingMsg) / databento::RecordHeader::kLengthMultiplier;
    msg->hd.rtype = databento::RType::SymbolMapping;
    msg->hd.instrument_id = 42;
    msg->start_ts = 1735689600000000000ULL;  // 2025-01-01 00:00:00 UTC
    msg->end_ts = 1738281600000000000ULL;    // 2025-01-31 00:00:00 UTC

    const char* symbol = "AAPL";
    std::strncpy(msg->stype_out_symbol.data(), symbol, msg->stype_out_symbol.size());

    map.OnSymbolMappingMsg(*msg);

    auto jan15 = TradingDate::FromIsoString("2025-01-15");
    auto result = map.Resolve(42, jan15);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "AAPL");
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:instrument_map_test --test_output=errors`
Expected: FAIL - OnSymbolMappingMsg not defined

**Step 3: Add OnSymbolMappingMsg method**

```cpp
// Add to InstrumentMap class in src/instrument_map.hpp
// Add include at top:
#include <databento/record.hpp>

    // Populate from DBN stream records
    void OnSymbolMappingMsg(const databento::SymbolMappingMsg& msg) {
        uint32_t id = msg.hd.instrument_id;
        std::string symbol(msg.STypeOutSymbol());

        // Convert timestamps to TradingDates
        auto start = TradingDate::FromNanoseconds(msg.start_ts);
        auto end = TradingDate::FromNanoseconds(msg.end_ts);

        Insert(id, symbol, start, end);
    }
```

Update `src/BUILD.bazel` for instrument_map:
```python
cc_library(
    name = "instrument_map",
    hdrs = ["instrument_map.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        ":storage",
        ":trading_date",
        "@databento_cpp//:databento_headers",
    ],
)
```

Update `tests/BUILD.bazel` for instrument_map_test:
```python
cc_test(
    name = "instrument_map_test",
    srcs = ["instrument_map_test.cpp"],
    deps = [
        "//src:instrument_map",
        "//src:trading_date",
        "@databento_cpp//:databento_with_impl",
        "@googletest//:gtest_main",
    ],
)
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:instrument_map_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add src/instrument_map.hpp src/BUILD.bazel tests/instrument_map_test.cpp tests/BUILD.bazel
git commit -m "feat: add InstrumentMap::OnSymbolMappingMsg for DBN stream population"
```

---

## Phase 3: Utilities Migration (RetryPolicy)

### Task 3.1: RetryPolicy Basic Structure

**Files:**
- Create: `src/retry_policy.hpp`
- Test: `tests/retry_policy_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

```cpp
// tests/retry_policy_test.cpp
#include <gtest/gtest.h>
#include "src/retry_policy.hpp"

using namespace dbn_pipe;

TEST(RetryPolicyTest, ShouldRetryInitiallyTrue) {
    RetryConfig config{.max_retries = 3};
    RetryPolicy policy(config);
    EXPECT_TRUE(policy.ShouldRetry());
}

TEST(RetryPolicyTest, ShouldRetryFalseAfterMaxAttempts) {
    RetryConfig config{.max_retries = 2};
    RetryPolicy policy(config);

    policy.RecordAttempt();
    EXPECT_TRUE(policy.ShouldRetry());

    policy.RecordAttempt();
    EXPECT_FALSE(policy.ShouldRetry());
}

TEST(RetryPolicyTest, ResetClearsAttempts) {
    RetryConfig config{.max_retries = 2};
    RetryPolicy policy(config);

    policy.RecordAttempt();
    policy.RecordAttempt();
    EXPECT_FALSE(policy.ShouldRetry());

    policy.Reset();
    EXPECT_TRUE(policy.ShouldRetry());
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:retry_policy_test --test_output=errors`
Expected: BUILD ERROR - file not found

**Step 3: Create RetryPolicy**

```cpp
// src/retry_policy.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>

namespace dbn_pipe {

struct RetryConfig {
    uint32_t max_retries = 5;
    std::chrono::milliseconds initial_delay{1000};
    std::chrono::milliseconds max_delay{60000};
    double backoff_multiplier = 2.0;
    double jitter_factor = 0.1;  // +/- 10%
};

class RetryPolicy {
public:
    explicit RetryPolicy(RetryConfig config = {})
        : config_(config), attempts_(0) {}

    bool ShouldRetry() const {
        return attempts_ < config_.max_retries;
    }

    void RecordAttempt() {
        ++attempts_;
    }

    void Reset() {
        attempts_ = 0;
    }

    // Calculate next delay with exponential backoff and jitter
    std::chrono::milliseconds GetNextDelay(
        std::optional<std::chrono::seconds> retry_after = std::nullopt) const {

        // If server specified Retry-After, use it
        if (retry_after.has_value()) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(*retry_after);
        }

        // Exponential backoff: initial * multiplier^attempts
        double delay_ms = static_cast<double>(config_.initial_delay.count());
        for (uint32_t i = 0; i < attempts_; ++i) {
            delay_ms *= config_.backoff_multiplier;
        }

        // Cap at max delay
        delay_ms = std::min(delay_ms, static_cast<double>(config_.max_delay.count()));

        // Add jitter
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(
            1.0 - config_.jitter_factor,
            1.0 + config_.jitter_factor);
        delay_ms *= dis(gen);

        return std::chrono::milliseconds(static_cast<int64_t>(delay_ms));
    }

    uint32_t Attempts() const { return attempts_; }

private:
    RetryConfig config_;
    uint32_t attempts_;
};

}  // namespace dbn_pipe
```

Add to `src/BUILD.bazel`:
```python
cc_library(
    name = "retry_policy",
    hdrs = ["retry_policy.hpp"],
    visibility = ["//visibility:public"],
)
```

Add to `tests/BUILD.bazel`:
```python
cc_test(
    name = "retry_policy_test",
    srcs = ["retry_policy_test.cpp"],
    deps = [
        "//src:retry_policy",
        "@googletest//:gtest_main",
    ],
)
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:retry_policy_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add src/retry_policy.hpp src/BUILD.bazel tests/retry_policy_test.cpp tests/BUILD.bazel
git commit -m "feat: add RetryPolicy with exponential backoff and jitter"
```

---

### Task 3.2: RetryPolicy Delay Calculation

**Files:**
- Modify: `tests/retry_policy_test.cpp`

**Step 1: Write the failing test**

```cpp
// Add to tests/retry_policy_test.cpp

TEST(RetryPolicyTest, GetNextDelayRespectsRetryAfterHeader) {
    RetryConfig config{.initial_delay = std::chrono::milliseconds(1000)};
    RetryPolicy policy(config);

    auto delay = policy.GetNextDelay(std::chrono::seconds(30));
    EXPECT_EQ(delay.count(), 30000);  // 30 seconds in ms
}

TEST(RetryPolicyTest, GetNextDelayUsesExponentialBackoff) {
    RetryConfig config{
        .initial_delay = std::chrono::milliseconds(100),
        .backoff_multiplier = 2.0,
        .jitter_factor = 0.0  // No jitter for predictable test
    };
    RetryPolicy policy(config);

    // First attempt: 100ms
    auto delay0 = policy.GetNextDelay();
    EXPECT_EQ(delay0.count(), 100);

    policy.RecordAttempt();

    // Second attempt: 200ms
    auto delay1 = policy.GetNextDelay();
    EXPECT_EQ(delay1.count(), 200);

    policy.RecordAttempt();

    // Third attempt: 400ms
    auto delay2 = policy.GetNextDelay();
    EXPECT_EQ(delay2.count(), 400);
}

TEST(RetryPolicyTest, GetNextDelayCapsAtMaxDelay) {
    RetryConfig config{
        .initial_delay = std::chrono::milliseconds(1000),
        .max_delay = std::chrono::milliseconds(5000),
        .backoff_multiplier = 10.0,
        .jitter_factor = 0.0
    };
    RetryPolicy policy(config);

    policy.RecordAttempt();  // Would be 10000ms without cap

    auto delay = policy.GetNextDelay();
    EXPECT_EQ(delay.count(), 5000);  // Capped at max
}
```

**Step 2: Run test to verify it passes (already implemented)**

Run: `bazel test //tests:retry_policy_test --test_output=errors`
Expected: PASS

**Step 3: Commit test**

```bash
git add tests/retry_policy_test.cpp
git commit -m "test: add RetryPolicy delay calculation tests"
```

---

## Phase 4: Schema Infrastructure

### Task 4.1: Schema Enum and String Conversion

**Files:**
- Create: `src/schema_utils.hpp`
- Test: `tests/schema_utils_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

```cpp
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
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:schema_utils_test --test_output=errors`
Expected: BUILD ERROR - file not found

**Step 3: Create schema_utils**

```cpp
// src/schema_utils.hpp
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <databento/enums.hpp>

namespace dbn_pipe {

// Schema enum matching Databento schemas
enum class Schema {
    Mbo,
    Mbp1,
    Mbp10,
    Trades,
    Tbbo,
    Ohlcv1S,
    Ohlcv1M,
    Ohlcv1H,
    Ohlcv1D,
    Definition,
    Statistics,
    Status,
    Imbalance,
    Cbbo,
    Cbbo1S,
    Cbbo1M,
    Tcbbo,
    Bbo1S,
    Bbo1M
};

// Schema from string
inline std::optional<Schema> SchemaFromString(std::string_view s) {
    if (s == "mbo") return Schema::Mbo;
    if (s == "mbp-1") return Schema::Mbp1;
    if (s == "mbp-10") return Schema::Mbp10;
    if (s == "trades") return Schema::Trades;
    if (s == "tbbo") return Schema::Tbbo;
    if (s == "ohlcv-1s") return Schema::Ohlcv1S;
    if (s == "ohlcv-1m") return Schema::Ohlcv1M;
    if (s == "ohlcv-1h") return Schema::Ohlcv1H;
    if (s == "ohlcv-1d") return Schema::Ohlcv1D;
    if (s == "definition") return Schema::Definition;
    if (s == "statistics") return Schema::Statistics;
    if (s == "status") return Schema::Status;
    if (s == "imbalance") return Schema::Imbalance;
    if (s == "cbbo") return Schema::Cbbo;
    if (s == "cbbo-1s") return Schema::Cbbo1S;
    if (s == "cbbo-1m") return Schema::Cbbo1M;
    if (s == "tcbbo") return Schema::Tcbbo;
    if (s == "bbo-1s") return Schema::Bbo1S;
    if (s == "bbo-1m") return Schema::Bbo1M;
    return std::nullopt;
}

// Schema to string
inline std::string_view SchemaToString(Schema schema) {
    switch (schema) {
        case Schema::Mbo: return "mbo";
        case Schema::Mbp1: return "mbp-1";
        case Schema::Mbp10: return "mbp-10";
        case Schema::Trades: return "trades";
        case Schema::Tbbo: return "tbbo";
        case Schema::Ohlcv1S: return "ohlcv-1s";
        case Schema::Ohlcv1M: return "ohlcv-1m";
        case Schema::Ohlcv1H: return "ohlcv-1h";
        case Schema::Ohlcv1D: return "ohlcv-1d";
        case Schema::Definition: return "definition";
        case Schema::Statistics: return "statistics";
        case Schema::Status: return "status";
        case Schema::Imbalance: return "imbalance";
        case Schema::Cbbo: return "cbbo";
        case Schema::Cbbo1S: return "cbbo-1s";
        case Schema::Cbbo1M: return "cbbo-1m";
        case Schema::Tcbbo: return "tcbbo";
        case Schema::Bbo1S: return "bbo-1s";
        case Schema::Bbo1M: return "bbo-1m";
    }
    return "";  // Unreachable
}

// Schema to RType mapping
inline std::optional<databento::RType> SchemaToRType(Schema schema) {
    switch (schema) {
        case Schema::Mbo: return databento::RType::Mbo;
        case Schema::Mbp1: return databento::RType::Mbp1;
        case Schema::Mbp10: return databento::RType::Mbp10;
        case Schema::Trades: return databento::RType::Mbp0;  // Trades use Mbp0
        case Schema::Tbbo: return databento::RType::Mbp1;
        case Schema::Ohlcv1S:
        case Schema::Ohlcv1M:
        case Schema::Ohlcv1H:
        case Schema::Ohlcv1D: return databento::RType::OhlcvDeprecated;
        case Schema::Definition: return databento::RType::InstrumentDef;
        case Schema::Statistics: return databento::RType::Statistics;
        case Schema::Status: return databento::RType::Status;
        case Schema::Imbalance: return databento::RType::Imbalance;
        case Schema::Cbbo:
        case Schema::Cbbo1S:
        case Schema::Cbbo1M: return databento::RType::Cbbo;
        case Schema::Tcbbo: return databento::RType::Tcbbo;
        case Schema::Bbo1S:
        case Schema::Bbo1M: return databento::RType::Bbo1S;
    }
    return std::nullopt;
}

// Dataset to schema name (e.g., "OPRA.PILLAR" -> "opra_pillar")
inline std::string DatasetToSchemaName(const std::string& dataset) {
    std::string result;
    result.reserve(dataset.size());
    for (char c : dataset) {
        if (c == '.') {
            result += '_';
        } else {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return result;
}

}  // namespace dbn_pipe
```

Add to `src/BUILD.bazel`:
```python
cc_library(
    name = "schema_utils",
    hdrs = ["schema_utils.hpp"],
    visibility = ["//visibility:public"],
    deps = [
        "@databento_cpp//:databento_headers",
    ],
)
```

Add to `tests/BUILD.bazel`:
```python
cc_test(
    name = "schema_utils_test",
    srcs = ["schema_utils_test.cpp"],
    deps = [
        "//src:schema_utils",
        "@googletest//:gtest_main",
    ],
)
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:schema_utils_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add src/schema_utils.hpp src/BUILD.bazel tests/schema_utils_test.cpp tests/BUILD.bazel
git commit -m "feat: add schema_utils with Schema enum and conversions"
```

---

### Task 4.2: SchemaToRType and DatasetToSchemaName

**Files:**
- Modify: `tests/schema_utils_test.cpp`

**Step 1: Write additional tests**

```cpp
// Add to tests/schema_utils_test.cpp
#include <databento/enums.hpp>

TEST(SchemaUtilsTest, SchemaToRTypeReturnsCorrectRType) {
    EXPECT_EQ(SchemaToRType(Schema::Trades), databento::RType::Mbp0);
    EXPECT_EQ(SchemaToRType(Schema::Mbp1), databento::RType::Mbp1);
    EXPECT_EQ(SchemaToRType(Schema::Definition), databento::RType::InstrumentDef);
}

TEST(SchemaUtilsTest, DatasetToSchemaNameConvertsCorrectly) {
    EXPECT_EQ(DatasetToSchemaName("OPRA.PILLAR"), "opra_pillar");
    EXPECT_EQ(DatasetToSchemaName("GLBX.MDP3"), "glbx_mdp3");
    EXPECT_EQ(DatasetToSchemaName("XNAS.ITCH"), "xnas_itch");
}
```

Update deps in `tests/BUILD.bazel`:
```python
cc_test(
    name = "schema_utils_test",
    srcs = ["schema_utils_test.cpp"],
    deps = [
        "//src:schema_utils",
        "@databento_cpp//:databento_headers",
        "@googletest//:gtest_main",
    ],
)
```

**Step 2: Run test to verify it passes**

Run: `bazel test //tests:schema_utils_test --test_output=errors`
Expected: PASS

**Step 3: Commit**

```bash
git add tests/schema_utils_test.cpp tests/BUILD.bazel
git commit -m "test: add SchemaToRType and DatasetToSchemaName tests"
```

---

## Phase 2: HTTP API Clients (Partial - Foundation)

### Task 2.1: SType Enum

**Files:**
- Create: `src/stype.hpp`
- Test: `tests/stype_test.cpp`
- Modify: `src/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Step 1: Write the failing test**

```cpp
// tests/stype_test.cpp
#include <gtest/gtest.h>
#include "src/stype.hpp"

using namespace dbn_pipe;

TEST(STypeTest, STypeFromStringParsesValidTypes) {
    EXPECT_EQ(STypeFromString("raw_symbol"), SType::RawSymbol);
    EXPECT_EQ(STypeFromString("instrument_id"), SType::InstrumentId);
    EXPECT_EQ(STypeFromString("parent"), SType::Parent);
}

TEST(STypeTest, STypeToStringReturnsCorrectString) {
    EXPECT_EQ(STypeToString(SType::RawSymbol), "raw_symbol");
    EXPECT_EQ(STypeToString(SType::InstrumentId), "instrument_id");
}
```

**Step 2: Run test to verify it fails**

Run: `bazel test //tests:stype_test --test_output=errors`
Expected: BUILD ERROR - file not found

**Step 3: Create stype.hpp**

```cpp
// src/stype.hpp
#pragma once

#include <optional>
#include <string_view>

namespace dbn_pipe {

// Symbology type enum
enum class SType {
    RawSymbol,      // Raw symbol string
    InstrumentId,   // Numeric instrument ID
    Parent,         // Parent symbol (e.g., "SPY.OPT" for all SPY options)
    Continuous,     // Continuous contract
    Smart           // Smart symbology
};

inline std::optional<SType> STypeFromString(std::string_view s) {
    if (s == "raw_symbol") return SType::RawSymbol;
    if (s == "instrument_id") return SType::InstrumentId;
    if (s == "parent") return SType::Parent;
    if (s == "continuous") return SType::Continuous;
    if (s == "smart") return SType::Smart;
    return std::nullopt;
}

inline std::string_view STypeToString(SType stype) {
    switch (stype) {
        case SType::RawSymbol: return "raw_symbol";
        case SType::InstrumentId: return "instrument_id";
        case SType::Parent: return "parent";
        case SType::Continuous: return "continuous";
        case SType::Smart: return "smart";
    }
    return "";
}

}  // namespace dbn_pipe
```

Add to `src/BUILD.bazel`:
```python
cc_library(
    name = "stype",
    hdrs = ["stype.hpp"],
    visibility = ["//visibility:public"],
)
```

Add to `tests/BUILD.bazel`:
```python
cc_test(
    name = "stype_test",
    srcs = ["stype_test.cpp"],
    deps = [
        "//src:stype",
        "@googletest//:gtest_main",
    ],
)
```

**Step 4: Run test to verify it passes**

Run: `bazel test //tests:stype_test --test_output=errors`
Expected: PASS

**Step 5: Commit**

```bash
git add src/stype.hpp src/BUILD.bazel tests/stype_test.cpp tests/BUILD.bazel
git commit -m "feat: add SType enum for symbology types"
```

---

## Final Task: Run All Tests

**Step 1: Run full test suite**

Run: `bazel test //tests/... --test_output=errors`
Expected: All tests PASS

**Step 2: Final commit with summary**

```bash
git add -A
git commit -m "feat: complete Phase 1, 3, 4 foundation for API feature parity

- TradingDate: date handling with America/New_York timezone
- IStorage/NoOpStorage: DI pattern for optional persistence
- InstrumentMap: date-interval symbol resolution for OPRA recycling
- RetryPolicy: exponential backoff with jitter
- Schema utilities: enum, string conversion, RType mapping
- SType: symbology type enum

See docs/plans/2026-01-19-api-feature-parity-design.md for full design."
```

---

## Summary

| Task | Component | Tests |
|------|-----------|-------|
| 1.1-1.3 | TradingDate | 6 tests |
| 1.4 | IStorage/NoOpStorage | 2 tests |
| 1.5-1.7 | InstrumentMap | 5 tests |
| 3.1-3.2 | RetryPolicy | 6 tests |
| 4.1-4.2 | schema_utils | 5 tests |
| 2.1 | SType | 2 tests |

**Total:** 26 tests across 6 components

**Remaining (Phase 2 HTTP, Phase 5 Batch/DBNStore):** Requires rapidjson dependency setup and more complex integration - continue in separate implementation session.
