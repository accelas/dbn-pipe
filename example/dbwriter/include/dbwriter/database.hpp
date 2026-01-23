// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <asio/awaitable.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dbwriter {

// Query result row
class IRow {
public:
    virtual ~IRow() = default;

    virtual int64_t get_int64(std::size_t col) const = 0;
    virtual int32_t get_int32(std::size_t col) const = 0;
    virtual std::string_view get_string(std::size_t col) const = 0;
    virtual bool is_null(std::size_t col) const = 0;
};

// Query result set
class QueryResult {
public:
    QueryResult() = default;
    explicit QueryResult(std::vector<std::unique_ptr<IRow>> rows)
        : rows_(std::move(rows)) {}

    bool empty() const { return rows_.empty(); }
    std::size_t size() const { return rows_.size(); }

    const IRow& operator[](std::size_t i) const { return *rows_[i]; }

    auto begin() const { return rows_.begin(); }
    auto end() const { return rows_.end(); }

private:
    std::vector<std::unique_ptr<IRow>> rows_;
};

// COPY writer interface
class ICopyWriter {
public:
    virtual ~ICopyWriter() = default;

    virtual asio::awaitable<void> start() = 0;
    virtual asio::awaitable<void> write_row(std::span<const std::byte> data) = 0;
    virtual asio::awaitable<void> finish() = 0;
    virtual asio::awaitable<void> abort() = 0;
};

// Database interface
class IDatabase {
public:
    virtual ~IDatabase() = default;

    virtual asio::awaitable<QueryResult> query(std::string_view sql) = 0;
    virtual asio::awaitable<void> execute(std::string_view sql) = 0;

    virtual std::unique_ptr<ICopyWriter> begin_copy(
        std::string_view table,
        std::span<const std::string_view> columns) = 0;

    virtual bool is_connected() const = 0;
};

}  // namespace dbwriter
