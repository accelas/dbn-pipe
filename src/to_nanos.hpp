// SPDX-License-Identifier: MIT

// src/to_nanos.hpp
#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace dbn_pipe {

/// Default timezone for to_nanos() and UnixNanos conversions.
inline constexpr const char* kDefaultTimezone = "America/New_York";

/// Convert a calendar date at local midnight to UTC nanoseconds.
///
/// Uses zoned_time for correct DST and timezone offset handling.
///
/// @param ymd  Calendar date (asserts ymd.ok())
/// @param tz   IANA timezone name
/// @return Nanoseconds since Unix epoch
inline uint64_t to_nanos(std::chrono::year_month_day ymd,
                         std::string_view tz = kDefaultTimezone) {
    assert(ymd.ok() && "to_nanos: invalid year_month_day");
    auto local_midnight = std::chrono::local_days{ymd};
    std::chrono::zoned_time zt{tz, local_midnight};
    auto sys = zt.get_sys_time();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            sys.time_since_epoch())
            .count());
}

/// Convert a local time point to UTC nanoseconds.
///
/// @param lt  Local time with arbitrary precision
/// @param tz  IANA timezone name
/// @return Nanoseconds since Unix epoch
template <typename Duration>
uint64_t to_nanos(std::chrono::local_time<Duration> lt,
                  std::string_view tz = kDefaultTimezone) {
    std::chrono::zoned_time zt{tz, lt};
    auto sys = zt.get_sys_time();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            sys.time_since_epoch())
            .count());
}

/// Convert a UTC time point directly (no timezone conversion).
///
/// @param tp  System clock time point
/// @return Nanoseconds since Unix epoch
template <typename Duration>
uint64_t to_nanos(std::chrono::sys_time<Duration> tp) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            tp.time_since_epoch())
            .count());
}

/// @cond
/// Deleted: utc_clock includes leap seconds, incompatible with Unix time.
template <typename Duration>
uint64_t to_nanos(std::chrono::utc_time<Duration>) = delete;
/// @endcond

/// Nanoseconds since Unix epoch, implicitly constructible from chrono types.
///
/// Wraps uint64_t with converting constructors that handle timezone
/// conversion via zoned_time. Default timezone: America/New_York.
///
/// @code
/// .start = 2024y / January / 1                           // NY midnight
/// .start = {2024y / January / 1, "America/Chicago"}      // Chicago midnight
/// .start = local_days{2024y/January/1} + 9h + 30min      // NY 9:30 AM
/// .start = sys_days{2024y / January / 1}                  // UTC midnight
/// .start = 1704067200000000000ULL                         // raw nanos
/// @endcode
struct UnixNanos {
    uint64_t nanos = 0;  ///< Nanoseconds since Unix epoch

    constexpr UnixNanos() = default;

    /// Construct from raw nanoseconds.
    constexpr UnixNanos(uint64_t ns) : nanos(ns) {}

    /// Construct from a calendar date at local midnight.
    /// @param ymd  Calendar date (asserts ymd.ok())
    /// @param tz   IANA timezone name
    UnixNanos(std::chrono::year_month_day ymd,
              std::string_view tz = kDefaultTimezone)
        : nanos(to_nanos(ymd, tz)) {}

    /// Construct from a local time with sub-day precision.
    /// @param lt  Local time point
    /// @param tz  IANA timezone name
    template <typename Duration>
    UnixNanos(std::chrono::local_time<Duration> lt,
              std::string_view tz = kDefaultTimezone)
        : nanos(to_nanos(lt, tz)) {}

    /// Construct from a UTC time point (no timezone conversion).
    template <typename Duration>
    UnixNanos(std::chrono::sys_time<Duration> tp)
        : nanos(to_nanos(tp)) {}

    /// @cond
    template <typename Duration>
    UnixNanos(std::chrono::utc_time<Duration>) = delete;
    /// @endcond

    /// Implicit conversion to uint64_t for use in QueryParam, comparisons, etc.
    constexpr operator uint64_t() const { return nanos; }
};

}  // namespace dbn_pipe
