// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "dbwriter/database.hpp"
#include <gmock/gmock.h>

namespace dbwriter::testing {

class MockRow : public IRow {
public:
    MOCK_METHOD(int64_t, get_int64, (std::size_t), (const, override));
    MOCK_METHOD(int32_t, get_int32, (std::size_t), (const, override));
    MOCK_METHOD(std::string_view, get_string, (std::size_t), (const, override));
    MOCK_METHOD(bool, is_null, (std::size_t), (const, override));
};

class MockCopyWriter : public ICopyWriter {
public:
    MOCK_METHOD(asio::awaitable<void>, start, (), (override));
    MOCK_METHOD(asio::awaitable<void>, write_row, (std::span<const std::byte>), (override));
    MOCK_METHOD(asio::awaitable<void>, finish, (), (override));
    MOCK_METHOD(asio::awaitable<void>, abort, (), (override));

    std::vector<std::vector<std::byte>> captured_rows;
};

class MockDatabase : public IDatabase {
public:
    MOCK_METHOD(asio::awaitable<QueryResult>, query, (std::string_view), (override));
    MOCK_METHOD(asio::awaitable<void>, execute, (std::string_view), (override));
    MOCK_METHOD(std::unique_ptr<ICopyWriter>, begin_copy,
        (std::string_view, std::span<const std::string_view>), (override));
    MOCK_METHOD(bool, is_connected, (), (const, override));
};

}  // namespace dbwriter::testing
