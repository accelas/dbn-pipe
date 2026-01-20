// src/trading_date.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dbn_pipe {

// TradingDate represents a calendar date for tracking symbol mappings.
// Used by InstrumentMap to manage date intervals for OPRA options
// where instrument_ids are recycled daily.
//
// Thread safety: Value type, safe to copy and use across threads.
class TradingDate {
public:
    // Parse ISO-8601 date string "YYYY-MM-DD"
    static TradingDate FromIsoString(std::string_view iso_date) {
        auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
        if (iso_date.size() != 10 || iso_date[4] != '-' || iso_date[7] != '-') {
            throw std::invalid_argument(
                "Invalid ISO date format (expected YYYY-MM-DD): " + std::string(iso_date));
        }
        for (size_t i : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
            if (!is_digit(iso_date[i])) {
                throw std::invalid_argument(
                    "Invalid ISO date format (expected YYYY-MM-DD): " + std::string(iso_date));
            }
        }

        int year = (iso_date[0] - '0') * 1000 + (iso_date[1] - '0') * 100 +
                   (iso_date[2] - '0') * 10 + (iso_date[3] - '0');
        int month = (iso_date[5] - '0') * 10 + (iso_date[6] - '0');
        int day = (iso_date[8] - '0') * 10 + (iso_date[9] - '0');

        std::chrono::year_month_day ymd{
            std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
            std::chrono::day{static_cast<unsigned>(day)}};
        if (!ymd.ok()) {
            throw std::invalid_argument("Invalid calendar date: " + std::string(iso_date));
        }

        return TradingDate(year, month, day);
    }

    // Convert nanoseconds since Unix epoch (UTC) to trading date in specified timezone.
    // Uses C++20 std::chrono::zoned_time for proper DST handling.
    //
    // Example timezones: "America/New_York", "America/Chicago", "UTC"
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

    int Year() const { return year_; }
    int Month() const { return month_; }
    int Day() const { return day_; }

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
