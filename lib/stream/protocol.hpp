// SPDX-License-Identifier: MIT

// lib/stream/protocol.hpp
#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>

#include "lib/stream/event_loop.hpp"
#include "lib/stream/sink.hpp"

namespace dbn_pipe {

// Protocol concept - defines the interface for protocol implementations
//
// Protocol is a static factory that defines types and construction.
// The Pipeline template uses these static methods to handle protocol-specific behavior.
template<typename P>
concept Protocol = requires {
    // Required type aliases
    typename P::Request;
    typename P::SinkType;
    typename P::ChainType;

    // SinkType must satisfy BasicSink concept
    requires BasicSink<typename P::SinkType>;

    // ChainType must support network lifecycle and backpressure
    requires requires(
        typename P::ChainType& chain,
        const typename P::ChainType& const_chain,
        const sockaddr_storage& addr,
        std::function<void()> cb,
        const std::string& dataset
    ) {
        { chain.Connect(addr) } -> std::same_as<void>;
        { chain.Close() } -> std::same_as<void>;
        { chain.SetReadyCallback(cb) } -> std::same_as<void>;
        { chain.SetDataset(dataset) } -> std::same_as<void>;  // For protocols needing dataset before auth
        { chain.Suspend() } -> std::same_as<void>;
        { chain.Resume() } -> std::same_as<void>;
        { const_chain.IsSuspended() } -> std::same_as<bool>;
    };

    // Chain building (sink passed by reference - Pipeline owns sink)
    requires requires(
        IEventLoop& loop,
        typename P::SinkType& sink,
        const std::string& api_key
    ) {
        { P::BuildChain(loop, sink, api_key) }
            -> std::same_as<std::shared_ptr<typename P::ChainType>>;
    };

    // Chain operations
    requires requires(
        std::shared_ptr<typename P::ChainType>& chain,
        const typename P::Request& request
    ) {
        { P::SendRequest(chain, request) } -> std::same_as<void>;
        { P::Teardown(chain) } -> std::same_as<void>;
        { P::GetHostname(request) } -> std::convertible_to<std::string>;
        { P::GetPort(request) } -> std::convertible_to<uint16_t>;
    };
};

}  // namespace dbn_pipe
