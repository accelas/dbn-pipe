// src/protocol_driver.hpp
#pragma once

#include <concepts>
#include <memory>
#include <string>

#include "buffer_chain.hpp"
#include "pipeline_sink.hpp"
#include "reactor.hpp"
#include "tcp_socket.hpp"

namespace databento_async {

// ProtocolDriver concept defines the interface for protocol implementations.
// Each protocol (Live, Historical) provides static methods that the Pipeline
// template uses to handle protocol-specific behavior.
//
// Template parameters:
//   P - The protocol type (e.g., LiveProtocol, HistoricalProtocol)
//   Record - The record type passed through the Sink
template <typename P, typename Record>
concept ProtocolDriver = requires {
    // Request type for this protocol
    typename P::Request;

    // Chain type - the entry point component for this protocol
    typename P::ChainType;

    // Required static methods
    requires requires(
        Reactor& reactor,
        Sink<Record>& sink,
        const std::string& api_key,
        TcpSocket& tcp,
        std::shared_ptr<typename P::ChainType> chain,
        const typename P::Request& request,
        BufferChain data
    ) {
        // Build component chain, returns entry point
        { P::BuildChain(reactor, sink, api_key) }
            -> std::same_as<std::shared_ptr<typename P::ChainType>>;

        // Wire TCP write callbacks to chain
        { P::WireTcp(tcp, chain) } -> std::same_as<void>;

        // Handle TCP connect event - returns true if ready to send request
        // Live: returns true (send on connect)
        // Historical: returns false (must wait for TLS handshake)
        { P::OnConnect(chain) } -> std::same_as<bool>;

        // Handle TCP read - returns true if ready to send request
        // Live: always returns true (already ready)
        // Historical: returns true after TLS handshake completes
        { P::OnRead(chain, std::move(data)) } -> std::same_as<bool>;

        // Send the protocol-specific request
        { P::SendRequest(chain, request) } -> std::same_as<void>;

        // Teardown chain components
        { P::Teardown(chain) } -> std::same_as<void>;
    };
};

}  // namespace databento_async
