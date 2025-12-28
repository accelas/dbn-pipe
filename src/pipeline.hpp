// src/pipeline.hpp
#pragma once

#include <concepts>
#include <memory_resource>

#include "error.hpp"
#include "reactor.hpp"

namespace databento_async {

// Downstream interface - receives data flowing toward application
template<typename D>
concept Downstream = requires(D& d, std::pmr::vector<std::byte> data, const Error& e) {
    { d.Read(std::move(data)) } -> std::same_as<void>;
    { d.OnError(e) } -> std::same_as<void>;
    { d.OnDone() } -> std::same_as<void>;
};

// Upstream interface - control flowing toward socket
template<typename U>
concept Upstream = requires(U& u, std::pmr::vector<std::byte> data) {
    { u.Write(std::move(data)) } -> std::same_as<void>;
    { u.Suspend() } -> std::same_as<void>;
    { u.Resume() } -> std::same_as<void>;
    { u.Close() } -> std::same_as<void>;
};

}  // namespace databento_async
