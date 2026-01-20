// tests/duckdb_storage_factory_test.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include "src/duckdb_storage.hpp"

using namespace dbn_pipe;

TEST(DuckDbStorageFactoryTest, CreateSucceedsWithEmptyPath) {
    auto result = DuckDbStorage::Create("", 100000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get()->MappingCount(), 0);
}

TEST(DuckDbStorageFactoryTest, CreateSucceedsWithValidPath) {
    std::string path = "/tmp/test_storage_factory.duckdb";
    std::filesystem::remove(path);

    auto result = DuckDbStorage::Create(path, 50000);
    ASSERT_TRUE(result.has_value());

    std::filesystem::remove(path);
}

TEST(DuckDbStorageFactoryTest, CreateFailsWithInvalidPath) {
    // Directory that doesn't exist and can't be created
    auto result = DuckDbStorage::Create("/nonexistent/path/to/db.duckdb", 100000);
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().empty());
}
