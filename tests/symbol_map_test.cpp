// SPDX-License-Identifier: MIT

// tests/symbol_map_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include <databento/record.hpp>

#include "src/symbol_map.hpp"

using namespace dbn_pipe;

class SymbolMapTest : public ::testing::Test {};

TEST_F(SymbolMapTest, FindReturnsEmptyForUnknownId) {
    SymbolMap map;
    EXPECT_EQ(map.Find(12345), "");
}

TEST_F(SymbolMapTest, OnRecordIgnoresNonSymbolMappingRecords) {
    SymbolMap map;

    // Create an MBP record (not a symbol mapping)
    std::vector<std::byte> record(sizeof(databento::Mbp1Msg));
    auto* msg = reinterpret_cast<databento::Mbp1Msg*>(record.data());
    msg->hd.length = sizeof(databento::Mbp1Msg) / databento::RecordHeader::kLengthMultiplier;
    msg->hd.rtype = databento::RType::Mbp1;
    msg->hd.instrument_id = 100;

    databento::Record rec{&msg->hd};
    map.OnRecord(rec);

    EXPECT_EQ(map.Find(100), "");
}

TEST_F(SymbolMapTest, OnRecordExtractsSymbolFromSymbolMappingMsg) {
    SymbolMap map;

    // Create a SymbolMappingMsg record
    std::vector<std::byte> record(sizeof(databento::SymbolMappingMsg));
    std::memset(record.data(), 0, record.size());

    auto* msg = reinterpret_cast<databento::SymbolMappingMsg*>(record.data());
    msg->hd.length = sizeof(databento::SymbolMappingMsg) / databento::RecordHeader::kLengthMultiplier;
    msg->hd.rtype = databento::RType::SymbolMapping;
    msg->hd.instrument_id = 42;

    // Set the symbol in stype_out_symbol field (the resolved/output symbol)
    const char* symbol = "AAPL";
    std::strncpy(msg->stype_out_symbol.data(), symbol, msg->stype_out_symbol.size());

    databento::Record rec{&msg->hd};
    map.OnRecord(rec);

    EXPECT_EQ(map.Find(42), "AAPL");
}

TEST_F(SymbolMapTest, OnRecordUpdatesExistingMapping) {
    SymbolMap map;

    auto make_symbol_mapping = [](uint32_t id, const char* symbol) {
        static std::vector<std::byte> record(sizeof(databento::SymbolMappingMsg));
        std::memset(record.data(), 0, record.size());

        auto* msg = reinterpret_cast<databento::SymbolMappingMsg*>(record.data());
        msg->hd.length = sizeof(databento::SymbolMappingMsg) / databento::RecordHeader::kLengthMultiplier;
        msg->hd.rtype = databento::RType::SymbolMapping;
        msg->hd.instrument_id = id;
        std::strncpy(msg->stype_out_symbol.data(), symbol, msg->stype_out_symbol.size());

        return databento::Record{&msg->hd};
    };

    map.OnRecord(make_symbol_mapping(42, "AAPL"));
    EXPECT_EQ(map.Find(42), "AAPL");

    // Same instrument_id, new symbol (e.g., contract roll)
    map.OnRecord(make_symbol_mapping(42, "AAPL.1"));
    EXPECT_EQ(map.Find(42), "AAPL.1");
}

TEST_F(SymbolMapTest, SizeReturnsNumberOfMappings) {
    SymbolMap map;
    EXPECT_EQ(map.Size(), 0);

    auto make_symbol_mapping = [](uint32_t id, const char* symbol) {
        static std::vector<std::byte> record(sizeof(databento::SymbolMappingMsg));
        std::memset(record.data(), 0, record.size());

        auto* msg = reinterpret_cast<databento::SymbolMappingMsg*>(record.data());
        msg->hd.length = sizeof(databento::SymbolMappingMsg) / databento::RecordHeader::kLengthMultiplier;
        msg->hd.rtype = databento::RType::SymbolMapping;
        msg->hd.instrument_id = id;
        std::strncpy(msg->stype_out_symbol.data(), symbol, msg->stype_out_symbol.size());

        return databento::Record{&msg->hd};
    };

    map.OnRecord(make_symbol_mapping(1, "AAPL"));
    EXPECT_EQ(map.Size(), 1);

    map.OnRecord(make_symbol_mapping(2, "MSFT"));
    EXPECT_EQ(map.Size(), 2);

    // Updating existing mapping should not increase size
    map.OnRecord(make_symbol_mapping(1, "AAPL.1"));
    EXPECT_EQ(map.Size(), 2);
}

TEST_F(SymbolMapTest, ClearRemovesAllMappings) {
    SymbolMap map;

    auto make_symbol_mapping = [](uint32_t id, const char* symbol) {
        static std::vector<std::byte> record(sizeof(databento::SymbolMappingMsg));
        std::memset(record.data(), 0, record.size());

        auto* msg = reinterpret_cast<databento::SymbolMappingMsg*>(record.data());
        msg->hd.length = sizeof(databento::SymbolMappingMsg) / databento::RecordHeader::kLengthMultiplier;
        msg->hd.rtype = databento::RType::SymbolMapping;
        msg->hd.instrument_id = id;
        std::strncpy(msg->stype_out_symbol.data(), symbol, msg->stype_out_symbol.size());

        return databento::Record{&msg->hd};
    };

    map.OnRecord(make_symbol_mapping(1, "AAPL"));
    map.OnRecord(make_symbol_mapping(2, "MSFT"));
    EXPECT_EQ(map.Size(), 2);

    map.Clear();

    EXPECT_EQ(map.Size(), 0);
    EXPECT_EQ(map.Find(1), "");
    EXPECT_EQ(map.Find(2), "");
}
