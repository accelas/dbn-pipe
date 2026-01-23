// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#include "dbwriter/schema_validator.hpp"
#include "dbwriter/table.hpp"
#include "dbwriter/pg_types.hpp"
#include "test/mock_database.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace dbwriter {
namespace {

constexpr auto test_table = Table{"test",
    Column<"id", int64_t, pg::BigInt>{},
    Column<"name", std::string_view, pg::Text>{},
};

TEST(SchemaValidatorTest, ModeEnum) {
    EXPECT_NE(static_cast<int>(SchemaValidator::Mode::Strict),
              static_cast<int>(SchemaValidator::Mode::Warn));
    EXPECT_NE(static_cast<int>(SchemaValidator::Mode::Warn),
              static_cast<int>(SchemaValidator::Mode::Bootstrap));
}

}  // namespace
}  // namespace dbwriter
