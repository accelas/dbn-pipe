// SPDX-License-Identifier: MIT

// src/dns_resolver.hpp
#pragma once

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace dbn_pipe {

// Resolve hostname to sockaddr_storage using getaddrinfo (blocking)
// Returns nullopt if resolution fails
inline std::optional<sockaddr_storage> ResolveHostname(std::string_view hostname, uint16_t port) {
    // Null-terminate the hostname
    std::string host_str(hostname);

    struct addrinfo hints{};
    hints.ai_family = AF_INET;      // IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = nullptr;
    int ret = getaddrinfo(host_str.c_str(), nullptr, &hints, &result);
    if (ret != 0 || result == nullptr) {
        return std::nullopt;
    }

    sockaddr_storage addr{};
    std::memcpy(&addr, result->ai_addr, result->ai_addrlen);

    // Set the port
    if (result->ai_family == AF_INET) {
        auto* addr_in = reinterpret_cast<sockaddr_in*>(&addr);
        addr_in->sin_port = htons(port);
    } else if (result->ai_family == AF_INET6) {
        auto* addr_in6 = reinterpret_cast<sockaddr_in6*>(&addr);
        addr_in6->sin6_port = htons(port);
    }

    freeaddrinfo(result);
    return addr;
}

}  // namespace dbn_pipe
