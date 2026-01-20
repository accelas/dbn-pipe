// tests/symbology_client_test.cpp
#include <gtest/gtest.h>

#include "src/api/symbology_client.hpp"

namespace dbn_pipe {
namespace {

TEST(SymbologyBuilderTest, ParsesSimpleMapping) {
    SymbologyBuilder builder;

    // {"result": {"SPY": [{"d0": "2025-01-01", "d1": "2025-12-31", "s": "15144"}]}}
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnKey("SPY");
    builder.OnStartArray();
    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("s");
    builder.OnString("15144");
    builder.OnEndObject();
    builder.OnEndArray();
    builder.OnEndObject();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->result.size(), 1);
    ASSERT_EQ(result->result.count("SPY"), 1);
    ASSERT_EQ(result->result.at("SPY").size(), 1);
    EXPECT_EQ(result->result.at("SPY")[0].start_date, "2025-01-01");
    EXPECT_EQ(result->result.at("SPY")[0].end_date, "2025-12-31");
    EXPECT_EQ(result->result.at("SPY")[0].symbol, "15144");
}

TEST(SymbologyBuilderTest, ParsesMultipleMappings) {
    SymbologyBuilder builder;

    // {"result": {"SPY": [...], "QQQ": [...]}}
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();

    // SPY mapping
    builder.OnKey("SPY");
    builder.OnStartArray();
    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnKey("d1");
    builder.OnString("2025-06-30");
    builder.OnKey("s");
    builder.OnString("15144");
    builder.OnEndObject();
    builder.OnEndArray();

    // QQQ mapping
    builder.OnKey("QQQ");
    builder.OnStartArray();
    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("s");
    builder.OnString("13340");
    builder.OnEndObject();
    builder.OnEndArray();

    builder.OnEndObject();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->result.size(), 2);
    EXPECT_EQ(result->result.at("SPY").size(), 1);
    EXPECT_EQ(result->result.at("QQQ").size(), 1);
    EXPECT_EQ(result->result.at("SPY")[0].symbol, "15144");
    EXPECT_EQ(result->result.at("QQQ")[0].symbol, "13340");
}

TEST(SymbologyBuilderTest, ParsesMultipleIntervalsForOneSymbol) {
    SymbologyBuilder builder;

    // Symbol with multiple time intervals (e.g., ticker changes)
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnKey("META");
    builder.OnStartArray();

    // First interval (as FB)
    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2020-01-01");
    builder.OnKey("d1");
    builder.OnString("2022-06-08");
    builder.OnKey("s");
    builder.OnString("FB");
    builder.OnEndObject();

    // Second interval (as META)
    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2022-06-09");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("s");
    builder.OnString("META");
    builder.OnEndObject();

    builder.OnEndArray();
    builder.OnEndObject();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->result.at("META").size(), 2);
    EXPECT_EQ(result->result.at("META")[0].start_date, "2020-01-01");
    EXPECT_EQ(result->result.at("META")[0].symbol, "FB");
    EXPECT_EQ(result->result.at("META")[1].start_date, "2022-06-09");
    EXPECT_EQ(result->result.at("META")[1].symbol, "META");
}

TEST(SymbologyBuilderTest, ParsesNotFound) {
    SymbologyBuilder builder;

    // {"result": {}, "not_found": ["INVALID1", "INVALID2"]}
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnEndObject();
    builder.OnKey("not_found");
    builder.OnStartArray();
    builder.OnString("INVALID1");
    builder.OnString("INVALID2");
    builder.OnEndArray();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->result.empty());
    ASSERT_EQ(result->not_found.size(), 2);
    EXPECT_EQ(result->not_found[0], "INVALID1");
    EXPECT_EQ(result->not_found[1], "INVALID2");
}

TEST(SymbologyBuilderTest, ParsesPartial) {
    SymbologyBuilder builder;

    // {"result": {}, "partial": ["AMBIGUOUS"]}
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnEndObject();
    builder.OnKey("partial");
    builder.OnStartArray();
    builder.OnString("AMBIGUOUS");
    builder.OnEndArray();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->result.empty());
    ASSERT_EQ(result->partial.size(), 1);
    EXPECT_EQ(result->partial[0], "AMBIGUOUS");
}

TEST(SymbologyBuilderTest, ParsesFullResponse) {
    SymbologyBuilder builder;

    // Full response with result, partial, and not_found
    builder.OnStartObject();

    // result
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnKey("SPY");
    builder.OnStartArray();
    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("s");
    builder.OnString("15144");
    builder.OnEndObject();
    builder.OnEndArray();
    builder.OnEndObject();

    // partial
    builder.OnKey("partial");
    builder.OnStartArray();
    builder.OnString("AMBIGUOUS1");
    builder.OnString("AMBIGUOUS2");
    builder.OnEndArray();

    // not_found
    builder.OnKey("not_found");
    builder.OnStartArray();
    builder.OnString("INVALID");
    builder.OnEndArray();

    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->result.size(), 1);
    EXPECT_EQ(result->partial.size(), 2);
    EXPECT_EQ(result->not_found.size(), 1);
}

