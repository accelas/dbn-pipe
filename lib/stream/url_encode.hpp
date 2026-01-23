// SPDX-License-Identifier: MIT

// lib/stream/url_encode.hpp
#pragma once

#include <cctype>
#include <cstdint>
#include <iterator>
#include <string_view>

#include <fmt/format.h>

namespace dbn_pipe {

// URL encode helper - writes to output iterator
// Encodes all characters except alphanumeric and -_.~
template<typename OutputIt>
OutputIt UrlEncode(OutputIt out, std::string_view value) {
    for (char c : value) {
        // Keep alphanumeric and other accepted characters
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            *out++ = c;
        } else {
            // Percent-encode the character
            out = fmt::format_to(out, "%{:02X}", static_cast<unsigned char>(c));
        }
    }
    return out;
}

// Base64 encode - writes to output iterator
template<typename OutputIt>
OutputIt Base64Encode(OutputIt out, std::string_view input) {
    static const char* kBase64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t i = 0;
    while (i + 2 < input.size()) {
        uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) |
                          (static_cast<uint8_t>(input[i + 1]) << 8) |
                          static_cast<uint8_t>(input[i + 2]);
        *out++ = kBase64Chars[(triple >> 18) & 0x3F];
        *out++ = kBase64Chars[(triple >> 12) & 0x3F];
        *out++ = kBase64Chars[(triple >> 6) & 0x3F];
        *out++ = kBase64Chars[triple & 0x3F];
        i += 3;
    }

    if (i + 1 == input.size()) {
        uint32_t val = static_cast<uint8_t>(input[i]) << 16;
        *out++ = kBase64Chars[(val >> 18) & 0x3F];
        *out++ = kBase64Chars[(val >> 12) & 0x3F];
        *out++ = '=';
        *out++ = '=';
    } else if (i + 2 == input.size()) {
        uint32_t val = (static_cast<uint8_t>(input[i]) << 16) |
                       (static_cast<uint8_t>(input[i + 1]) << 8);
        *out++ = kBase64Chars[(val >> 18) & 0x3F];
        *out++ = kBase64Chars[(val >> 12) & 0x3F];
        *out++ = kBase64Chars[(val >> 6) & 0x3F];
        *out++ = '=';
    }

    return out;
}

}  // namespace dbn_pipe
