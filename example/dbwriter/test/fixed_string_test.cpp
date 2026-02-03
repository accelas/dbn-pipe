// SPDX-License-Identifier: MIT

#include "dbn_pipe/table/fixed_string.hpp"
#include <gtest/gtest.h>

namespace dbn_pipe {
namespace {

TEST(FixedStringTest, ConstructFromLiteral) {
    constexpr FixedString<5> fs{"hello"};
    EXPECT_EQ(fs.view(), "hello");
}

TEST(FixedStringTest, CompileTimeComparison) {
    constexpr FixedString<3> a{"foo"};
    constexpr FixedString<3> b{"foo"};
    constexpr FixedString<3> c{"bar"};

    static_assert(a == b);
    static_assert(a != c);

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(FixedStringTest, Size) {
    constexpr FixedString<5> fs{"hello"};
    static_assert(fs.size() == 5);
    EXPECT_EQ(fs.size(), 5);
}

TEST(FixedStringTest, UsableAsTemplateParameter) {
    // This compiles only if FixedString is a valid NTTP
    constexpr auto fs = FixedString{"test"};

    auto get_name = []<FixedString Name>() {
        return Name.view();
    };

    EXPECT_EQ(get_name.template operator()<fs>(), "test");
}

}  // namespace
}  // namespace dbn_pipe