TEST(SymbologyBuilderTest, EmptyResponseIsValid) {
    SymbologyBuilder builder;

    // {"result": {}, "partial": [], "not_found": []}
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnEndObject();
    builder.OnKey("partial");
    builder.OnStartArray();
    builder.OnEndArray();
    builder.OnKey("not_found");
    builder.OnStartArray();
    builder.OnEndArray();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->result.empty());
    EXPECT_TRUE(result->partial.empty());
    EXPECT_TRUE(result->not_found.empty());
}

TEST(SymbologyBuilderTest, StateTransitionsCorrectly) {
    SymbologyBuilder builder;

    EXPECT_EQ(builder.GetState(), SymbologyBuilder::State::Root);

    builder.OnStartObject();
    EXPECT_EQ(builder.GetState(), SymbologyBuilder::State::Root);

    builder.OnKey("result");
    builder.OnStartObject();
    EXPECT_EQ(builder.GetState(), SymbologyBuilder::State::InResult);

    builder.OnKey("SPY");
    builder.OnStartArray();
    EXPECT_EQ(builder.GetState(), SymbologyBuilder::State::InSymbolArray);

    builder.OnStartObject();
    EXPECT_EQ(builder.GetState(), SymbologyBuilder::State::InInterval);

    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("s");
    builder.OnString("15144");
    EXPECT_EQ(builder.GetState(), SymbologyBuilder::State::InInterval);

    builder.OnEndObject();
    EXPECT_EQ(builder.GetState(), SymbologyBuilder::State::InSymbolArray);

    builder.OnEndArray();
    EXPECT_EQ(builder.GetState(), SymbologyBuilder::State::InResult);

    builder.OnEndObject();
    EXPECT_EQ(builder.GetState(), SymbologyBuilder::State::Root);
}

TEST(SymbologyBuilderTest, IgnoresUnknownFieldsInInterval) {
    SymbologyBuilder builder;

    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnKey("SPY");
    builder.OnStartArray();
    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnKey("extra_field");
    builder.OnString("ignored");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("another_extra");
    builder.OnInt(12345);
    builder.OnKey("s");
    builder.OnString("15144");
    builder.OnEndObject();
    builder.OnEndArray();
    builder.OnEndObject();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->result.at("SPY")[0].start_date, "2025-01-01");
    EXPECT_EQ(result->result.at("SPY")[0].end_date, "2025-12-31");
    EXPECT_EQ(result->result.at("SPY")[0].symbol, "15144");
}

TEST(SymbologyBuilderTest, HandlesFieldsInAnyOrder) {
    SymbologyBuilder builder;

    // Fields in different order: s, d1, d0
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnKey("SPY");
    builder.OnStartArray();
    builder.OnStartObject();
    builder.OnKey("s");
    builder.OnString("15144");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnEndObject();
    builder.OnEndArray();
    builder.OnEndObject();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->result.at("SPY")[0].start_date, "2025-01-01");
    EXPECT_EQ(result->result.at("SPY")[0].end_date, "2025-12-31");
    EXPECT_EQ(result->result.at("SPY")[0].symbol, "15144");
}

TEST(SymbologyBuilderTest, HandlesNumericInstrumentId) {
    SymbologyBuilder builder;

    // When stype_out=instrument_id, the "s" field is a numeric value
    // {"result": {"SPY": [{"d0": "2025-01-01", "d1": "2025-12-31", "s": 15144}]}}
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnKey("SPY");
    builder.OnStartArray();
    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("s");
    builder.OnUint(15144);  // Numeric instrument_id
    builder.OnEndObject();
    builder.OnEndArray();
    builder.OnEndObject();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->result.size(), 1);
    ASSERT_EQ(result->result.at("SPY").size(), 1);
    EXPECT_EQ(result->result.at("SPY")[0].start_date, "2025-01-01");
    EXPECT_EQ(result->result.at("SPY")[0].end_date, "2025-12-31");
    EXPECT_EQ(result->result.at("SPY")[0].symbol, "15144");
}

TEST(SymbologyBuilderTest, HandlesNegativeInstrumentId) {
    SymbologyBuilder builder;

    // Edge case: negative instrument_id (unlikely but should handle)
    builder.OnStartObject();
    builder.OnKey("result");
    builder.OnStartObject();
    builder.OnKey("TEST");
    builder.OnStartArray();
    builder.OnStartObject();
    builder.OnKey("d0");
    builder.OnString("2025-01-01");
    builder.OnKey("d1");
    builder.OnString("2025-12-31");
    builder.OnKey("s");
    builder.OnInt(-1);  // Negative value via OnInt
    builder.OnEndObject();
    builder.OnEndArray();
    builder.OnEndObject();
    builder.OnEndObject();

    auto result = builder.Build();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->result.at("TEST")[0].symbol, "-1");
}

}  // namespace
}  // namespace dbn_pipe
