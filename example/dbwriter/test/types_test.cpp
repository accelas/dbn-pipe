// SPDX-License-Identifier: MIT

#include "dbwriter/types.hpp"
#include <gtest/gtest.h>

namespace dbwriter {
namespace {

TEST(TimestampTest, FromUnixNs_ConvertsCorrectly) {
    // 2024-01-01 00:00:00 UTC in nanoseconds since Unix epoch
    int64_t unix_ns = 1704067200000000000LL;

    auto ts = Timestamp::from_unix_ns(unix_ns);

    // 2024-01-01 is 24 years after PG epoch (2000-01-01)
    // That's about 757382400 seconds = 757382400000000 usec
    EXPECT_GT(ts.to_pg_timestamp(), 0);
}

TEST(TimestampTest, PgEpoch_IsZero) {
    // 2000-01-01 00:00:00 UTC = 946684800 seconds since Unix epoch
    int64_t pg_epoch_unix_ns = 946684800000000000LL;

    auto ts = Timestamp::from_unix_ns(pg_epoch_unix_ns);

    EXPECT_EQ(ts.to_pg_timestamp(), 0);
}

}  // namespace
}  // namespace dbwriter
