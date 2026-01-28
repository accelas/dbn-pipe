// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>

namespace dbn_pipe {

// Format-agnostic logical column types.
// Each type carries its C++ representation type.
// Consumers implement ColumnBackend to map these to their format
// (e.g., PostgreSQL, Arrow/Parquet, CSV).

struct Int64     { using cpp_type = int64_t; };
struct Int32     { using cpp_type = int32_t; };
struct Int16     { using cpp_type = int16_t; };
struct Char      { using cpp_type = char; };
struct Timestamp { using cpp_type = int64_t; };  // unix nanoseconds
struct Text      { using cpp_type = std::string_view; };
struct Bool      { using cpp_type = bool; };
struct Float64   { using cpp_type = double; };

// Concept: a backend that can encode all logical types.
// Consumers implement this (e.g., PgBackend, ArrowBackend).
template <typename B>
concept ColumnBackend = requires(B& b) {
    { b.template encode<Int64>(int64_t{}) };
    { b.template encode<Int32>(int32_t{}) };
    { b.template encode<Int16>(int16_t{}) };
    { b.template encode<Char>(char{}) };
    { b.template encode<Timestamp>(int64_t{}) };
    { b.template encode<Text>(std::string_view{}) };
    { b.template encode<Bool>(bool{}) };
    { b.template encode<Float64>(double{}) };
};

}  // namespace dbn_pipe
