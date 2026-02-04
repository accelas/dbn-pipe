// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <string_view>

namespace dbn_pipe {

/// Compile-time fixed-capacity string for use as a non-type template parameter.
///
/// @tparam N  Fixed capacity in bytes (not including the null terminator).
template <std::size_t N>
struct FixedString {
    char data[N + 1]{};  ///< Null-terminated character storage.

    /// Construct from a string literal.
    constexpr FixedString(const char (&str)[N + 1]) {
        std::copy_n(str, N + 1, data);
    }

    /// @return A string_view over the stored characters.
    constexpr std::string_view view() const { return {data, N}; }
    /// @return The fixed capacity in bytes.
    constexpr std::size_t size() const { return N; }

    /// Equality comparison across different capacities.
    template <std::size_t M>
    constexpr bool operator==(const FixedString<M>& other) const {
        return view() == other.view();
    }

    /// Three-way comparison across different capacities.
    template <std::size_t M>
    constexpr auto operator<=>(const FixedString<M>& other) const {
        return view() <=> other.view();
    }
};

/// Deduction guide: deduce N from a string literal (subtracting the null terminator).
template <std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N - 1>;

}  // namespace dbn_pipe
