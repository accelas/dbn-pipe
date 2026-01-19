// src/trading_date.hpp
#pragma once

#include <cstdint>
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

private:
    TradingDate(int year, int month, int day)
        : year_(year), month_(month), day_(day) {}

    int16_t year_;
    int8_t month_;
    int8_t day_;
};

}  // namespace dbn_pipe
