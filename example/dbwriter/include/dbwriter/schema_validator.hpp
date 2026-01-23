// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "dbwriter/database.hpp"
#include "dbwriter/pg_types.hpp"
#include <asio/awaitable.hpp>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace dbwriter {

struct SchemaMismatch {
    std::string column;
    std::string expected;
    std::string actual;
};

class SchemaValidator {
public:
    enum class Mode {
        Strict,     // Fail if any mismatch
        Warn,       // Log warnings, continue
        Bootstrap,  // Create table if missing
    };

    template <typename Table>
    asio::awaitable<std::vector<SchemaMismatch>> validate(
            IDatabase& db, const Table& table, Mode mode);

    template <typename Table>
    asio::awaitable<void> ensure_table(IDatabase& db, const Table& table);

    template <typename Table>
    std::string create_table_sql(const Table& table);
};

// Template implementations

template <typename Table>
std::string SchemaValidator::create_table_sql(const Table& table) {
    std::ostringstream ss;
    ss << "CREATE TABLE IF NOT EXISTS \"" << table.name() << "\" (";

    auto columns = table.column_names();
    auto types = table.column_pg_types();

    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << "\"" << columns[i] << "\" " << types[i];
    }
    ss << ")";
    return ss.str();
}

template <typename Table>
asio::awaitable<void> SchemaValidator::ensure_table(
        IDatabase& db, const Table& table) {
    std::string sql = create_table_sql(table);
    co_await db.execute(sql);
}

template <typename Table>
asio::awaitable<std::vector<SchemaMismatch>> SchemaValidator::validate(
        IDatabase& db, const Table& table, Mode mode) {
    std::vector<SchemaMismatch> mismatches;

    // Query column info from information_schema
    std::ostringstream sql;
    sql << "SELECT column_name, data_type FROM information_schema.columns "
        << "WHERE table_name = '" << table.name() << "' "
        << "ORDER BY ordinal_position";

    auto result = co_await db.query(sql.str());

    auto expected_columns = table.column_names();
    auto expected_types = table.column_pg_types();

    // Check if table exists
    if (result.empty() && mode == Mode::Bootstrap) {
        co_await ensure_table(db, table);
        co_return mismatches;  // Table created, no mismatches
    }

    // Build map of actual columns
    std::unordered_map<std::string, std::string> actual_cols;
    for (const auto& row : result) {
        std::string col_name{row->get_string(0)};
        std::string data_type{row->get_string(1)};
        actual_cols[col_name] = data_type;
    }

    // Check each expected column
    for (size_t i = 0; i < expected_columns.size(); ++i) {
        std::string col_name{expected_columns[i]};
        std::string expected_type{expected_types[i]};

        auto it = actual_cols.find(col_name);
        if (it == actual_cols.end()) {
            mismatches.push_back({col_name, expected_type, "(missing)"});
        } else {
            // Normalize type comparison (e.g., "bigint" vs "BIGINT")
            std::string actual_type = it->second;
            // Simple case-insensitive comparison for common types
            std::string exp_lower = expected_type;
            std::string act_lower = actual_type;
            for (auto& c : exp_lower) c = static_cast<char>(std::tolower(c));
            for (auto& c : act_lower) c = static_cast<char>(std::tolower(c));

            // Handle common type aliases
            if (exp_lower == "int8" || exp_lower == "bigint") {
                exp_lower = "bigint";
            }
            if (act_lower == "int8" || act_lower == "bigint") {
                act_lower = "bigint";
            }
            if (exp_lower == "int4" || exp_lower == "integer") {
                exp_lower = "integer";
            }
            if (act_lower == "int4" || act_lower == "integer") {
                act_lower = "integer";
            }

            if (exp_lower != act_lower) {
                mismatches.push_back({col_name, expected_type, actual_type});
            }
        }
    }

    if (!mismatches.empty() && mode == Mode::Strict) {
        std::ostringstream err;
        err << "Schema validation failed for table '" << table.name() << "': ";
        for (const auto& m : mismatches) {
            err << m.column << " (expected " << m.expected
                << ", got " << m.actual << "); ";
        }
        throw std::runtime_error(err.str());
    }

    co_return mismatches;
}

}  // namespace dbwriter
