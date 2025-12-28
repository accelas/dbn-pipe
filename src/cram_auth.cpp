// src/cram_auth.cpp
#include "cram_auth.hpp"

#include <openssl/sha.h>

#include <iomanip>
#include <sstream>

namespace databento_async {

std::optional<Greeting> CramAuth::ParseGreeting(std::string_view data) {
    // Format: "session_id|version\n"
    auto pipe = data.find('|');
    if (pipe == std::string_view::npos) {
        return std::nullopt;
    }

    auto newline = data.find('\n', pipe);
    if (newline == std::string_view::npos) {
        newline = data.size();
    }

    return Greeting{
        .session_id = std::string(data.substr(0, pipe)),
        .version = std::string(data.substr(pipe + 1, newline - pipe - 1)),
    };
}

std::optional<std::string> CramAuth::ParseChallenge(std::string_view data) {
    // Format: "cram=challenge\n"
    constexpr std::string_view prefix = "cram=";
    if (!data.starts_with(prefix)) {
        return std::nullopt;
    }

    auto newline = data.find('\n', prefix.size());
    if (newline == std::string_view::npos) {
        newline = data.size();
    }

    return std::string(data.substr(prefix.size(), newline - prefix.size()));
}

std::string CramAuth::ComputeResponse(std::string_view challenge,
                                      std::string_view api_key) {
    // Build challenge_key = challenge + '|' + api_key
    std::string challenge_key;
    challenge_key.reserve(challenge.size() + 1 + api_key.size());
    challenge_key.append(challenge);
    challenge_key.push_back('|');
    challenge_key.append(api_key);

    // Compute SHA256 hash
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(challenge_key.data()),
           challenge_key.size(),
           digest);

    // Convert to hex string
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(digest[i]);
    }
    return oss.str();
}

}  // namespace databento_async
