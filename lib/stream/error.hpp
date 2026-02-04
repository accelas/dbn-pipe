// SPDX-License-Identifier: MIT

// lib/stream/error.hpp
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace dbn_pipe {

/// Error codes for all pipeline and API operations.
enum class ErrorCode {
    // Connection
    ConnectionFailed,      ///< TCP connection failed (ECONNREFUSED, timeout, etc.)
    ConnectionClosed,      ///< Remote peer closed the connection
    DnsResolutionFailed,   ///< Hostname could not be resolved

    // Auth
    AuthFailed,            ///< CRAM authentication rejected by server
    InvalidApiKey,         ///< API key format is invalid

    // Protocol
    InvalidGreeting,       ///< Live gateway sent unexpected greeting
    InvalidChallenge,      ///< CRAM challenge was malformed
    ParseError,            ///< DBN record or HTTP response could not be parsed
    BufferOverflow,        ///< Internal buffer exceeded size limit

    // Subscription
    InvalidDataset,        ///< Dataset identifier not recognized
    InvalidSymbol,         ///< Symbol not found in dataset
    InvalidSchema,         ///< Schema string not recognized
    InvalidTimeRange,      ///< Start/end range is invalid

    // State
    InvalidState,          ///< Method called in wrong client state

    // TLS (Historical)
    TlsHandshakeFailed,   ///< TLS handshake did not complete
    CertificateError,      ///< Server certificate validation failed

    // HTTP (Historical)
    HttpError,             ///< HTTP-level error (malformed response, unexpected status)

    // Decompression (Historical)
    DecompressionError,    ///< Zstd decompression failed

    // API errors
    Unauthorized,          ///< HTTP 401/403 — invalid or expired API key
    RateLimited,           ///< HTTP 429 — too many requests
    NotFound,              ///< HTTP 404 — resource does not exist
    ValidationError,       ///< HTTP 422 — request parameters invalid
    ServerError,           ///< HTTP 5xx — Databento server error
};

/// Error payload delivered to OnError callbacks.
struct Error {
    ErrorCode code;                ///< Classified error code
    std::string message;           ///< Human-readable description
    int os_errno = 0;              ///< OS errno if applicable, 0 otherwise
    std::optional<std::chrono::milliseconds> retry_after = {};  ///< Server-requested delay (rate limiting)
};

/// Return a short category string for an error code (e.g. "connection", "auth").
constexpr std::string_view error_category(ErrorCode code) {
    switch (code) {
        case ErrorCode::ConnectionFailed:
        case ErrorCode::ConnectionClosed:
        case ErrorCode::DnsResolutionFailed:
            return "connection";
        case ErrorCode::AuthFailed:
        case ErrorCode::InvalidApiKey:
            return "auth";
        case ErrorCode::InvalidGreeting:
        case ErrorCode::InvalidChallenge:
        case ErrorCode::ParseError:
        case ErrorCode::BufferOverflow:
            return "protocol";
        case ErrorCode::InvalidDataset:
        case ErrorCode::InvalidSymbol:
        case ErrorCode::InvalidSchema:
        case ErrorCode::InvalidTimeRange:
            return "subscription";
        case ErrorCode::InvalidState:
            return "state";
        case ErrorCode::TlsHandshakeFailed:
        case ErrorCode::CertificateError:
            return "tls";
        case ErrorCode::HttpError:
            return "http";
        case ErrorCode::DecompressionError:
            return "decompression";
        case ErrorCode::Unauthorized:
        case ErrorCode::RateLimited:
        case ErrorCode::NotFound:
        case ErrorCode::ValidationError:
        case ErrorCode::ServerError:
            return "api";
    }
    return "unknown";
}

}  // namespace dbn_pipe
