// SPDX-License-Identifier: MIT

#pragma once

#include "dbwriter/types.hpp"
#include <cstdint>

namespace dbwriter::pg {

// PostgreSQL BIGINT (8 bytes)
struct BigInt {
    static constexpr const char* pg_type_name() { return "BIGINT"; }
    static void encode(int64_t val, ByteBuffer& buf) {
        buf.put_int32_be(8);       // field length
        buf.put_int64_be(val);     // value in network byte order
    }
};

// PostgreSQL INTEGER (4 bytes)
struct Integer {
    static constexpr const char* pg_type_name() { return "INTEGER"; }
    static void encode(int32_t val, ByteBuffer& buf) {
        buf.put_int32_be(4);
        buf.put_int32_be(val);
    }
};

// PostgreSQL SMALLINT (2 bytes)
struct SmallInt {
    static constexpr const char* pg_type_name() { return "SMALLINT"; }
    static void encode(int16_t val, ByteBuffer& buf) {
        buf.put_int32_be(2);
        buf.put_int16_be(val);
    }
};

// PostgreSQL CHAR(1) (1 byte)
struct Char {
    static constexpr const char* pg_type_name() { return "CHAR(1)"; }
    static void encode(char val, ByteBuffer& buf) {
        buf.put_int32_be(1);
        buf.put_byte(static_cast<std::byte>(val));
    }
};

// PostgreSQL TIMESTAMPTZ (8 bytes, microseconds since 2000-01-01)
struct Timestamptz {
    static constexpr const char* pg_type_name() { return "TIMESTAMPTZ"; }
    static void encode(Timestamp val, ByteBuffer& buf) {
        buf.put_int32_be(8);
        buf.put_int64_be(val.to_pg_timestamp());
    }
};

// PostgreSQL NULL (-1 length)
struct Null {
    static constexpr const char* pg_type_name() { return "NULL"; }
    static void encode(ByteBuffer& buf) {
        buf.put_int32_be(-1);
    }
};

// PostgreSQL TEXT (variable length)
struct Text {
    static constexpr const char* pg_type_name() { return "TEXT"; }
    static void encode(std::string_view val, ByteBuffer& buf) {
        buf.put_int32_be(static_cast<int32_t>(val.size()));
        buf.put_bytes({reinterpret_cast<const std::byte*>(val.data()), val.size()});
    }
};

}  // namespace dbwriter::pg
