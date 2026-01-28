// SPDX-License-Identifier: MIT

#include "src/table/fixed_string.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe {
namespace {

TEST(FixedStringTest, StoresString) {
    constexpr FixedString fs("hello");
    EXPECT_EQ(fs.view(), "hello");
    EXPECT_EQ(fs.size(), 5);
}

TEST(FixedStringTest, Equality) {
    constexpr FixedString a("foo");
    constexpr FixedString b("foo");
    constexpr FixedString c("bar");

    static_assert(a == b);
    static_assert(!(a == c));
}

TEST(FixedStringTest, Ordering) {
    constexpr FixedString a("alpha");
    constexpr FixedString b("beta");

    static_assert(a < b);
}

TEST(FixedStringTest, DeductionGuide) {
    constexpr FixedString fs("test");
    static_assert(fs.size() == 4);
}

}  // namespace
}  // namespace dbn_pipe
