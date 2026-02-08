// SPDX-License-Identifier: MIT

#include "dbn_pipe/to_nanos.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono;
using namespace std::chrono_literals;
using dbn_pipe::to_nanos;
using dbn_pipe::UnixNanos;

// 2024-01-01 00:00:00 UTC = 1704067200 seconds since epoch
static constexpr uint64_t kJan1_2024_UTC_nanos = 1704067200'000'000'000ULL;

// America/New_York is UTC-5 in winter (EST)
// So midnight NY = 05:00 UTC
static constexpr uint64_t kJan1_2024_NY_midnight_nanos =
    kJan1_2024_UTC_nanos + 5ULL * 3600 * 1'000'000'000;

// --- to_nanos free function tests ---

TEST(ToNanosTest, CalendarDateDefaultNY) {
    auto result = to_nanos(2024y / January / 1);
    EXPECT_EQ(result, kJan1_2024_NY_midnight_nanos);
}

TEST(ToNanosTest, CalendarDateExplicitNY) {
    auto result = to_nanos(2024y / January / 1, "America/New_York");
    EXPECT_EQ(result, kJan1_2024_NY_midnight_nanos);
}

TEST(ToNanosTest, CalendarDateUTC) {
    auto result = to_nanos(2024y / January / 1, "UTC");
    EXPECT_EQ(result, kJan1_2024_UTC_nanos);
}

TEST(ToNanosTest, CalendarDateChicago) {
    // America/Chicago is UTC-6 in winter (CST)
    // Midnight Chicago = 06:00 UTC
    uint64_t expected = kJan1_2024_UTC_nanos + 6ULL * 3600 * 1'000'000'000;
    auto result = to_nanos(2024y / January / 1, "America/Chicago");
    EXPECT_EQ(result, expected);
}

TEST(ToNanosTest, DSTSummerNY) {
    // 2024-07-01: America/New_York is UTC-4 (EDT)
    // Midnight NY = 04:00 UTC
    // 2024-07-01 00:00:00 UTC = 1719792000 seconds since epoch
    uint64_t jul1_utc = 1719792000'000'000'000ULL;
    uint64_t expected = jul1_utc + 4ULL * 3600 * 1'000'000'000;
    auto result = to_nanos(2024y / July / 1);
    EXPECT_EQ(result, expected);
}

TEST(ToNanosTest, LocalTimeWithSubDayPrecision) {
    // 2024-01-01 09:30:00 NY (market open) = 14:30:00 UTC
    auto local_tp = local_days{2024y / January / 1} + 9h + 30min;
    auto result = to_nanos(local_tp, "America/New_York");

    uint64_t expected = kJan1_2024_UTC_nanos +
                        14ULL * 3600 * 1'000'000'000 +
                        30ULL * 60 * 1'000'000'000;
    EXPECT_EQ(result, expected);
}

TEST(ToNanosTest, SysTimePassthrough) {
    // UTC time point directly — no timezone conversion
    auto sys_tp = sys_days{2024y / January / 1};
    auto result = to_nanos(sys_tp);
    EXPECT_EQ(result, kJan1_2024_UTC_nanos);
}

TEST(ToNanosTest, SysTimeWithPrecision) {
    auto sys_tp = sys_days{2024y / January / 1} + 12h + 30min;
    auto result = to_nanos(sys_tp);
    uint64_t expected = kJan1_2024_UTC_nanos +
                        12ULL * 3600 * 1'000'000'000 +
                        30ULL * 60 * 1'000'000'000;
    EXPECT_EQ(result, expected);
}

// utc_clock overload is deleted — calling to_nanos(utc_time<...>) won't compile.
// Uncomment to verify:
//   to_nanos(utc_clock::now());  // error: use of deleted function

// --- UnixNanos type tests ---

TEST(TimestampTest, FromRawNanos) {
    UnixNanos ts = 1704067200'000'000'000ULL;
    EXPECT_EQ(uint64_t(ts), kJan1_2024_UTC_nanos);
}

TEST(TimestampTest, FromCalendarDateDefaultNY) {
    UnixNanos ts = 2024y / January / 1;
    EXPECT_EQ(uint64_t(ts), kJan1_2024_NY_midnight_nanos);
}

TEST(TimestampTest, FromCalendarDateWithTimezone) {
    UnixNanos ts{2024y / January / 1, "America/Chicago"};
    uint64_t expected = kJan1_2024_UTC_nanos + 6ULL * 3600 * 1'000'000'000;
    EXPECT_EQ(uint64_t(ts), expected);
}

TEST(TimestampTest, FromLocalTime) {
    // 9:30 AM NY = 14:30 UTC
    UnixNanos ts = local_days{2024y / January / 1} + 9h + 30min;
    uint64_t expected = kJan1_2024_UTC_nanos +
                        14ULL * 3600 * 1'000'000'000 +
                        30ULL * 60 * 1'000'000'000;
    EXPECT_EQ(uint64_t(ts), expected);
}

TEST(TimestampTest, FromSysTime) {
    UnixNanos ts = sys_days{2024y / January / 1};
    EXPECT_EQ(uint64_t(ts), kJan1_2024_UTC_nanos);
}

TEST(TimestampTest, ImplicitConversionToUint64) {
    UnixNanos ts = 42ULL;
    uint64_t val = ts;  // implicit conversion
    EXPECT_EQ(val, 42ULL);
}

TEST(TimestampTest, DesignatedInitializerStyle) {
    // Simulates how it looks in HistoricalRequest
    struct FakeRequest {
        UnixNanos start;
        UnixNanos end;
    };

    FakeRequest req{
        .start = 2024y / January / 1,
        .end = {2024y / January / 2, "America/Chicago"},
    };

    EXPECT_EQ(uint64_t(req.start), kJan1_2024_NY_midnight_nanos);
    uint64_t expected_end = kJan1_2024_UTC_nanos +
                            24ULL * 3600 * 1'000'000'000 +  // Jan 2
                            6ULL * 3600 * 1'000'000'000;    // CST offset
    EXPECT_EQ(uint64_t(req.end), expected_end);
}
