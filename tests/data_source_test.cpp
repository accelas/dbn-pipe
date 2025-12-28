// tests/data_source_test.cpp
#include <gtest/gtest.h>

#include <databento/record.hpp>

#include "src/data_source.hpp"

using namespace databento;
using namespace databento_async;

// Concrete test implementation
class TestDataSource : public DataSource {
public:
    void Start() override { started_ = true; }
    void Stop() override { started_ = false; }

    // Expose protected methods for testing
    using DataSource::DeliverBytes;
    using DataSource::DeliverError;

    bool started_ = false;
};

TEST(DataSourceTest, PauseResume) {
    TestDataSource ds;

    EXPECT_FALSE(ds.IsPaused());
    ds.Pause();
    EXPECT_TRUE(ds.IsPaused());
    ds.Resume();
    EXPECT_FALSE(ds.IsPaused());
}

TEST(DataSourceTest, OnRecordCallback) {
    TestDataSource ds;

    int call_count = 0;
    ds.OnRecord([&](const Record&) { call_count++; });

    // Create a valid MboMsg
    alignas(8) std::byte buffer[sizeof(MboMsg)] = {};
    auto* msg = reinterpret_cast<MboMsg*>(buffer);
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;

    ds.DeliverBytes({buffer, sizeof(buffer)});

    EXPECT_EQ(call_count, 1);
}

TEST(DataSourceTest, OnErrorCallback) {
    TestDataSource ds;

    databento_async::ErrorCode received_code{};
    ds.OnError([&](const Error& e) { received_code = e.code; });

    ds.DeliverError({databento_async::ErrorCode::ConnectionClosed, "closed"});

    EXPECT_EQ(received_code, databento_async::ErrorCode::ConnectionClosed);
}

TEST(DataSourceTest, PausedDoesNotDeliver) {
    TestDataSource ds;

    int call_count = 0;
    ds.OnRecord([&](const Record&) { call_count++; });

    ds.Pause();

    // Create a valid record
    alignas(8) std::byte buffer[sizeof(MboMsg)] = {};
    auto* msg = reinterpret_cast<MboMsg*>(buffer);
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;

    ds.DeliverBytes({buffer, sizeof(buffer)});

    // Should buffer but not deliver while paused
    EXPECT_EQ(call_count, 0);

    ds.Resume();

    // After resume, should deliver buffered records
    EXPECT_EQ(call_count, 1);
}
