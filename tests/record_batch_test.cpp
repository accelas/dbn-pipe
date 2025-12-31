// tests/record_batch_test.cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>

#include <databento/constants.hpp>
#include <databento/enums.hpp>
#include <databento/record.hpp>

#include "src/record_batch.hpp"

using namespace databento;
using namespace dbn_pipe;

// Helper to create an aligned buffer for testing
class AlignedBuffer {
public:
    static constexpr size_t kMaxSize = 1024;

    AlignedBuffer() = default;

    std::byte* data() { return buffer_.data(); }
    const std::byte* data() const { return buffer_.data(); }

    // Create a shared_ptr keepalive that points to this buffer
    std::shared_ptr<void> MakeKeepalive() {
        return std::shared_ptr<void>(this, [](void*){});  // No-op deleter
    }

private:
    alignas(8) std::array<std::byte, kMaxSize> buffer_{};
};

TEST(RecordBatchTest, EmptyBatch) {
    RecordBatch batch;
    EXPECT_EQ(batch.size(), 0);
    EXPECT_TRUE(batch.empty());
}

TEST(RecordBatchTest, SingleRecordRef) {
    AlignedBuffer buffer;

    // Create a valid MboMsg
    auto* msg = reinterpret_cast<MboMsg*>(buffer.data());
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;
    msg->hd.publisher_id = 1;
    msg->hd.instrument_id = 12345;
    msg->order_id = 999;
    msg->price = 100 * kFixedPriceScale;
    msg->size = 10;
    msg->side = Side::Bid;
    msg->action = Action::Add;

    // Build RecordBatch with RecordRef
    RecordBatch batch;
    RecordRef ref;
    ref.data = buffer.data();
    ref.size = sizeof(MboMsg);
    ref.keepalive = buffer.MakeKeepalive();
    batch.Add(std::move(ref));

    EXPECT_EQ(batch.size(), 1);
    EXPECT_FALSE(batch.empty());

    // Test Header() access
    const auto& header = batch[0].Header();
    EXPECT_EQ(header.rtype, RType::Mbo);
    EXPECT_EQ(header.publisher_id, 1);
    EXPECT_EQ(header.instrument_id, 12345);

    // Test As<T>() typed access
    const auto& record = batch[0].As<MboMsg>();
    EXPECT_EQ(record.order_id, 999);
    EXPECT_EQ(record.price, 100 * kFixedPriceScale);
    EXPECT_EQ(record.size, 10);
    EXPECT_EQ(record.side, Side::Bid);
    EXPECT_EQ(record.action, Action::Add);
}

TEST(RecordBatchTest, MultipleRecordRefs) {
    AlignedBuffer buffer1;
    AlignedBuffer buffer2;

    auto* msg1 = reinterpret_cast<MboMsg*>(buffer1.data());
    msg1->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg1->hd.rtype = RType::Mbo;
    msg1->hd.instrument_id = 111;
    msg1->order_id = 1001;

    auto* msg2 = reinterpret_cast<MboMsg*>(buffer2.data());
    msg2->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg2->hd.rtype = RType::Mbo;
    msg2->hd.instrument_id = 222;
    msg2->order_id = 2002;

    // Build RecordBatch with both records
    RecordBatch batch;

    RecordRef ref1;
    ref1.data = buffer1.data();
    ref1.size = sizeof(MboMsg);
    ref1.keepalive = buffer1.MakeKeepalive();
    batch.Add(std::move(ref1));

    RecordRef ref2;
    ref2.data = buffer2.data();
    ref2.size = sizeof(MboMsg);
    ref2.keepalive = buffer2.MakeKeepalive();
    batch.Add(std::move(ref2));

    EXPECT_EQ(batch.size(), 2);
    EXPECT_FALSE(batch.empty());

    // Verify first record
    EXPECT_EQ(batch[0].Header().instrument_id, 111);
    EXPECT_EQ(batch[0].As<MboMsg>().order_id, 1001);

    // Verify second record
    EXPECT_EQ(batch[1].Header().instrument_id, 222);
    EXPECT_EQ(batch[1].As<MboMsg>().order_id, 2002);
}

