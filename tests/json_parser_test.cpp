// tests/json_parser_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <expected>
#include <optional>
#include <string>

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/error.hpp"
#include "src/api/json_parser.hpp"

namespace dbn_pipe {
namespace {

// Simple builder that extracts {"value": N} from JSON
struct IntBuilder {
    using Result = int64_t;
    std::optional<int64_t> value;
    std::string current_key;

    void OnKey(std::string_view key) { current_key = key; }
    void OnInt(int64_t v) {
        if (current_key == "value") value = v;
    }
    void OnUint(uint64_t v) {
        if (current_key == "value") value = static_cast<int64_t>(v);
    }
    void OnString(std::string_view) {}
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (!value) return std::unexpected("missing 'value'");
        return *value;
    }
};

// Helper to feed JSON string to parser
void FeedJson(JsonParser<IntBuilder>& parser, const std::string& json) {
    BufferChain chain;
    auto seg = std::make_shared<Segment>();
    std::memcpy(seg->data.data(), json.data(), json.size());
    seg->size = json.size();
    chain.Append(std::move(seg));
    parser.OnData(chain);
}

TEST(JsonParserTest, ParsesSimpleObject) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder, [&](auto r) {
            result = std::move(r);
            called = true;
        });

    FeedJson(*parser, R"({"value": 42})");
    parser->OnDone();

    ASSERT_TRUE(called);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(JsonParserTest, HandlesMissingField) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder, [&](auto r) {
            result = std::move(r);
            called = true;
        });

    FeedJson(*parser, R"({"other": 123})");
    parser->OnDone();

    ASSERT_TRUE(called);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::ParseError);
    EXPECT_TRUE(result.error().message.find("missing 'value'") != std::string::npos);
}

TEST(JsonParserTest, BestEffortOnError) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder, [&](auto r) {
            result = std::move(r);
            called = true;
        });

    // Feed complete JSON, then signal error
    FeedJson(*parser, R"({"value": 99})");
    parser->OnError(Error{ErrorCode::ConnectionClosed, "connection lost"});

    ASSERT_TRUE(called);
    // Should succeed with best-effort result since JSON is complete
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 99);
}

TEST(JsonParserTest, ErrorWhenIncomplete) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder, [&](auto r) {
            result = std::move(r);
            called = true;
        });

    // Feed incomplete JSON, then signal error
    FeedJson(*parser, R"({"value": )");
    parser->OnError(Error{ErrorCode::ConnectionClosed, "connection lost"});

    ASSERT_TRUE(called);
    // Should propagate original error since Build() fails on incomplete data
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::ConnectionClosed);
}

TEST(JsonParserTest, ParsesChunkedInput) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder, [&](auto r) {
            result = std::move(r);
            called = true;
        });

    // Feed JSON in multiple chunks
    FeedJson(*parser, R"({"val)");
    FeedJson(*parser, R"(ue": 12)");
    FeedJson(*parser, R"(34})");
    parser->OnDone();

    ASSERT_TRUE(called);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1234);
}

TEST(JsonParserTest, HandlesNegativeIntegers) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder, [&](auto r) {
            result = std::move(r);
            called = true;
        });

    FeedJson(*parser, R"({"value": -42})");
    parser->OnDone();

    ASSERT_TRUE(called);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, -42);
}

TEST(JsonParserTest, HandlesLargeIntegers) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder, [&](auto r) {
            result = std::move(r);
            called = true;
        });

    // Use a large int64 value that fits in signed range
    // (rapidjson routes INT64_MAX through Uint64 since it doesn't have sign bit)
    FeedJson(*parser, R"({"value": 4611686018427387903})");
    parser->OnDone();

    ASSERT_TRUE(called);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 4611686018427387903LL);
}

TEST(JsonParserTest, IgnoresMalformedJson) {
    IntBuilder builder;
    std::expected<int64_t, Error> result;
    bool called = false;

    auto parser = JsonParser<IntBuilder>::Create(
        builder, [&](auto r) {
            result = std::move(r);
            called = true;
        });

    FeedJson(*parser, R"({invalid json})");
    parser->OnDone();

    ASSERT_TRUE(called);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::ParseError);
}

TEST(JsonParserTest, OnlyCallsCallbackOnce) {
    IntBuilder builder;
    int call_count = 0;

    auto parser = JsonParser<IntBuilder>::Create(
        builder, [&](auto) { ++call_count; });

    FeedJson(*parser, R"({"value": 1})");
    parser->OnDone();
    parser->OnDone();  // Second call should be ignored
    parser->OnError(Error{ErrorCode::ConnectionClosed, "test"});  // Should be ignored

    EXPECT_EQ(call_count, 1);
}

// More complex builder for testing nested structures
struct NestedBuilder {
    using Result = std::pair<std::string, int64_t>;
    std::string name;
    int64_t count = 0;
    std::string current_key;
    int depth = 0;

    void OnKey(std::string_view key) { current_key = key; }
    void OnInt(int64_t v) {
        if (current_key == "count") count = v;
    }
    void OnUint(uint64_t v) {
        if (current_key == "count") count = static_cast<int64_t>(v);
    }
    void OnString(std::string_view s) {
        if (current_key == "name") name = s;
    }
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() { ++depth; }
    void OnEndObject() { --depth; }
    void OnStartArray() {}
    void OnEndArray() {}

    std::expected<Result, std::string> Build() {
        if (name.empty()) return std::unexpected("missing 'name'");
        return std::make_pair(name, count);
    }
};

TEST(JsonParserTest, ParsesNestedObject) {
    NestedBuilder builder;
    std::expected<std::pair<std::string, int64_t>, Error> result;
    bool called = false;

    auto parser = JsonParser<NestedBuilder>::Create(
        builder, [&](auto r) {
            result = std::move(r);
            called = true;
        });

    BufferChain chain;
    auto seg = std::make_shared<Segment>();
    std::string json = R"({"data": {"name": "test", "count": 5}})";
    std::memcpy(seg->data.data(), json.data(), json.size());
    seg->size = json.size();
    chain.Append(std::move(seg));
    parser->OnData(chain);
    parser->OnDone();

    ASSERT_TRUE(called);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "test");
    EXPECT_EQ(result->second, 5);
}

}  // namespace
}  // namespace dbn_pipe
