// SPDX-License-Identifier: MIT

#include "dbwriter/schema_validator.hpp"
#include "dbwriter/pg_types.hpp"
#include "src/table/table.hpp"
#include <gtest/gtest.h>
#include <asio.hpp>

namespace dbwriter {
namespace {

constexpr auto test_table = dbn_pipe::Table{"test",
    dbn_pipe::Column<"id", dbn_pipe::Int64>{},
    dbn_pipe::Column<"name", dbn_pipe::Text>{},
};

// Simple row implementation for testing
class TestRow : public IRow {
public:
    TestRow(std::vector<std::string> values) : values_(std::move(values)) {}

    int64_t get_int64(std::size_t col) const override {
        return col < values_.size() ? std::stoll(values_[col]) : 0;
    }
    int32_t get_int32(std::size_t col) const override {
        return col < values_.size() ? std::stoi(values_[col]) : 0;
    }
    std::string_view get_string(std::size_t col) const override {
        return col < values_.size() ? values_[col] : "";
    }
    bool is_null(std::size_t col) const override {
        return col >= values_.size();
    }

private:
    std::vector<std::string> values_;
};

// Configurable mock database for testing schema validation
class TestSchemaDatabase : public IDatabase {
public:
    QueryResult query_result;
    bool execute_called = false;
    std::string last_execute_sql;
    std::string last_query_sql;

    asio::awaitable<QueryResult> query(std::string_view sql) override {
        last_query_sql = std::string(sql);
        co_return std::move(query_result);
    }
    asio::awaitable<void> execute(std::string_view sql) override {
        execute_called = true;
        last_execute_sql = std::string(sql);
        co_return;
    }
    std::unique_ptr<ICopyWriter> begin_copy(
            std::string_view,
            std::span<const std::string_view>) override {
        return nullptr;  // Not used in schema tests
    }
    bool is_connected() const override { return true; }

    // Helper to set up result rows
    void set_result(std::vector<std::vector<std::string>> rows) {
        std::vector<std::unique_ptr<IRow>> result_rows;
        for (auto& row : rows) {
            result_rows.push_back(std::make_unique<TestRow>(std::move(row)));
        }
        query_result = QueryResult{std::move(result_rows)};
    }
};

TEST(SchemaValidatorTest, ModeEnum) {
    EXPECT_NE(static_cast<int>(SchemaValidator::Mode::Strict),
              static_cast<int>(SchemaValidator::Mode::Warn));
    EXPECT_NE(static_cast<int>(SchemaValidator::Mode::Warn),
              static_cast<int>(SchemaValidator::Mode::Bootstrap));
}

TEST(SchemaValidatorTest, MissingTableStrictThrows) {
    asio::io_context ctx;
    TestSchemaDatabase db;
    SchemaValidator validator;

    // Empty result = table doesn't exist
    db.set_result({});

    bool threw = false;
    std::string error_msg;

    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        try {
            co_await validator.validate(db, test_table, SchemaValidator::Mode::Strict);
        } catch (const std::runtime_error& e) {
            threw = true;
            error_msg = e.what();
        }
    }, asio::detached);

    ctx.run();

    EXPECT_TRUE(threw);
    EXPECT_NE(error_msg.find("does not exist"), std::string::npos);
}

TEST(SchemaValidatorTest, MissingTableWarnReturnsMismatch) {
    asio::io_context ctx;
    TestSchemaDatabase db;
    SchemaValidator validator;

    // Empty result = table doesn't exist
    db.set_result({});

    std::vector<SchemaMismatch> result;

    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        result = co_await validator.validate(db, test_table, SchemaValidator::Mode::Warn);
    }, asio::detached);

    ctx.run();

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].column, "(table)");
    EXPECT_EQ(result[0].expected, "test");
    EXPECT_EQ(result[0].actual, "(does not exist)");
}

TEST(SchemaValidatorTest, MissingTableBootstrapCreatesTable) {
    asio::io_context ctx;
    TestSchemaDatabase db;
    SchemaValidator validator;

    // Empty result = table doesn't exist
    db.set_result({});

    std::vector<SchemaMismatch> result;

    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        result = co_await validator.validate(db, test_table, SchemaValidator::Mode::Bootstrap);
    }, asio::detached);

    ctx.run();

    // Should create table and return no mismatches
    EXPECT_TRUE(db.execute_called);
    EXPECT_NE(db.last_execute_sql.find("CREATE TABLE"), std::string::npos);
    EXPECT_TRUE(result.empty());
}

TEST(SchemaValidatorTest, QueryUsesCurrentSchema) {
    asio::io_context ctx;
    TestSchemaDatabase db;
    SchemaValidator validator;

    // Set up matching schema
    db.set_result({
        {"id", "bigint"},
        {"name", "text"},
    });

    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        co_await validator.validate(db, test_table, SchemaValidator::Mode::Strict);
    }, asio::detached);

    ctx.run();

    // Verify query uses current_schema()
    EXPECT_NE(db.last_query_sql.find("current_schema()"), std::string::npos);
}

TEST(SchemaValidatorTest, TableNameEscaped) {
    asio::io_context ctx;
    TestSchemaDatabase db;
    SchemaValidator validator;

    // Table with SQL injection attempt in name
    constexpr auto evil_table = dbn_pipe::Table{"test'; DROP TABLE users; --",
        dbn_pipe::Column<"id", dbn_pipe::Int64>{},
    };

    db.set_result({});  // Table doesn't exist

    asio::co_spawn(ctx, [&]() -> asio::awaitable<void> {
        try {
            co_await validator.validate(db, evil_table, SchemaValidator::Mode::Warn);
        } catch (...) {
            // Ignore exceptions
        }
    }, asio::detached);

    ctx.run();

    // Verify the single quote was escaped (doubled)
    EXPECT_NE(db.last_query_sql.find("test''; DROP TABLE"), std::string::npos);
}

}  // namespace
}  // namespace dbwriter
