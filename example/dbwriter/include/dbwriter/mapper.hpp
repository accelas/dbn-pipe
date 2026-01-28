// SPDX-License-Identifier: MIT

#pragma once

#include "dbwriter/pg_types.hpp"
#include "dbwriter/types.hpp"
#include "src/table/table.hpp"
#include <tuple>

namespace dbwriter {

// dbn_pipe columns expose ::type (logical type); legacy dbwriter columns expose ::pg_type.
template <typename C>
concept LogicalColumn = requires { typename C::type; };

template <typename Table>
class Mapper {
public:
    using RowType = typename Table::RowType;

    explicit Mapper(const Table&) {}

    void encode_row(const RowType& row, ByteBuffer& buf) const {
        // Field count (int16_t)
        buf.put_int16_be(static_cast<int16_t>(Table::kColumnCount));

        // Encode each field
        encode_fields(row, buf, std::make_index_sequence<Table::kColumnCount>{});
    }

    void write_copy_header(ByteBuffer& buf) const {
        // Magic: "PGCOPY\n\377\r\n\0"
        static constexpr std::byte magic[] = {
            std::byte{'P'}, std::byte{'G'}, std::byte{'C'}, std::byte{'O'},
            std::byte{'P'}, std::byte{'Y'}, std::byte{'\n'}, std::byte{0xFF},
            std::byte{'\r'}, std::byte{'\n'}, std::byte{0x00}
        };
        buf.put_bytes(magic);

        // Flags (4 bytes, 0 for no OIDs)
        buf.put_int32_be(0);

        // Extension area length (4 bytes, 0)
        buf.put_int32_be(0);
    }

    void write_copy_trailer(ByteBuffer& buf) const {
        // -1 as int16_t signals end of data
        buf.put_int16_be(-1);
    }

private:
    template <std::size_t... Is>
    void encode_fields(const RowType& row, ByteBuffer& buf,
                       std::index_sequence<Is...>) const {
        (encode_field<Is>(row, buf), ...);
    }

    template <std::size_t I>
    void encode_field(const RowType& row, ByteBuffer& buf) const {
        using ColumnsTuple = typename Table::ColumnsTuple;
        using Column = std::tuple_element_t<I, ColumnsTuple>;

        const auto& value = std::get<I>(row.as_tuple());

        if constexpr (LogicalColumn<Column>) {
            // dbn_pipe Column: map logical type to PG encoder
            using PgType = typename pg::PgTypeFor<typename Column::type>::type;
            PgType::encode(value, buf);
        } else {
            // Legacy dbwriter Column: use pg_type directly
            using PgType = typename Column::pg_type;
            PgType::encode(value, buf);
        }
    }
};

template <typename Table>
Mapper<Table> make_mapper(const Table& table) {
    return Mapper<Table>(table);
}

}  // namespace dbwriter