TEST(RecordBatchTest, DifferentRecordTypes) {
    AlignedBuffer mbo_buffer;
    AlignedBuffer trade_buffer;

    auto* mbo = reinterpret_cast<MboMsg*>(mbo_buffer.data());
    mbo->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    mbo->hd.rtype = RType::Mbo;
    mbo->hd.instrument_id = 100;
    mbo->order_id = 5555;

    auto* trade = reinterpret_cast<TradeMsg*>(trade_buffer.data());
    trade->hd.length = sizeof(TradeMsg) / kRecordHeaderLengthMultiplier;
    trade->hd.rtype = RType::Mbp0;
    trade->hd.instrument_id = 200;
    trade->price = 75 * kFixedPriceScale;
    trade->size = 100;

    RecordBatch batch;

    RecordRef ref1;
    ref1.data = mbo_buffer.data();
    ref1.size = sizeof(MboMsg);
    ref1.keepalive = mbo_buffer.MakeKeepalive();
    batch.Add(std::move(ref1));

    RecordRef ref2;
    ref2.data = trade_buffer.data();
    ref2.size = sizeof(TradeMsg);
    ref2.keepalive = trade_buffer.MakeKeepalive();
    batch.Add(std::move(ref2));

    EXPECT_EQ(batch.size(), 2);

    // Check first record (MboMsg)
    EXPECT_EQ(batch[0].Header().rtype, RType::Mbo);
    EXPECT_EQ(batch[0].Header().instrument_id, 100);
    EXPECT_EQ(batch[0].As<MboMsg>().order_id, 5555);

    // Check second record (TradeMsg)
    EXPECT_EQ(batch[1].Header().rtype, RType::Mbp0);
    EXPECT_EQ(batch[1].Header().instrument_id, 200);
    EXPECT_EQ(batch[1].As<TradeMsg>().price, 75 * kFixedPriceScale);
    EXPECT_EQ(batch[1].As<TradeMsg>().size, 100);
}

TEST(RecordBatchTest, BatchClear) {
    AlignedBuffer buffer;
    auto* msg = reinterpret_cast<MboMsg*>(buffer.data());
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;

    RecordBatch batch;
    RecordRef ref;
    ref.data = buffer.data();
    ref.size = sizeof(MboMsg);
    ref.keepalive = buffer.MakeKeepalive();
    batch.Add(std::move(ref));

    EXPECT_EQ(batch.size(), 1);
    EXPECT_FALSE(batch.empty());

    batch.Clear();

    EXPECT_EQ(batch.size(), 0);
    EXPECT_TRUE(batch.empty());
}

TEST(RecordBatchTest, BatchIteration) {
    AlignedBuffer buffer1;
    AlignedBuffer buffer2;
    AlignedBuffer buffer3;

    for (size_t i = 0; i < 3; ++i) {
        AlignedBuffer* buf = (i == 0) ? &buffer1 : (i == 1) ? &buffer2 : &buffer3;
        auto* msg = reinterpret_cast<MboMsg*>(buf->data());
        msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
        msg->hd.rtype = RType::Mbo;
        msg->hd.instrument_id = static_cast<uint32_t>(i + 1);
    }

    RecordBatch batch;

    RecordRef ref1;
    ref1.data = buffer1.data();
    ref1.size = sizeof(MboMsg);
    ref1.keepalive = buffer1.MakeKeepalive();
    batch.Add(std::move(ref1));

    RecordRef ref2;
    ref2.data = buffer2.data();
    ref2.size = sizeof(MboMsg);
    ref2.keepalive = buffer2.MakeKeepalive();
    batch.Add(std::move(ref2));

    RecordRef ref3;
    ref3.data = buffer3.data();
    ref3.size = sizeof(MboMsg);
    ref3.keepalive = buffer3.MakeKeepalive();
    batch.Add(std::move(ref3));

    // Test range-for iteration
    size_t idx = 0;
    for (const auto& ref : batch) {
        EXPECT_EQ(ref.Header().instrument_id, idx + 1);
        ++idx;
    }
    EXPECT_EQ(idx, 3);

    // Test begin/end
    EXPECT_EQ(batch.end() - batch.begin(), 3);
}

