// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "dbwriter/database.hpp"
#include <asio/awaitable.hpp>
#include <string>
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

// Template implementations would go here or in a .ipp file

}  // namespace dbwriter
