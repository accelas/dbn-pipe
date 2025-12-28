// tests/pipeline_test.cpp
#include <gtest/gtest.h>

#include <memory>
#include <optional>
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

// Verify concepts compile
static_assert(Downstream<MockDownstream>);

TEST(PipelineTest, ConceptsSatisfied) {
    MockDownstream ds;
    static_assert(Downstream<MockDownstream>);
    SUCCEED();
}
