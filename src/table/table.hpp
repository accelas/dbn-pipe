// SPDX-License-Identifier: MIT

#pragma once

#include "src/table/column_type.hpp"
#include "src/table/fixed_string.hpp"
#include <array>
#include <string_view>
#include <tuple>
#include <utility>

namespace dbn_pipe {

// Format-agnostic column definition.
// Name: compile-time string (column name in the table)
// LogicalType: one of Int64, Int32, Int16, Char, Timestamp, Text, Bool, Float64
template <FixedString Name, typename LogicalType>
struct Column {
    static constexpr auto name = Name;
    using type = LogicalType;
    using cpp_type = typename LogicalType::cpp_type;
};

// Row storage with named access
template <typename... Columns>
class Row {
public:
    using Storage = std::tuple<typename Columns::cpp_type...>;

    template <FixedString Name>
    auto& get() {
        return std::get<index_of<Name>()>(data_);
    }

    template <FixedString Name>
    const auto& get() const {
        return std::get<index_of<Name>()>(data_);
    }

    Storage& as_tuple() { return data_; }
    const Storage& as_tuple() const { return data_; }

private:
    template <FixedString Name, std::size_t I = 0>
    static constexpr std::size_t index_of() {
        if constexpr (I >= sizeof...(Columns)) {
            static_assert(I < sizeof...(Columns), "Column not found");
            return I;
        } else {
            using Col = std::tuple_element_t<I, std::tuple<Columns...>>;
            if constexpr (Col::name.view() == Name.view()) {
                return I;
            } else {
                return index_of<Name, I + 1>();
            }
        }
    }

    Storage data_{};
};

// Table definition - format-agnostic, no PG coupling
template <typename... Columns>
class Table {
public:
    static constexpr std::size_t kColumnCount = sizeof...(Columns);

    template <std::size_t N>
    constexpr Table(const char (&str)[N], Columns...)
        : name_(str, std::make_index_sequence<N-1>{}) {}

    constexpr std::string_view name() const { return name_.view(); }
    constexpr std::size_t column_count() const { return kColumnCount; }

    std::array<std::string_view, kColumnCount> column_names() const {
        return {Columns::name.view()...};
    }

    using RowType = Row<Columns...>;
    using ColumnsTuple = std::tuple<Columns...>;

private:
    static constexpr std::size_t kMaxNameSize = 128;

    struct NameStorage {
        char data[kMaxNameSize + 1]{};
        std::size_t len{};

        template <std::size_t N, std::size_t... Is>
        constexpr NameStorage(const char (&str)[N], std::index_sequence<Is...>)
            : data{str[Is]..., '\0'}, len{N - 1} {}

        constexpr NameStorage() = default;

        constexpr std::string_view view() const { return {data, len}; }
    };

    NameStorage name_;
};

// Deduction guide
template <std::size_t N, typename... Columns>
Table(const char (&)[N], Columns...) -> Table<Columns...>;

}  // namespace dbn_pipe
