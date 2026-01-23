// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

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
    MOCK_METHOD(int, putCopyData, (PGconn*, const char*, int), (override));
    MOCK_METHOD(int, putCopyEnd, (PGconn*, const char*), (override));
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

    int putCopyData(PGconn*, const char*, int) override {
        return 1;  // Success
    }

    int putCopyEnd(PGconn*, const char*) override {
        return 1;  // Success
    }

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
};

}  // namespace dbwriter::testing
