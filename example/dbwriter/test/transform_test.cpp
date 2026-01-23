// SPDX-License-Identifier: MIT

#include "dbwriter/transform.hpp"
#include "dbwriter/table.hpp"
#include "dbwriter/pg_types.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace dbwriter {

// Mock instrument map - needs to be in dbwriter namespace for IInstrumentMap
class MockInstrumentMap : public IInstrumentMap {
public:
    MOCK_METHOD(std::optional<uint32_t>, underlying, (uint32_t), (const, override));
};

// Sample source record - needs to be in dbwriter namespace for specialization
struct TradeRecord {
    int64_t ts_event_ns = 0;
    int32_t instrument_id = 0;
    int64_t price = 0;
    int32_t size = 0;
};

// Target table
constexpr auto trades_table = Table{"trades",
    Column<"ts_event_ns", int64_t, pg::BigInt>{},
    Column<"ts_event", Timestamp, pg::Timestamptz>{},
    Column<"instrument_id", int32_t, pg::Integer>{},
    Column<"underlying_id", int32_t, pg::Integer>{},
    Column<"price", int64_t, pg::BigInt>{},
    Column<"size", int32_t, pg::Integer>{},
};

using TradesRow = decltype(trades_table)::RowType;

// Transform specialization - must be in dbwriter namespace
template <>
struct Transform<TradeRecord, decltype(trades_table)> {
    const IInstrumentMap& instruments;

    TradesRow operator()(const TradeRecord& rec) const {
        TradesRow row;
        row.get<"ts_event_ns">() = rec.ts_event_ns;
        row.get<"ts_event">() = Timestamp::from_unix_ns(rec.ts_event_ns);
        row.get<"instrument_id">() = rec.instrument_id;
        row.get<"underlying_id">() = instruments.underlying(rec.instrument_id).value_or(0);
        row.get<"price">() = rec.price;
        row.get<"size">() = rec.size;
        return row;
    }
};

namespace {

TEST(TransformTest, TransformsBasicFields) {
    MockInstrumentMap instruments;
    EXPECT_CALL(instruments, underlying(1234))
        .WillOnce(::testing::Return(std::optional<uint32_t>{5678}));

    Transform<TradeRecord, decltype(trades_table)> transform{instruments};

    TradeRecord rec{
        .ts_event_ns = 1704067200000000000LL,
        .instrument_id = 1234,
        .price = 150'000'000'000LL,
        .size = 100,
    };

    auto row = transform(rec);

    EXPECT_EQ(row.get<"ts_event_ns">(), 1704067200000000000LL);
    EXPECT_EQ(row.get<"instrument_id">(), 1234);
    EXPECT_EQ(row.get<"underlying_id">(), 5678);
    EXPECT_EQ(row.get<"price">(), 150'000'000'000LL);
    EXPECT_EQ(row.get<"size">(), 100);
}

TEST(TransformTest, DerivesTimestamp) {
    MockInstrumentMap instruments;
    EXPECT_CALL(instruments, underlying(::testing::_))
        .WillRepeatedly(::testing::Return(std::optional<uint32_t>{0}));

    Transform<TradeRecord, decltype(trades_table)> transform{instruments};

    // PG epoch: 2000-01-01 00:00:00 UTC
    int64_t pg_epoch_ns = 946684800000000000LL;

    TradeRecord rec{.ts_event_ns = pg_epoch_ns, .instrument_id = 0, .price = 0, .size = 0};
    auto row = transform(rec);

    // ts_event should be 0 (PG epoch)
    EXPECT_EQ(row.get<"ts_event">().to_pg_timestamp(), 0);
}

TEST(TransformTest, LookupFailure_ReturnsZero) {
    MockInstrumentMap instruments;
    EXPECT_CALL(instruments, underlying(9999))
        .WillOnce(::testing::Return(std::nullopt));

    Transform<TradeRecord, decltype(trades_table)> transform{instruments};

    TradeRecord rec{.ts_event_ns = 0, .instrument_id = 9999, .price = 0, .size = 0};
    auto row = transform(rec);

    EXPECT_EQ(row.get<"underlying_id">(), 0);
}

}  // namespace
}  // namespace dbwriter
