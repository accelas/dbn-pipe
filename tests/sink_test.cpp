// SPDX-License-Identifier: MIT

// tests/sink_test.cpp
#include <gtest/gtest.h>

#include <expected>
#include <string>

#include "dbn_pipe/stream/sink.hpp"
#include "dbn_pipe/stream/error.hpp"
#include "dbn_pipe/record_batch.hpp"

using namespace dbn_pipe;

// A minimal type that should satisfy Sink
struct MinimalSink {
    void OnError(const Error&) {}
    void OnComplete() {}
    void Invalidate() {}
};

// Verify concept satisfaction at compile time
static_assert(BasicSink<MinimalSink>, "MinimalSink must satisfy Sink concept");

TEST(SinkConceptTest, MinimalSinkSatisfiesConcept) {
    SUCCEED();  // Compile-time check above is the real test
}

// A type that should satisfy StreamingSink
struct MockStreamingSink {
    void OnData(RecordBatch&&) {}
    void OnError(const Error&) {}
    void OnComplete() {}
    void Invalidate() {}
};

static_assert(StreamingSink<MockStreamingSink>, "MockStreamingSink must satisfy StreamingSink");

// Verify ResultSink satisfies SingleResultSink concept
static_assert(SingleResultSink<ResultSink<std::string>>, "ResultSink<std::string> must satisfy SingleResultSink");
static_assert(SingleResultSink<ResultSink<int>>, "ResultSink<int> must satisfy SingleResultSink");

TEST(SinkConceptTest, StreamingSinkSatisfiesConcept) {
    SUCCEED();
}

TEST(StreamRecordSinkTest, DeliversDataToCallback) {
    bool data_called = false;
    RecordBatch captured_batch;

    StreamRecordSink sink(
        [&](RecordBatch&& batch) { data_called = true; captured_batch = std::move(batch); },
        [](const Error&) {},
        []() {}
    );

    RecordBatch batch;
    sink.OnData(std::move(batch));

    EXPECT_TRUE(data_called);
}

TEST(StreamRecordSinkTest, InvalidateStopsCallbacks) {
    bool data_called = false;

    StreamRecordSink sink(
        [&](RecordBatch&&) { data_called = true; },
        [](const Error&) {},
        []() {}
    );

    sink.Invalidate();

    RecordBatch batch;
    sink.OnData(std::move(batch));

    EXPECT_FALSE(data_called);
}

TEST(ResultSinkTest, DeliversResultToCallback) {
    std::expected<std::string, Error> captured;

    ResultSink<std::string> sink([&](std::expected<std::string, Error> result) {
        captured = std::move(result);
    });

    sink.OnResult("success");

    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(*captured, "success");
}

TEST(ResultSinkTest, DeliversErrorViaOnError) {
    std::expected<std::string, Error> captured;

    ResultSink<std::string> sink([&](std::expected<std::string, Error> result) {
        captured = std::move(result);
    });

    sink.OnError(Error{ErrorCode::ConnectionFailed, "test error"});

    ASSERT_FALSE(captured.has_value());
    EXPECT_EQ(captured.error().code, ErrorCode::ConnectionFailed);
}

TEST(ResultSinkTest, ExactlyOnceDelivery) {
    int call_count = 0;

    ResultSink<int> sink([&](std::expected<int, Error>) {
        ++call_count;
    });

    sink.OnResult(1);
    sink.OnResult(2);  // Should be ignored
    sink.OnError(Error{ErrorCode::InvalidState, "late"});  // Should be ignored

    EXPECT_EQ(call_count, 1);
}
