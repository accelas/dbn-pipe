// SPDX-License-Identifier: MIT

#pragma once

#include "dbwriter/types.hpp"
#include "src/table/column_type.hpp"
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
    // Encode from unix nanoseconds (dbn_pipe::Timestamp::cpp_type)
    static void encode(int64_t unix_ns, ByteBuffer& buf) {
        constexpr int64_t kPgEpochOffsetUsec = 946684800000000LL;
        int64_t pg_usec = unix_ns / 1000 - kPgEpochOffsetUsec;
        buf.put_int32_be(8);
        buf.put_int64_be(pg_usec);
    }
    // Legacy overload: encode from dbwriter::Timestamp (already PG epoch)
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

// Map dbn_pipe logical types to PG encoding types.
template <typename LogicalType>
struct PgTypeFor;

template <> struct PgTypeFor<dbn_pipe::Int64>     { using type = BigInt;      static constexpr const char* name() { return "BIGINT"; } };
template <> struct PgTypeFor<dbn_pipe::Int32>     { using type = Integer;     static constexpr const char* name() { return "INTEGER"; } };
template <> struct PgTypeFor<dbn_pipe::Int16>     { using type = SmallInt;    static constexpr const char* name() { return "SMALLINT"; } };
template <> struct PgTypeFor<dbn_pipe::Char>      { using type = Char;        static constexpr const char* name() { return "CHAR(1)"; } };
template <> struct PgTypeFor<dbn_pipe::Timestamp> { using type = Timestamptz; static constexpr const char* name() { return "TIMESTAMPTZ"; } };
template <> struct PgTypeFor<dbn_pipe::Text>      { using type = Text;        static constexpr const char* name() { return "TEXT"; } };

}  // namespace dbwriter::pg
