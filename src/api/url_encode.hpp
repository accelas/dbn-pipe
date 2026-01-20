// src/api/url_encode.hpp
#pragma once

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <string_view>

namespace dbn_pipe {

// URL encode helper - writes directly to output stream
// Encodes all characters except alphanumeric and -_.~
inline void UrlEncode(std::ostream& out, std::string_view value) {
    auto flags = out.flags();
    out.fill('0');
    out << std::hex;

    for (char c : value) {
        // Keep alphanumeric and other accepted characters
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            // Percent-encode the character
            out << '%' << std::setw(2) << std::uppercase
                << static_cast<int>(static_cast<unsigned char>(c));
        }
    }

    out.flags(flags);  // Restore flags for subsequent output
}

// Base64 encode - writes directly to output stream
inline void Base64Encode(std::ostream& out, std::string_view input) {
    static const char* kBase64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t i = 0;
    while (i + 2 < input.size()) {
        uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) |
                          (static_cast<uint8_t>(input[i + 1]) << 8) |
                          static_cast<uint8_t>(input[i + 2]);
        out << kBase64Chars[(triple >> 18) & 0x3F];
        out << kBase64Chars[(triple >> 12) & 0x3F];
        out << kBase64Chars[(triple >> 6) & 0x3F];
        out << kBase64Chars[triple & 0x3F];
        i += 3;
    }

    if (i + 1 == input.size()) {
        uint32_t val = static_cast<uint8_t>(input[i]) << 16;
        out << kBase64Chars[(val >> 18) & 0x3F];
        out << kBase64Chars[(val >> 12) & 0x3F];
        out << '=';
        out << '=';
    } else if (i + 2 == input.size()) {
        uint32_t val = (static_cast<uint8_t>(input[i]) << 16) |
                       (static_cast<uint8_t>(input[i + 1]) << 8);
        out << kBase64Chars[(val >> 18) & 0x3F];
        out << kBase64Chars[(val >> 12) & 0x3F];
        out << kBase64Chars[(val >> 6) & 0x3F];
        out << '=';
    }
}

}  // namespace dbn_pipe
