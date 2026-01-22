// tests/sink_test.cpp
#include <gtest/gtest.h>

#include "lib/stream/sink.hpp"
#include "lib/stream/error.hpp"
#include "src/record_batch.hpp"

using namespace dbn_pipe;

// A minimal type that should satisfy Sink
struct MinimalSink {
    void OnError(const Error&) {}
    void OnComplete() {}
    void Invalidate() {}
};

// Verify concept satisfaction at compile time
static_assert(Sink<MinimalSink>, "MinimalSink must satisfy Sink concept");

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

TEST(SinkConceptTest, StreamingSinkSatisfiesConcept) {
    SUCCEED();
}

TEST(RecordSinkTest, DeliversDataToCallback) {
    bool data_called = false;
    RecordBatch captured_batch;

    RecordSink sink(
        [&](RecordBatch&& batch) { data_called = true; captured_batch = std::move(batch); },
        [](const Error&) {},
        []() {}
    );

    RecordBatch batch;
    sink.OnData(std::move(batch));

    EXPECT_TRUE(data_called);
}

TEST(RecordSinkTest, InvalidateStopsCallbacks) {
    bool data_called = false;

    RecordSink sink(
        [&](RecordBatch&&) { data_called = true; },
        [](const Error&) {},
        []() {}
    );

    sink.Invalidate();

    RecordBatch batch;
    sink.OnData(std::move(batch));

    EXPECT_FALSE(data_called);
}
