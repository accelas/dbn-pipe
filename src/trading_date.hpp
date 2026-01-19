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

    int16_t year_;
    int8_t month_;
    int8_t day_;
};

}  // namespace dbn_pipe