TEST(RecordBatchTest, BatchReservation) {
    RecordBatch batch(128);  // Reserve for 128 records

    EXPECT_EQ(batch.size(), 0);
    EXPECT_TRUE(batch.empty());

    // Add should work without reallocation up to reserved amount
    AlignedBuffer buffer;
    auto* msg = reinterpret_cast<MboMsg*>(buffer.data());
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;

    RecordRef ref;
    ref.data = buffer.data();
    ref.size = sizeof(MboMsg);
    ref.keepalive = buffer.MakeKeepalive();
    batch.Add(std::move(ref));

    EXPECT_EQ(batch.size(), 1);
}

TEST(RecordBatchTest, BatchMoveSemantics) {
    AlignedBuffer buffer;
    auto* msg = reinterpret_cast<MboMsg*>(buffer.data());
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;
    msg->hd.instrument_id = 42;

    RecordBatch batch1;
    RecordRef ref;
    ref.data = buffer.data();
    ref.size = sizeof(MboMsg);
    ref.keepalive = buffer.MakeKeepalive();
    batch1.Add(std::move(ref));

    // Move construct
    RecordBatch batch2(std::move(batch1));
    EXPECT_EQ(batch2.size(), 1);
    EXPECT_EQ(batch2[0].Header().instrument_id, 42);

    // Move assign
    RecordBatch batch3;
    batch3 = std::move(batch2);
    EXPECT_EQ(batch3.size(), 1);
    EXPECT_EQ(batch3[0].Header().instrument_id, 42);
}

TEST(RecordRefTest, HeaderAccess) {
    AlignedBuffer buffer;
    auto* msg = reinterpret_cast<MboMsg*>(buffer.data());
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;
    msg->hd.publisher_id = 7;
    msg->hd.instrument_id = 999;

    RecordRef ref;
    ref.data = buffer.data();
    ref.size = sizeof(MboMsg);
    ref.keepalive = buffer.MakeKeepalive();

    const auto& header = ref.Header();
    EXPECT_EQ(header.rtype, RType::Mbo);
    EXPECT_EQ(header.publisher_id, 7);
    EXPECT_EQ(header.instrument_id, 999);
    EXPECT_EQ(header.Size(), sizeof(MboMsg));
}

TEST(RecordRefTest, TypedAccessWithAs) {
    AlignedBuffer buffer;
    auto* msg = reinterpret_cast<MboMsg*>(buffer.data());
    msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
    msg->hd.rtype = RType::Mbo;
    msg->order_id = 12345;
    msg->price = 50 * kFixedPriceScale;
    msg->size = 100;
    msg->side = Side::Ask;
    msg->action = Action::Cancel;

    RecordRef ref;
    ref.data = buffer.data();
    ref.size = sizeof(MboMsg);
    ref.keepalive = buffer.MakeKeepalive();

    const auto& record = ref.As<MboMsg>();
    EXPECT_EQ(record.order_id, 12345);
    EXPECT_EQ(record.price, 50 * kFixedPriceScale);
    EXPECT_EQ(record.size, 100);
    EXPECT_EQ(record.side, Side::Ask);
    EXPECT_EQ(record.action, Action::Cancel);
}

TEST(RecordRefTest, KeepaliveOwnership) {
    bool destroyed = false;

    {
        RecordBatch batch;

        // Create a keepalive that sets a flag when destroyed
        auto keepalive = std::shared_ptr<bool>(new bool(true), [&destroyed](bool* p) {
            destroyed = true;
            delete p;
        });

        alignas(8) std::byte buffer[sizeof(MboMsg)] = {};
        auto* msg = reinterpret_cast<MboMsg*>(buffer);
        msg->hd.length = sizeof(MboMsg) / kRecordHeaderLengthMultiplier;
        msg->hd.rtype = RType::Mbo;

        RecordRef ref;
        ref.data = buffer;
        ref.size = sizeof(MboMsg);
        ref.keepalive = keepalive;
        batch.Add(std::move(ref));

        // Keepalive should still be alive
        EXPECT_FALSE(destroyed);
    }

    // After batch is destroyed, keepalive should be destroyed
    EXPECT_TRUE(destroyed);
}
