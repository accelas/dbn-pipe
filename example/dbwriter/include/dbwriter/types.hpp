// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

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

// Byte buffer for binary COPY data
class ByteBuffer {
public:
    void put_int16_be(int16_t val);
    void put_int32_be(int32_t val);
    void put_int64_be(int64_t val);
    void put_byte(std::byte b);
    void put_bytes(std::span<const std::byte> data);

    std::span<const std::byte> view() const { return data_; }
    void clear() { data_.clear(); }
    size_t size() const { return data_.size(); }

private:
    std::vector<std::byte> data_;
};

}  // namespace dbwriter
