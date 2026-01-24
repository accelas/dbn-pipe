// SPDX-License-Identifier: MIT

// lib/stream/http_request_builder.hpp
#pragma once

#include <concepts>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "url_encode.hpp"

namespace dbn_pipe {

// HttpRequestBuilder - Builds HTTP/1.1 requests to any output iterator
//
// Template parameter OutputIt must be an output iterator accepting char.
// Examples: std::back_insert_iterator<std::string>, char*
//
// Usage:
//   std::string out;
//   HttpRequestBuilder(std::back_inserter(out))
//       .Method("GET")
//       .Path("/v0/endpoint")
//       .QueryParam("key", "value")
//       .Host("api.example.com")
//       .BasicAuth(api_key)
//       .Header("Accept", "application/json")
//       .Finish();
//
template<typename OutputIt>
class HttpRequestBuilder {
public:
    explicit HttpRequestBuilder(OutputIt out) : out_(out) {}

    // Set HTTP method (GET, POST, etc.)
    HttpRequestBuilder& Method(std::string_view method) {
        out_ = fmt::format_to(out_, "{}", method);
        return *this;
    }

    // Set request path (e.g., "/v0/timeseries.get_range")
    HttpRequestBuilder& Path(std::string_view path) {
        out_ = fmt::format_to(out_, " {}", path);
        return *this;
    }

    // Set request path with parameter substitution
    // Template syntax: {name} is replaced with corresponding value from params
    // Missing params result in empty substitution
    HttpRequestBuilder& PathTemplate(
        std::string_view path_template,
        std::span<const std::pair<std::string, std::string>> params
    ) {
        *out_++ = ' ';

        size_t pos = 0;
        while (pos < path_template.size()) {
            size_t brace = path_template.find('{', pos);
            if (brace == std::string_view::npos) {
                out_ = fmt::format_to(out_, "{}", path_template.substr(pos));
                break;
            }

            // Copy literal part before placeholder
            if (brace > pos) {
                out_ = fmt::format_to(out_, "{}", path_template.substr(pos, brace - pos));
            }

            // Find closing brace
            size_t end_brace = path_template.find('}', brace);
            if (end_brace == std::string_view::npos) {
                // Malformed template, copy rest as-is
                out_ = fmt::format_to(out_, "{}", path_template.substr(brace));
                break;
            }

            // Extract param name and substitute
            auto name = path_template.substr(brace + 1, end_brace - brace - 1);
            for (const auto& [key, value] : params) {
                if (key == name) {
                    out_ = fmt::format_to(out_, "{}", value);
                    break;
                }
            }

            pos = end_brace + 1;
        }

        return *this;
    }

    // Add a query parameter (URL-encoded)
    HttpRequestBuilder& QueryParam(std::string_view key, std::string_view value) {
        *out_++ = first_param_ ? '?' : '&';
        first_param_ = false;
        out_ = UrlEncode(out_, key);
        *out_++ = '=';
        out_ = UrlEncode(out_, value);
        return *this;
    }

    // Add a query parameter with numeric value
    template<typename T>
        requires std::integral<T> || std::floating_point<T>
    HttpRequestBuilder& QueryParam(std::string_view key, T value) {
        *out_++ = first_param_ ? '?' : '&';
        first_param_ = false;
        out_ = UrlEncode(out_, key);
        out_ = fmt::format_to(out_, "={}", value);
        return *this;
    }

    // End the request line and start headers
    HttpRequestBuilder& EndRequestLine() {
        out_ = fmt::format_to(out_, " HTTP/1.1\r\n");
        headers_started_ = true;
        return *this;
    }

    // Add Host header
    HttpRequestBuilder& Host(std::string_view host) {
        EnsureHeadersStarted();
        out_ = fmt::format_to(out_, "Host: {}\r\n", host);
        return *this;
    }

    // Add Basic Authorization header
    HttpRequestBuilder& BasicAuth(std::string_view api_key) {
        EnsureHeadersStarted();
        out_ = fmt::format_to(out_, "Authorization: Basic ");
        // Databento API expects api_key + ":" (empty password)
        std::string credentials{api_key};
        credentials += ':';
        out_ = Base64Encode(out_, credentials);
        out_ = fmt::format_to(out_, "\r\n");
        return *this;
    }

    // Add arbitrary header
    HttpRequestBuilder& Header(std::string_view name, std::string_view value) {
        EnsureHeadersStarted();
        out_ = fmt::format_to(out_, "{}: {}\r\n", name, value);
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
        auto body_it = std::back_inserter(body);
        bool first = true;
        for (const auto& [key, value] : params) {
            if (!first) *body_it++ = '&';
            first = false;
            body_it = UrlEncode(body_it, key);
            *body_it++ = '=';
            body_it = UrlEncode(body_it, value);
        }

        out_ = fmt::format_to(out_, "Content-Type: application/x-www-form-urlencoded\r\n");
        out_ = fmt::format_to(out_, "Content-Length: {}\r\n", body.size());
        out_ = fmt::format_to(out_, "\r\n");
        out_ = fmt::format_to(out_, "{}", body);
        body_written_ = true;
        return *this;
    }

    // Finish the request (writes final CRLF if no body)
    void Finish() {
        EnsureHeadersStarted();
        if (!body_written_) {
            out_ = fmt::format_to(out_, "\r\n");
        }
    }

    // Get current iterator position (for determining output size)
    OutputIt GetIterator() const { return out_; }

private:
    void EnsureHeadersStarted() {
        if (!headers_started_) {
            out_ = fmt::format_to(out_, " HTTP/1.1\r\n");
            headers_started_ = true;
        }
    }

    OutputIt out_;
    bool first_param_ = true;
    bool headers_started_ = false;
    bool body_written_ = false;
};

// Deduction guide
template<typename OutputIt>
HttpRequestBuilder(OutputIt) -> HttpRequestBuilder<OutputIt>;

}  // namespace dbn_pipe
