// SPDX-License-Identifier: MIT

// tests/api_protocol_concept_test.cpp
#include <gtest/gtest.h>

#include <expected>
#include <string>

#include "lib/stream/protocol.hpp"
#include "src/api_protocol.hpp"

using namespace dbn_pipe;

// Mock builder for testing
struct MockBuilder {
    using Result = std::string;

    void OnKey(std::string_view) {}
    void OnString(std::string_view s) { result = std::string(s); }
    void OnInt(int64_t) {}
    void OnUint(uint64_t) {}
    void OnDouble(double) {}
    void OnBool(bool) {}
    void OnNull() {}
    void OnStartObject() {}
    void OnEndObject() {}
    void OnStartArray() {}
    void OnEndArray() {}
    std::expected<Result, std::string> Build() { return result; }

    std::string result;
};

static_assert(JsonBuilder<MockBuilder>, "MockBuilder must satisfy JsonBuilder concept");

TEST(ApiProtocolConceptTest, SatisfiesProtocolConcept) {
    static_assert(Protocol<ApiProtocol<MockBuilder>>);
    SUCCEED();
}

TEST(ApiProtocolConceptTest, RequestHasRequiredFields) {
    ApiRequest req;
    req.method = "GET";
    req.path = "/v0/test";
    req.host = "hist.databento.com";
    req.port = 443;
    req.query_params = {{"key", "value"}};

    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.path, "/v0/test");
    EXPECT_EQ(req.host, "hist.databento.com");
    EXPECT_EQ(req.port, 443);
    EXPECT_EQ(req.query_params.size(), 1);
}

TEST(ApiProtocolConceptTest, GetHostnameReturnsRequestHost) {
    ApiRequest req;
    req.host = "api.example.com";

    auto hostname = ApiProtocol<MockBuilder>::GetHostname(req);
    EXPECT_EQ(hostname, "api.example.com");
}

TEST(ApiProtocolConceptTest, GetPortReturnsRequestPort) {
    ApiRequest req;
    req.port = 8080;

    auto port = ApiProtocol<MockBuilder>::GetPort(req);
    EXPECT_EQ(port, 8080);
}
