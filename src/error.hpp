// src/error.hpp
#pragma once

#include <string>
#include <string_view>

namespace databento_async {

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

    // Subscription
    InvalidDataset,
    InvalidSymbol,
    InvalidSchema,

    // TLS (Historical)
    TlsHandshakeFailed,
    CertificateError,

    // HTTP (Historical)
    HttpError,

    // Decompression (Historical)
    DecompressionError,
};

struct Error {
    ErrorCode code;
    std::string message;
    int os_errno = 0;
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
            return "protocol";
        case ErrorCode::InvalidDataset:
        case ErrorCode::InvalidSymbol:
        case ErrorCode::InvalidSchema:
            return "subscription";
        case ErrorCode::TlsHandshakeFailed:
        case ErrorCode::CertificateError:
            return "tls";
        case ErrorCode::HttpError:
            return "http";
        case ErrorCode::DecompressionError:
            return "decompression";
    }
    return "unknown";
}

}  // namespace databento_async
