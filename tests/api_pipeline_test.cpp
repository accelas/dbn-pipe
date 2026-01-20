#include <gtest/gtest.h>

#include <expected>
#include <optional>
#include <string>

#include "src/api/api_pipeline.hpp"

namespace dbn_pipe {
namespace {

TEST(ApiRequestTest, BuildsGetRequest) {
    ApiRequest req;
    req.method = "GET";
    req.path = "/v0/metadata.get_dataset_range";
    req.query_params = {{"dataset", "GLBX.MDP3"}};

    std::string http = req.BuildHttpRequest("hist.databento.com", "db-test123");

    EXPECT_NE(http.find("GET /v0/metadata.get_dataset_range?dataset=GLBX.MDP3"), std::string::npos);
    EXPECT_NE(http.find("Host: hist.databento.com"), std::string::npos);
    EXPECT_NE(http.find("Authorization: Basic"), std::string::npos);
}

TEST(ApiRequestTest, BuildsPostRequest) {
    ApiRequest req;
    req.method = "POST";
    req.path = "/v0/metadata.get_record_count";
    req.form_params = {
        {"dataset", "GLBX.MDP3"},
        {"symbols", "ESM4"},
        {"schema", "trades"},
    };

    std::string http = req.BuildHttpRequest("hist.databento.com", "db-test123");

    EXPECT_NE(http.find("POST /v0/metadata.get_record_count"), std::string::npos);
    EXPECT_NE(http.find("Content-Type: application/x-www-form-urlencoded"), std::string::npos);
    EXPECT_NE(http.find("dataset=GLBX.MDP3"), std::string::npos);
}

TEST(ApiRequestTest, UrlEncodesParams) {
    ApiRequest req;
    req.method = "GET";
    req.path = "/test";
    req.query_params = {{"symbols", "SPY,QQQ"}};

    std::string http = req.BuildHttpRequest("example.com", "key");

    EXPECT_NE(http.find("symbols=SPY%2CQQQ"), std::string::npos);
}

TEST(ApiRequestTest, MultipleQueryParams) {
    ApiRequest req;
    req.method = "GET";
    req.path = "/v0/metadata.list_schemas";
    req.query_params = {
        {"dataset", "GLBX.MDP3"},
        {"start_date", "2024-01-01"},
        {"end_date", "2024-01-31"},
    };

    std::string http = req.BuildHttpRequest("hist.databento.com", "db-key");

    // Verify path with query string
    EXPECT_NE(http.find("GET /v0/metadata.list_schemas?"), std::string::npos);
    // Verify all params present
    EXPECT_NE(http.find("dataset=GLBX.MDP3"), std::string::npos);
    EXPECT_NE(http.find("start_date=2024-01-01"), std::string::npos);
    EXPECT_NE(http.find("end_date=2024-01-31"), std::string::npos);
    // Verify params are separated by &
    EXPECT_NE(http.find("&"), std::string::npos);
}

TEST(ApiRequestTest, BasicAuthEncoding) {
    ApiRequest req;
    req.method = "GET";
    req.path = "/test";

    std::string http = req.BuildHttpRequest("example.com", "myapikey");

    // API key with colon suffix: "myapikey:" -> base64 encoded
    // "myapikey:" in base64 is "bXlhcGlrZXk6"
    EXPECT_NE(http.find("Authorization: Basic bXlhcGlrZXk6"), std::string::npos);
}

TEST(ApiRequestTest, IncludesRequiredHeaders) {
    ApiRequest req;
    req.method = "GET";
    req.path = "/test";

    std::string http = req.BuildHttpRequest("api.example.com", "key");

    // Check for required headers
    EXPECT_NE(http.find("Host: api.example.com"), std::string::npos);
    EXPECT_NE(http.find("Accept: application/json"), std::string::npos);
    EXPECT_NE(http.find("Connection: close"), std::string::npos);
}

TEST(ApiRequestTest, PostWithContentLength) {
    ApiRequest req;
    req.method = "POST";
    req.path = "/submit";
    req.form_params = {{"key", "value"}};

    std::string http = req.BuildHttpRequest("example.com", "key");

    // POST requests should have Content-Length header
    EXPECT_NE(http.find("Content-Length:"), std::string::npos);
    // Body should be separated from headers by double CRLF
    EXPECT_NE(http.find("\r\n\r\n"), std::string::npos);
}

TEST(ApiRequestTest, EmptyQueryParams) {
    ApiRequest req;
    req.method = "GET";
    req.path = "/simple";

    std::string http = req.BuildHttpRequest("example.com", "key");

    // Path should not have query string marker
    EXPECT_NE(http.find("GET /simple HTTP/1.1"), std::string::npos);
    EXPECT_EQ(http.find("GET /simple?"), std::string::npos);
}

}  // namespace
}  // namespace dbn_pipe
