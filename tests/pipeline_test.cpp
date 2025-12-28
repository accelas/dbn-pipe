// tests/pipeline_test.cpp
#include <gtest/gtest.h>

#include <vector>

#include "src/pipeline.hpp"

using namespace databento_async;

// Mock downstream that satisfies Downstream concept
struct MockDownstream {
    std::vector<std::byte> received;
    bool error_called = false;
    bool done_called = false;

    void Read(std::pmr::vector<std::byte> data) {
        received.insert(received.end(), data.begin(), data.end());
    }
    void OnError(const Error&) { error_called = true; }
    void OnDone() { done_called = true; }
};

// Mock upstream that satisfies Upstream concept
struct MockUpstream {
    void Write(std::pmr::vector<std::byte>) {}
    void Suspend() {}
    void Resume() {}
    void Close() {}
};

// Verify concepts compile
static_assert(Downstream<MockDownstream>);
static_assert(Upstream<MockUpstream>);

TEST(PipelineTest, ConceptsSatisfied) {
    MockDownstream ds;
    static_assert(Downstream<MockDownstream>);
    SUCCEED();
}
