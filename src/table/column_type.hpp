// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>

namespace dbn_pipe {

/// @name Logical column types
/// Format-agnostic logical column types.  Each type carries a @c cpp_type
/// alias for its C++ representation.  Consumers implement ColumnBackend to
/// map these to a concrete format (e.g. PostgreSQL, Arrow/Parquet, CSV).
/// @{
struct Int64     { using cpp_type = int64_t; };           ///< 64-bit signed integer.
struct Int32     { using cpp_type = int32_t; };           ///< 32-bit signed integer.
struct Int16     { using cpp_type = int16_t; };           ///< 16-bit signed integer.
struct Char      { using cpp_type = char; };              ///< Single ASCII character.
struct TimestampCol { using cpp_type = int64_t; };           ///< Unix timestamp in nanoseconds.
struct Text      { using cpp_type = std::string_view; };  ///< Variable-length UTF-8 text.
struct Bool      { using cpp_type = bool; };              ///< Boolean value.
struct Float64   { using cpp_type = double; };            ///< 64-bit IEEE 754 floating point.
/// @}

/// Concept for a backend that can encode every logical column type.
///
/// Consumers implement this (e.g. PgBackend, ArrowBackend) to convert
/// logical values into their storage-specific representation.
template <typename B>
concept ColumnBackend = requires(B& b) {
    { b.template encode<Int64>(int64_t{}) };
    { b.template encode<Int32>(int32_t{}) };
    { b.template encode<Int16>(int16_t{}) };
    { b.template encode<Char>(char{}) };
    { b.template encode<TimestampCol>(int64_t{}) };
    { b.template encode<Text>(std::string_view{}) };
    { b.template encode<Bool>(bool{}) };
    { b.template encode<Float64>(double{}) };
};

}  // namespace dbn_pipe
