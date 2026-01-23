// SPDX-License-Identifier: MIT

// tests/pipeline_sink_test.cpp
// Tests for StreamRecordSink used by streaming protocols
#include <gtest/gtest.h>

#include "lib/stream/sink.hpp"
#include "lib/stream/error.hpp"
#include "src/record_batch.hpp"

using namespace dbn_pipe;

TEST(StreamRecordSinkTest, InvalidationStopsCallbacks) {
    bool data_called = false;
    bool error_called = false;
    bool complete_called = false;

    StreamRecordSink sink(
        [&](RecordBatch&&) { data_called = true; },
        [&](const Error&) { error_called = true; },
        [&]() { complete_called = true; }
    );

    // Before invalidation, callbacks should work
    sink.OnData(RecordBatch{});
    EXPECT_TRUE(data_called);

    // Invalidate
    sink.Invalidate();

    // Reset flags
    data_called = false;

    // After invalidation, callbacks should be no-ops
    sink.OnData(RecordBatch{});
    sink.OnError(Error{ErrorCode::ConnectionFailed, "test"});
    sink.OnComplete();

    EXPECT_FALSE(data_called);
    EXPECT_FALSE(error_called);
    EXPECT_FALSE(complete_called);
}

TEST(StreamRecordSinkTest, CallbacksWorkBeforeInvalidation) {
    int data_count = 0;
    int error_count = 0;
    int complete_count = 0;

    StreamRecordSink sink(
        [&](RecordBatch&&) { data_count++; },
        [&](const Error&) { error_count++; },
        [&]() { complete_count++; }
    );

    sink.OnData(RecordBatch{});
    sink.OnData(RecordBatch{});
    sink.OnError(Error{ErrorCode::ConnectionFailed, "test"});
    sink.OnComplete();

    EXPECT_EQ(data_count, 2);
    EXPECT_EQ(error_count, 1);
    EXPECT_EQ(complete_count, 1);
}
