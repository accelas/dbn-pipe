// lib/stream/http_request_builder.hpp
#pragma once

#include <concepts>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "url_encode.hpp"

namespace dbn_pipe {

// HttpRequestBuilder - Builds HTTP/1.1 requests to any output stream
//
// Template parameter Stream must support operator<< for string_view and char.
// Examples: std::ostringstream, std::ospanstream
//
// Usage:
//   std::ostringstream out;
//   HttpRequestBuilder(out)
//       .Method("GET")
//       .Path("/v0/endpoint")
//       .QueryParam("key", "value")
//       .Host("api.example.com")
//       .BasicAuth(api_key)
//       .Header("Accept", "application/json")
//       .Finish();
//
// Zero-copy usage with ospanstream:
//   std::ospanstream out(std::span<char>(buffer, size));
//   HttpRequestBuilder(out)
//       .Method("GET")
//       ...
//       .Finish();
//
template<typename Stream>
class HttpRequestBuilder {
public:
    explicit HttpRequestBuilder(Stream& out) : out_(out) {}

    // Set HTTP method (GET, POST, etc.)
    HttpRequestBuilder& Method(std::string_view method) {
        out_ << method;
        return *this;
    }

    // Set request path (e.g., "/v0/timeseries.get_range")
    HttpRequestBuilder& Path(std::string_view path) {
        out_ << " " << path;
        return *this;
    }

    // Add a query parameter (URL-encoded)
    HttpRequestBuilder& QueryParam(std::string_view key, std::string_view value) {
        out_ << (first_param_ ? "?" : "&");
        first_param_ = false;
        UrlEncode(out_, key);
        out_ << "=";
        UrlEncode(out_, value);
        return *this;
    }

    // Add a query parameter with numeric value
    template<typename T>
        requires std::integral<T> || std::floating_point<T>
    HttpRequestBuilder& QueryParam(std::string_view key, T value) {
        out_ << (first_param_ ? "?" : "&");
        first_param_ = false;
        UrlEncode(out_, key);
        out_ << "=" << value;
        return *this;
    }

    // End the request line and start headers
    HttpRequestBuilder& EndRequestLine() {
        out_ << " HTTP/1.1\r\n";
        headers_started_ = true;
        return *this;
    }

    // Add Host header
    HttpRequestBuilder& Host(std::string_view host) {
        EnsureHeadersStarted();
        out_ << "Host: " << host << "\r\n";
        return *this;
    }

    // Add Basic Authorization header
    HttpRequestBuilder& BasicAuth(std::string_view api_key) {
        EnsureHeadersStarted();
        out_ << "Authorization: Basic ";
        // Databento API expects api_key + ":" (empty password)
        std::string credentials{api_key};
        credentials += ':';
        Base64Encode(out_, credentials);
        out_ << "\r\n";
        return *this;
    }

    // Add arbitrary header
    HttpRequestBuilder& Header(std::string_view name, std::string_view value) {
        EnsureHeadersStarted();
        out_ << name << ": " << value << "\r\n";
        return *this;
    }

    // Add form-urlencoded body (for POST requests)
    // Sets Content-Type and Content-Length headers, then writes body
    HttpRequestBuilder& FormBody(
        std::span<const std::pair<std::string, std::string>> params
    ) {
        EnsureHeadersStarted();

        // Build body first to get length
        std::string body;
        bool first = true;
        for (const auto& [key, value] : params) {
            if (!first) body += '&';
            first = false;
            // URL encode into temporary string
            std::ostringstream tmp;
            UrlEncode(tmp, key);
            body += tmp.str();
            body += '=';
            tmp.str("");
            UrlEncode(tmp, value);
            body += tmp.str();
        }

        out_ << "Content-Type: application/x-www-form-urlencoded\r\n";
        out_ << "Content-Length: " << body.size() << "\r\n";
        out_ << "\r\n";
        out_ << body;
        body_written_ = true;
        return *this;
    }

    // Finish the request (writes final CRLF if no body)
    void Finish() {
        EnsureHeadersStarted();
        if (!body_written_) {
            out_ << "\r\n";
        }
    }

private:
    void EnsureHeadersStarted() {
        if (!headers_started_) {
            out_ << " HTTP/1.1\r\n";
            headers_started_ = true;
        }
    }

    Stream& out_;
    bool first_param_ = true;
    bool headers_started_ = false;
    bool body_written_ = false;
};

// Deduction guide
template<typename Stream>
HttpRequestBuilder(Stream&) -> HttpRequestBuilder<Stream>;

}  // namespace dbn_pipe
