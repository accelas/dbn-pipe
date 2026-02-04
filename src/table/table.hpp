// SPDX-License-Identifier: MIT

#pragma once

#include "dbn_pipe/table/column_type.hpp"
#include "dbn_pipe/table/fixed_string.hpp"
#include <array>
#include <string_view>
#include <tuple>
#include <utility>

namespace dbn_pipe {

/// Format-agnostic column definition.
///
/// @tparam Name         Compile-time string used as the column name.
/// @tparam LogicalType  One of Int64, Int32, Int16, Char, Timestamp, Text, Bool, Float64.
template <FixedString Name, typename LogicalType>
struct Column {
    static constexpr auto name = Name;   ///< Column name as a FixedString.
    using type = LogicalType;            ///< The logical type tag.
    using cpp_type = typename LogicalType::cpp_type;  ///< Corresponding C++ value type.
};

/// Row storage with compile-time named column access.
///
/// @tparam Columns  Pack of Column instantiations that define the schema.
template <typename... Columns>
class Row {
public:
    /// Tuple holding one value per column.
    using Storage = std::tuple<typename Columns::cpp_type...>;

    /// Access a column value by name (mutable).
    /// @tparam Name  Column name as a FixedString literal.
    template <FixedString Name>
    auto& get() {
        return std::get<index_of<Name>()>(data_);
    }

    /// Access a column value by name (const).
    /// @tparam Name  Column name as a FixedString literal.
    template <FixedString Name>
    const auto& get() const {
        return std::get<index_of<Name>()>(data_);
    }

    /// @return Mutable reference to the underlying tuple.
    Storage& as_tuple() { return data_; }
    /// @return Const reference to the underlying tuple.
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

/// Format-agnostic, constexpr table definition.
///
/// A Table is a named, ordered collection of Column types that can be
/// instantiated at compile time.  It carries no backend coupling; storage
/// backends (DuckDB, PostgreSQL, Arrow, CSV) map logical types via the
/// ColumnBackend concept.
///
/// @tparam Columns  Pack of Column instantiations that define the schema.
template <typename... Columns>
class Table {
public:
    static constexpr std::size_t kColumnCount = sizeof...(Columns);  ///< Number of columns.

    /// Construct from a string literal table name and a pack of Column values.
    template <std::size_t N>
    constexpr Table(const char (&str)[N], Columns...)
        : name_(str, std::make_index_sequence<N-1>{}) {}

    /// @return The table name.
    constexpr std::string_view name() const { return name_.view(); }
    /// @return Number of columns.
    constexpr std::size_t column_count() const { return kColumnCount; }

    /// @return An array of column name string_views.
    std::array<std::string_view, kColumnCount> column_names() const {
        return {Columns::name.view()...};
    }

    using RowType = Row<Columns...>;              ///< Row type for this table.
    using ColumnsTuple = std::tuple<Columns...>;  ///< Tuple of Column types.

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

/// Deduction guide: deduce Column types from constructor arguments.
template <std::size_t N, typename... Columns>
Table(const char (&)[N], Columns...) -> Table<Columns...>;

}  // namespace dbn_pipe
