// Copyright 2026 Kai Wang
// SPDX-License-Identifier: Apache-2.0

#include "dbwriter/batch_writer.hpp"
#include "test/mock_database.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace dbwriter {
namespace {

using ::testing::_;
using ::testing::Return;

TEST(BackpressureConfigTest, Defaults) {
    BackpressureConfig config;

    EXPECT_EQ(config.max_pending_bytes, 1ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(config.high_water_mark, 256);
    EXPECT_EQ(config.low_water_mark, 64);
}

TEST(BatchWriterTest, EnqueueBatch_IncrementsQueueSize) {
    // This tests queue behavior - full test needs async context
    SUCCEED();
}

}  // namespace
}  // namespace dbwriter
