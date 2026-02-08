// SPDX-License-Identifier: MIT

#include "dbn_pipe/table/column_type.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe {
namespace {

// Verify cpp_type aliases
TEST(ColumnTypeTest, CppTypes) {
    static_assert(std::is_same_v<Int64::cpp_type, int64_t>);
    static_assert(std::is_same_v<Int32::cpp_type, int32_t>);
    static_assert(std::is_same_v<Int16::cpp_type, int16_t>);
    static_assert(std::is_same_v<Char::cpp_type, char>);
    static_assert(std::is_same_v<TimestampCol::cpp_type, int64_t>);
    static_assert(std::is_same_v<Text::cpp_type, std::string_view>);
    static_assert(std::is_same_v<Bool::cpp_type, bool>);
    static_assert(std::is_same_v<Float64::cpp_type, double>);
    SUCCEED();
}

// A minimal mock backend that satisfies ColumnBackend
struct MockBackend {
    template <typename T>
    void encode(typename T::cpp_type) {}
};

TEST(ColumnTypeTest, BackendConcept) {
    static_assert(ColumnBackend<MockBackend>);
    SUCCEED();
}

// A non-backend should not satisfy the concept
struct NotABackend {};

TEST(ColumnTypeTest, NonBackendDoesNotSatisfy) {
    static_assert(!ColumnBackend<NotABackend>);
    SUCCEED();
}

}  // namespace
}  // namespace dbn_pipe
