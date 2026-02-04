// SPDX-License-Identifier: MIT

// src/trading_date.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dbn_pipe {

/// Calendar date for tracking symbol-mapping intervals.
///
/// Used by InstrumentMap to manage date intervals for OPRA options
/// where instrument_ids are recycled daily.
///
/// Thread safety: value type, safe to copy and use across threads.
class TradingDate {
public:
    /// Parse an ISO-8601 date string ("YYYY-MM-DD").
    /// @param iso_date  String in "YYYY-MM-DD" format.
    /// @return A TradingDate representing the parsed date.
    /// @throws std::invalid_argument if the format is invalid.
    static TradingDate FromIsoString(std::string_view iso_date) {
        // Validate exact format: 10 chars, YYYY-MM-DD
        if (iso_date.size() != 10) {
            throw std::invalid_argument(
                "Invalid ISO date (expected YYYY-MM-DD): " + std::string(iso_date));
        }

        std::chrono::year_month_day ymd;
        std::istringstream ss{std::string(iso_date)};
        ss >> std::chrono::parse("%F", ymd);  // %F = YYYY-MM-DD

        if (ss.fail() || !ymd.ok()) {
            throw std::invalid_argument(
                "Invalid ISO date (expected YYYY-MM-DD): " + std::string(iso_date));
        }

        return TradingDate(
            static_cast<int>(ymd.year()),
            static_cast<unsigned>(ymd.month()),
            static_cast<unsigned>(ymd.day()));
    }

    /// Convert nanoseconds since Unix epoch (UTC) to a trading date in the
    /// specified timezone.  Uses C++20 std::chrono::zoned_time for proper
    /// DST handling.
    ///
    /// @param ns_since_epoch  Nanoseconds since 1970-01-01T00:00:00 UTC.
    /// @param timezone        IANA timezone name (e.g. "America/New_York", "UTC").
    /// @return A TradingDate in the local calendar of @p timezone.
    static TradingDate FromNanoseconds(uint64_t ns_since_epoch, std::string_view timezone) {
        // Convert nanoseconds to sys_time (UTC)
        auto sys_time = std::chrono::sys_time<std::chrono::nanoseconds>{
            std::chrono::nanoseconds{ns_since_epoch}};

        // Convert to local time in specified timezone
        std::chrono::zoned_time zt{timezone, sys_time};
        auto local_time = zt.get_local_time();

        // Extract local days and convert to date
        auto local_days = std::chrono::floor<std::chrono::days>(local_time);
        std::chrono::year_month_day ymd{local_days};

        return TradingDate(
            static_cast<int>(ymd.year()),
            static_cast<unsigned>(ymd.month()),
            static_cast<unsigned>(ymd.day()));
    }

    /// @return Calendar year.
    int Year() const { return year_; }
    /// @return Calendar month (1--12).
    int Month() const { return month_; }
    /// @return Calendar day (1--31).
    int Day() const { return day_; }

    /// Format the date as an ISO-8601 string ("YYYY-MM-DD").
    /// @return The formatted date string.
    std::string ToIsoString() const {
        char buf[20];  // Worst case: "-YYYY-MM-DD" with negative values + null
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      static_cast<int>(year_), static_cast<int>(month_), static_cast<int>(day_));
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

private:
    TradingDate(int year, int month, int day)
        : year_(year), month_(month), day_(day) {}

    // Convert days since 1970-01-01 to year/month/day.
    // Algorithm from Howard Hinnant's date library (public domain).
    static TradingDate FromDaysSinceEpoch(int32_t days) {
        days += 719468;  // Shift to March 1, 0000
        int32_t era = (days >= 0 ? days : days - 146096) / 146097;
        int32_t doe = days - era * 146097;  // day of era [0, 146096]
        int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // year of era
        int32_t y = yoe + era * 400;
        int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // day of year [0, 365]
        int32_t mp = (5 * doy + 2) / 153;  // month in Mar-Feb cycle [0, 11]
        int32_t d = doy - (153 * mp + 2) / 5 + 1;  // day [1, 31]
        int32_t m = mp + (mp < 10 ? 3 : -9);  // month [1, 12]
        y += (m <= 2);  // Adjust year for Jan/Feb
        return TradingDate(y, m, d);
    }

    int16_t year_;
    int8_t month_;
    int8_t day_;
};

}  // namespace dbn_pipe
