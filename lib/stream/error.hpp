// lib/stream/error.hpp
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace dbn_pipe {

enum class ErrorCode {
    // Connection
    ConnectionFailed,
    ConnectionClosed,
    DnsResolutionFailed,

    // Auth
    AuthFailed,
    InvalidApiKey,

    // Protocol
    InvalidGreeting,
    InvalidChallenge,
    ParseError,
    BufferOverflow,

    // Subscription
    InvalidDataset,
    InvalidSymbol,
    InvalidSchema,
    InvalidTimeRange,

    // State
    InvalidState,

    // TLS (Historical)
    TlsHandshakeFailed,
    CertificateError,

    // HTTP (Historical)
    HttpError,

    // Decompression (Historical)
    DecompressionError,

    // API errors
    Unauthorized,      // HTTP 401/403
    RateLimited,       // HTTP 429
    NotFound,          // HTTP 404
    ValidationError,   // HTTP 422
    ServerError,       // HTTP 5xx
};

struct Error {
    ErrorCode code;
    std::string message;
    int os_errno = 0;
    std::optional<std::chrono::milliseconds> retry_after = {};  // For rate limiting
};

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
