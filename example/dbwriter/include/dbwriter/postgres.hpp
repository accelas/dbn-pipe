// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "dbwriter/database.hpp"
#include "dbwriter/libpq_wrapper.hpp"
#include <libpq-fe.h>
#include <asio.hpp>
#include <memory>
#include <string>

namespace dbwriter {

struct PostgresConfig {
    std::string host = "localhost";
    int port = 5432;
    std::string database;
    std::string user;
    std::string password;

    std::string connection_string() const;
};

class PostgresCopyWriter : public ICopyWriter {
public:
    PostgresCopyWriter(PGconn* conn, asio::io_context& ctx,
                       std::string_view table,
                       std::span<const std::string_view> columns,
                       ILibPq& pq = GetLibPq());
    ~PostgresCopyWriter();

    asio::awaitable<void> start() override;
    asio::awaitable<void> write_row(std::span<const std::byte> data) override;
    asio::awaitable<void> finish() override;
    asio::awaitable<void> abort() override;

private:
    asio::awaitable<void> wait_writable();
    asio::awaitable<void> send_data(std::span<const std::byte> data);

    PGconn* conn_;
    asio::posix::stream_descriptor socket_;
    std::string table_;
    std::vector<std::string> columns_;
    bool in_copy_ = false;
    ILibPq& pq_;
};

class PostgresDatabase : public IDatabase {
public:
    PostgresDatabase(asio::io_context& ctx, const PostgresConfig& config,
                     ILibPq& pq = GetLibPq());
    ~PostgresDatabase();

    asio::awaitable<void> connect();

    asio::awaitable<QueryResult> query(std::string_view sql) override;
    asio::awaitable<void> execute(std::string_view sql) override;

    std::unique_ptr<ICopyWriter> begin_copy(
        std::string_view table,
        std::span<const std::string_view> columns) override;

    bool is_connected() const override;

private:
    asio::io_context& ctx_;
    PostgresConfig config_;
    PGconn* conn_ = nullptr;
    ILibPq& pq_;
};

}  // namespace dbwriter
