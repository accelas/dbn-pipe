// src/protocol_driver.hpp
#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <concepts>
#include <functional>
#include <memory>
#include <string>

#include "lib/stream/event_loop.hpp"
#include "pipeline_sink.hpp"

namespace dbn_pipe {

// ProtocolDriver concept defines the interface for protocol implementations.
// Each protocol (Live, Historical) provides static methods that the Pipeline
// template uses to handle protocol-specific behavior.
//
// With TcpSocket as part of the chain, the interface is simplified:
// - Connect/disconnect handled by ChainType
// - Ready callback notifies when protocol is ready to send request
// - Data flow handled internally by chain components
//
// Template parameters:
//   P - The protocol type (e.g., LiveProtocol, HistoricalProtocol)
//   Record - The record type passed through the Sink
template <typename P, typename Record>
concept ProtocolDriver = requires {
    // Request type for this protocol
    typename P::Request;

    // Chain type - includes TcpSocket as head
    typename P::ChainType;

    // SinkType - must be Sink<Record> or compatible
    typename P::SinkType;

    // Required static methods
    requires requires(
        IEventLoop& loop,
        typename P::SinkType& sink,
        const std::string& api_key,
        std::shared_ptr<typename P::ChainType> chain,
        const typename P::Request& request
    ) {
        // Build component chain (including TcpSocket), returns entry point
        // Dataset is set via SetDataset() after BuildChain() for protocols that need it
        { P::BuildChain(loop, sink, api_key) }
            -> std::same_as<std::shared_ptr<typename P::ChainType>>;

        // Send the protocol-specific request
        { P::SendRequest(chain, request) } -> std::same_as<void>;

        // Teardown chain components
        { P::Teardown(chain) } -> std::same_as<void>;
    };

    // ChainType must support Connect, SetReadyCallback, SetDataset, Suspend, Resume, Close
    requires requires(
        typename P::ChainType& chain,
        const sockaddr_storage& addr,
        std::function<void()> cb,
        const std::string& dataset
    ) {
        { chain.Connect(addr) } -> std::same_as<void>;
        { chain.SetReadyCallback(cb) } -> std::same_as<void>;
        { chain.SetDataset(dataset) } -> std::same_as<void>;
        { chain.Suspend() } -> std::same_as<void>;
        { chain.Resume() } -> std::same_as<void>;
        { chain.Close() } -> std::same_as<void>;
    };
};

}  // namespace dbn_pipe
