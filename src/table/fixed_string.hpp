// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <string_view>

namespace dbn_pipe {

// Compile-time string for use as template parameter
template <std::size_t N>
struct FixedString {
    char data[N + 1]{};

    constexpr FixedString(const char (&str)[N + 1]) {
        std::copy_n(str, N + 1, data);
    }

    constexpr std::string_view view() const { return {data, N}; }
    constexpr std::size_t size() const { return N; }

    template <std::size_t M>
    constexpr bool operator==(const FixedString<M>& other) const {
        return view() == other.view();
    }

    template <std::size_t M>
    constexpr auto operator<=>(const FixedString<M>& other) const {
        return view() <=> other.view();
    }
};

// Deduction guide
template <std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N - 1>;

}  // namespace dbn_pipe
