// SPDX-License-Identifier: MIT

#pragma once

#include "dbwriter/libpq_wrapper.hpp"
#include <gmock/gmock.h>
#include <queue>
#include <functional>

namespace dbwriter::testing {

// Mock libpq that simulates async timing behavior
class MockLibPq : public ILibPq {
public:
    MOCK_METHOD(PGconn*, connectdb, (const char*), (override));
    MOCK_METHOD(void, finish, (PGconn*), (override));
    MOCK_METHOD(ConnStatusType, status, (const PGconn*), (override));
    MOCK_METHOD(int, socket, (const PGconn*), (override));
    MOCK_METHOD(int, setnonblocking, (PGconn*, int), (override));
    MOCK_METHOD(char*, errorMessage, (const PGconn*), (override));
    MOCK_METHOD(int, sendQuery, (PGconn*, const char*), (override));
    MOCK_METHOD(int, flush, (PGconn*), (override));
    MOCK_METHOD(int, consumeInput, (PGconn*), (override));
    MOCK_METHOD(int, isBusy, (PGconn*), (override));
    MOCK_METHOD(PGresult*, getResult, (PGconn*), (override));
    MOCK_METHOD(ExecStatusType, resultStatus, (const PGresult*), (override));
    MOCK_METHOD(char*, resultErrorMessage, (const PGresult*), (override));
    MOCK_METHOD(void, clear, (PGresult*), (override));
    MOCK_METHOD(int, ntuples, (const PGresult*), (override));
    MOCK_METHOD(int, nfields, (const PGresult*), (override));
    MOCK_METHOD(char*, getvalue, (const PGresult*, int, int), (override));
    MOCK_METHOD(int, getisnull, (const PGresult*, int, int), (override));
    MOCK_METHOD(char*, fname, (const PGresult*, int), (override));
    MOCK_METHOD(int, putCopyData, (PGconn*, const char*, int), (override));
    MOCK_METHOD(int, putCopyEnd, (PGconn*, const char*), (override));
    MOCK_METHOD(char*, cmdTuples, (PGresult*), (override));
};

// Simulates libpq async state machine for testing
// Models the behavior: sendQuery -> (wait for data) -> consumeInput -> isBusy -> getResult
class SimulatedLibPq : public ILibPq {
public:
    // Configure how many consumeInput calls before result is ready
    void set_consume_calls_until_ready(int n) { consume_calls_until_ready_ = n; }

    // Configure consumeInput to fail
    void set_consume_fails(bool fails) { consume_fails_ = fails; }

    // Configure result status
    void set_result_status(ExecStatusType status) { result_status_ = status; }

    PGconn* connectdb(const char*) override {
        return reinterpret_cast<PGconn*>(0x1234);  // Fake pointer
    }

    void finish(PGconn*) override {}

    ConnStatusType status(const PGconn*) override {
        return CONNECTION_OK;
    }

    int socket(const PGconn*) override {
        return fake_fd_;
    }

    int setnonblocking(PGconn*, int) override {
        return 0;
    }

    char* errorMessage(const PGconn*) override {
        return const_cast<char*>(error_message_.c_str());
    }

    int sendQuery(PGconn*, const char*) override {
        query_in_progress_ = true;
        consume_call_count_ = 0;
        result_returned_ = false;
        results_consumed_ = false;
        return 1;  // Success
    }

    int flush(PGconn*) override {
        return 0;  // All data flushed
    }

    int consumeInput(PGconn*) override {
        consume_call_count_++;
        if (consume_fails_) {
            error_message_ = "consume failed";
            return 0;  // Failure
        }
        return 1;  // Success
    }

    int isBusy(PGconn*) override {
        if (!query_in_progress_) return 0;
        // Simulate data arriving after N consume calls
        return (consume_call_count_ < consume_calls_until_ready_) ? 1 : 0;
    }

    PGresult* getResult(PGconn*) override {
        if (results_consumed_) {
            return nullptr;  // No more results
        }
        if (result_returned_) {
            // Second call after first result - no more results
            results_consumed_ = true;
            return nullptr;
        }
        result_returned_ = true;
        query_in_progress_ = false;
        return reinterpret_cast<PGresult*>(0x5678);  // Fake pointer
    }

    ExecStatusType resultStatus(const PGresult*) override {
        return result_status_;
    }

    char* resultErrorMessage(const PGresult*) override {
        return const_cast<char*>(result_error_.c_str());
    }

    void clear(PGresult*) override {
        // Don't reset result state here - that's for the next query
    }

    int ntuples(const PGresult*) override {
        return static_cast<int>(result_rows_.size());
    }

    int nfields(const PGresult*) override {
        return result_rows_.empty() ? 0 : static_cast<int>(result_rows_[0].size());
    }

    char* getvalue(const PGresult*, int row, int col) override {
        if (row < 0 || static_cast<size_t>(row) >= result_rows_.size()) {
            return const_cast<char*>("");
        }
        if (col < 0 || static_cast<size_t>(col) >= result_rows_[row].size()) {
            return const_cast<char*>("");
        }
        return const_cast<char*>(result_rows_[row][col].c_str());
    }

    int getisnull(const PGresult*, int row, int col) override {
        if (row < 0 || static_cast<size_t>(row) >= result_nulls_.size()) {
            return 1;
        }
        if (col < 0 || static_cast<size_t>(col) >= result_nulls_[row].size()) {
            return 1;
        }
        return result_nulls_[row][col] ? 1 : 0;
    }

    char* fname(const PGresult*, int col) override {
        if (col < 0 || static_cast<size_t>(col) >= column_names_.size()) {
            return const_cast<char*>("");
        }
        return const_cast<char*>(column_names_[col].c_str());
    }

    // Configure result data for query tests
    void set_result_data(std::vector<std::vector<std::string>> rows,
                         std::vector<std::vector<bool>> nulls = {},
                         std::vector<std::string> col_names = {}) {
        result_rows_ = std::move(rows);
        result_nulls_ = std::move(nulls);
        column_names_ = std::move(col_names);
        // Fill nulls if not provided
        if (result_nulls_.empty() && !result_rows_.empty()) {
            result_nulls_.resize(result_rows_.size());
            for (size_t i = 0; i < result_rows_.size(); ++i) {
                result_nulls_[i].resize(result_rows_[i].size(), false);
            }
        }
    }

    int putCopyData(PGconn*, const char*, int) override {
        return 1;  // Success
    }

    int putCopyEnd(PGconn*, const char*) override {
        return 1;  // Success
    }

    char* cmdTuples(PGresult*) override {
        return const_cast<char*>(cmd_tuples_.c_str());
    }

    void set_cmd_tuples(std::string val) { cmd_tuples_ = std::move(val); }

    // Test inspection
    int get_consume_call_count() const { return consume_call_count_; }

private:
    int fake_fd_ = 42;
    int consume_calls_until_ready_ = 1;  // Default: ready after first consume
    int consume_call_count_ = 0;
    bool consume_fails_ = false;
    bool query_in_progress_ = false;
    bool result_returned_ = false;
    bool results_consumed_ = false;
    ExecStatusType result_status_ = PGRES_COMMAND_OK;
    std::string error_message_;
    std::string result_error_;
    std::vector<std::vector<std::string>> result_rows_;
    std::vector<std::vector<bool>> result_nulls_;
    std::vector<std::string> column_names_;
    std::string cmd_tuples_ = "0";
};

}  // namespace dbwriter::testing
