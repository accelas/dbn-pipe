// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace dbwriter {

// Timestamp with PostgreSQL epoch (2000-01-01)
struct Timestamp {
    int64_t usec_since_pg_epoch;

    // Convert from Unix nanoseconds to PostgreSQL microseconds
    static Timestamp from_unix_ns(int64_t ns) {
        // Unix epoch: 1970-01-01
        // PG epoch: 2000-01-01
        // Difference: 946684800 seconds = 946684800000000 microseconds
        constexpr int64_t kPgEpochOffsetUsec = 946684800000000LL;
        return Timestamp{ns / 1000 - kPgEpochOffsetUsec};
    }

    int64_t to_pg_timestamp() const { return usec_since_pg_epoch; }
};

}  // namespace dbwriter
