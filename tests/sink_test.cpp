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
