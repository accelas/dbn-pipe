// src/trading_date.hpp
#pragma once

#include <cstdint>
#include <cstdio>
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
        int year = 0, month = 0, day = 0;
        if (iso_date.size() >= 10) {
            year = (iso_date[0] - '0') * 1000 + (iso_date[1] - '0') * 100 +
                   (iso_date[2] - '0') * 10 + (iso_date[3] - '0');
            month = (iso_date[5] - '0') * 10 + (iso_date[6] - '0');
            day = (iso_date[8] - '0') * 10 + (iso_date[9] - '0');
        }
        return TradingDate(year, month, day);
    }

    // Convert nanoseconds since Unix epoch (UTC) to trading date in America/New_York.
    // Note: Uses fixed EST offset (-5 hours). For production with DST handling,
    // consider using Howard Hinnant's date library or system tzdb.
    static TradingDate FromNanoseconds(uint64_t ns_since_epoch) {
        // Convert to seconds
        int64_t seconds = static_cast<int64_t>(ns_since_epoch / 1000000000ULL);

        // Apply EST offset (-5 hours = -18000 seconds)
        constexpr int64_t kEstOffsetSeconds = -5 * 3600;
        seconds += kEstOffsetSeconds;

        // Convert seconds since epoch to days (floor division)
        int64_t days = seconds / 86400;
        if (seconds < 0 && seconds % 86400 != 0) {
            days--;  // Floor division for negative values
        }

        return FromDaysSinceEpoch(static_cast<int32_t>(days));
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
