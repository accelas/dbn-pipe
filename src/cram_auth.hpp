// src/cram_auth.hpp
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace databento_async {

struct Greeting {
    std::string session_id;
    std::string version;
};

class CramAuth {
public:
    // Parse "session_id|version\n"
    static std::optional<Greeting> ParseGreeting(std::string_view data);

    // Parse "cram=challenge\n"
    static std::optional<std::string> ParseChallenge(std::string_view data);

    // Compute SHA256(challenge + '|' + api_key) as hex
    static std::string ComputeResponse(std::string_view challenge,
                                       std::string_view api_key);

    // Format "auth=api_key|method|response\n"
    static std::string FormatAuthMessage(std::string_view api_key,
                                         std::string_view method,
                                         std::string_view response);
};

}  // namespace databento_async
