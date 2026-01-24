// SPDX-License-Identifier: MIT

// tests/http_request_builder_test.cpp
#include <gtest/gtest.h>

#include <iterator>
#include <string>

#include "lib/stream/http_request_builder.hpp"

using namespace dbn_pipe;

TEST(HttpRequestBuilderTest, SimpleGetRequest) {
    std::string out;
    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .Path("/v0/endpoint")
        .Host("api.example.com")
        .Header("Connection", "close")
        .Finish();

    std::string expected =
        "GET /v0/endpoint HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "Connection: close\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, GetWithQueryParams) {
    std::string out;
    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .Path("/v0/data")
        .QueryParam("dataset", "GLBX.MDP3")
        .QueryParam("schema", "mbp-1")
        .Host("hist.databento.com")
        .Finish();

    std::string expected =
        "GET /v0/data?dataset=GLBX.MDP3&schema=mbp-1 HTTP/1.1\r\n"
        "Host: hist.databento.com\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, QueryParamUrlEncoding) {
    std::string out;
    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .Path("/search")
        .QueryParam("q", "hello world")
        .QueryParam("special", "a&b=c")
        .Host("example.com")
        .Finish();

    std::string expected =
        "GET /search?q=hello%20world&special=a%26b%3Dc HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, NumericQueryParam) {
    std::string out;
    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .Path("/v0/timeseries")
        .QueryParam("start", 1704067200000000000ULL)
        .QueryParam("end", 1704153600000000000ULL)
        .Host("hist.databento.com")
        .Finish();

    std::string expected =
        "GET /v0/timeseries?start=1704067200000000000&end=1704153600000000000 HTTP/1.1\r\n"
        "Host: hist.databento.com\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, BasicAuth) {
    std::string out;
    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .Path("/v0/data")
        .Host("api.example.com")
        .BasicAuth("test_api_key")
        .Finish();

    // base64("test_api_key:") = "dGVzdF9hcGlfa2V5Og=="
    std::string expected =
        "GET /v0/data HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "Authorization: Basic dGVzdF9hcGlfa2V5Og==\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, MultipleHeaders) {
    std::string out;
    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .Path("/v0/data")
        .Host("api.example.com")
        .Header("Accept", "application/json")
        .Header("Accept-Encoding", "gzip")
        .Header("Connection", "close")
        .Finish();

    std::string expected =
        "GET /v0/data HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "Accept: application/json\r\n"
        "Accept-Encoding: gzip\r\n"
        "Connection: close\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, PostWithFormBody) {
    std::string out;
    std::vector<std::pair<std::string, std::string>> params = {
        {"username", "test"},
        {"password", "secret"}
    };

    HttpRequestBuilder(std::back_inserter(out))
        .Method("POST")
        .Path("/login")
        .Host("api.example.com")
        .FormBody(params);

    std::string expected =
        "POST /login HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 29\r\n"
        "\r\n"
        "username=test&password=secret";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, HistoricalStyleRequest) {
    std::string out;
    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .Path("/v0/timeseries.get_range")
        .QueryParam("dataset", "GLBX.MDP3")
        .QueryParam("symbols", "ESZ4")
        .QueryParam("schema", "mbp-1")
        .QueryParam("start", 1704067200000000000ULL)
        .QueryParam("end", 1704153600000000000ULL)
        .QueryParam("encoding", "dbn")
        .QueryParam("compression", "zstd")
        .Host("hist.databento.com")
        .BasicAuth("my_api_key")
        .Header("Accept", "application/octet-stream")
        .Header("Accept-Encoding", "zstd")
        .Header("Connection", "close")
        .Finish();

    // Verify it starts correctly
    EXPECT_TRUE(out.starts_with("GET /v0/timeseries.get_range?"));
    EXPECT_TRUE(out.find("dataset=GLBX.MDP3") != std::string::npos);
    EXPECT_TRUE(out.find("symbols=ESZ4") != std::string::npos);
    EXPECT_TRUE(out.find("Host: hist.databento.com") != std::string::npos);
    EXPECT_TRUE(out.find("Authorization: Basic") != std::string::npos);
    EXPECT_TRUE(out.find("Accept: application/octet-stream") != std::string::npos);
    EXPECT_TRUE(out.ends_with("\r\n\r\n"));
}

TEST(HttpRequestBuilderTest, ChainedBuilderWithConditionals) {
    std::string out;
    auto builder = HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .Path("/v0/data")
        .QueryParam("required", "value");

    // Conditionally add params
    bool add_optional = true;
    if (add_optional) {
        builder.QueryParam("optional", "yes");
    }

    builder.Host("example.com").Finish();

    EXPECT_TRUE(out.find("required=value") != std::string::npos);
    EXPECT_TRUE(out.find("optional=yes") != std::string::npos);
}

TEST(HttpRequestBuilderTest, PathTemplateSubstitution) {
    std::string out;
    std::vector<std::pair<std::string, std::string>> params = {
        {"ticker", "AAPL"},
        {"start", "2024-01-01"},
        {"end", "2024-01-31"}
    };

    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .PathTemplate("/v2/aggs/ticker/{ticker}/range/1/day/{start}/{end}", params)
        .Host("api.polygon.io")
        .Finish();

    std::string expected =
        "GET /v2/aggs/ticker/AAPL/range/1/day/2024-01-01/2024-01-31 HTTP/1.1\r\n"
        "Host: api.polygon.io\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, PathTemplateNoParams) {
    std::string out;
    std::vector<std::pair<std::string, std::string>> params;

    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .PathTemplate("/v0/data", params)
        .Host("example.com")
        .Finish();

    std::string expected =
        "GET /v0/data HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, PathTemplateMissingParam) {
    std::string out;
    std::vector<std::pair<std::string, std::string>> params = {
        {"ticker", "AAPL"}
        // "date" is missing
    };

    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .PathTemplate("/v2/ticker/{ticker}/date/{date}", params)
        .Host("example.com")
        .Finish();

    // Missing param results in empty substitution
    std::string expected =
        "GET /v2/ticker/AAPL/date/ HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}

TEST(HttpRequestBuilderTest, PathTemplateWithQueryParams) {
    std::string out;
    std::vector<std::pair<std::string, std::string>> path_params = {
        {"symbol", "SPY"}
    };

    HttpRequestBuilder(std::back_inserter(out))
        .Method("GET")
        .PathTemplate("/v1/quote/{symbol}", path_params)
        .QueryParam("apiKey", "test123")
        .Host("api.example.com")
        .Finish();

    std::string expected =
        "GET /v1/quote/SPY?apiKey=test123 HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "\r\n";

    EXPECT_EQ(out, expected);
}
