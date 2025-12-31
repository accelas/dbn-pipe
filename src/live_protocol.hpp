// src/live_protocol.hpp
#pragma once

#include <memory>
#include <string>

#include "buffer_chain.hpp"
#include "cram_auth.hpp"
#include "dbn_parser_component.hpp"
#include "pipeline_component.hpp"
#include "pipeline_sink.hpp"
#include "reactor.hpp"
#include "sink_adapter.hpp"
#include "tcp_socket.hpp"

namespace databento_async {

// LiveRequest - Parameters for live data subscription
struct LiveRequest {
    std::string dataset;   // Dataset to subscribe to (e.g., "GLBX.MDP3")
    std::string symbols;   // Symbol(s) to subscribe to (e.g., "ESZ4")
    std::string schema;    // Schema for data (e.g., "mbp-1")
};

// LiveProtocol - ProtocolDriver implementation for live streaming
//
// Satisfies the ProtocolDriver concept. Uses CramAuth for authentication
// and subscription management.
//
// Chain: TcpSocket -> CramAuth -> DbnParserComponent -> SinkAdapter -> Sink
//
// Live protocol is ready immediately on connect (OnConnect returns true).
// SendRequest subscribes to the dataset/symbols/schema and starts streaming.
struct LiveProtocol {
    using Request = LiveRequest;

    // ChainType wraps the full pipeline including TcpSocket
    // Type-erased wrapper to avoid exposing template parameters
    struct ChainType {
        virtual ~ChainType() = default;

        // Network lifecycle
        virtual void Connect(const sockaddr_storage& addr) = 0;
        virtual void Close() = 0;

        // Ready callback - fires when chain is ready to send request
        virtual void SetReadyCallback(std::function<void()> cb) = 0;

        // Backpressure
        virtual void Suspend() = 0;
        virtual void Resume() = 0;

        // Protocol-specific
        virtual void Subscribe(std::string dataset, std::string symbols, std::string schema) = 0;
        virtual void StartStreaming() = 0;
    };

    // Concrete implementation of ChainType for a specific Record type
    // TcpSocket is the head, wrapping the rest of the chain
    // Static dispatch within chain via template parameters
    template <typename Record>
    struct ChainImpl : ChainType {
        using SinkAdapterType = SinkAdapter<Record>;
        using ParserType = DbnParserComponent<SinkAdapterType>;
        using CramType = CramAuth<ParserType>;
        using HeadType = TcpSocket<CramType>;

        ChainImpl(Reactor& reactor, Sink<Record>& sink, const std::string& api_key)
            : sink_adapter_(std::make_unique<SinkAdapterType>(sink))
            , parser_(std::make_shared<ParserType>(*sink_adapter_))
            , cram_(CramType::Create(reactor, parser_, api_key))
            , head_(HeadType::Create(reactor, cram_))
        {
            // Wire connect callback for ready signal
            head_->OnConnect([this]() {
                // Live protocol is ready immediately on connect
                if (ready_cb_) ready_cb_();
            });
        }

        // Network lifecycle
        void Connect(const sockaddr_storage& addr) override {
            head_->Connect(addr);
        }

        void Close() override {
            head_->Close();
        }

        // Ready callback
        void SetReadyCallback(std::function<void()> cb) override {
            ready_cb_ = std::move(cb);
        }

        // Backpressure - forward to head (TcpSocket)
        void Suspend() override { head_->Suspend(); }
        void Resume() override { head_->Resume(); }

        // Protocol-specific - forward to CramAuth
        void Subscribe(std::string dataset, std::string symbols, std::string schema) override {
            cram_->Subscribe(std::move(dataset), std::move(symbols), std::move(schema));
        }

        void StartStreaming() override {
            cram_->StartStreaming();
        }

    private:
        std::unique_ptr<SinkAdapterType> sink_adapter_;
        std::shared_ptr<ParserType> parser_;
        std::shared_ptr<CramType> cram_;
        std::shared_ptr<HeadType> head_;
        std::function<void()> ready_cb_;
    };

    // Build the component chain for live protocol
    template <typename Record>
    static std::shared_ptr<ChainType> BuildChain(
        Reactor& reactor,
        Sink<Record>& sink,
        const std::string& api_key
    ) {
        return std::make_shared<ChainImpl<Record>>(reactor, sink, api_key);
    }

    // Send request - subscribe and start streaming
    static void SendRequest(std::shared_ptr<ChainType>& chain, const Request& request) {
        if (chain) {
            chain->Subscribe(request.dataset, request.symbols, request.schema);
            chain->StartStreaming();
        }
    }

    // Teardown - close the chain
    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) {
            chain->Close();
        }
    }
};

}  // namespace databento_async
