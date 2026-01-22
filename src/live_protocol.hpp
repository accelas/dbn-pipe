// src/live_protocol.hpp
#pragma once

#include <memory>
#include <string>

#include "lib/stream/buffer_chain.hpp"
#include "cram_auth.hpp"
#include "dbn_parser_component.hpp"
#include "lib/stream/event_loop.hpp"
#include "lib/stream/component.hpp"
#include "lib/stream/sink.hpp"
#include "lib/stream/tcp_socket.hpp"

namespace dbn_pipe {

// LiveRequest - Parameters for live data subscription
struct LiveRequest {
    std::string dataset;   // Dataset to subscribe to (e.g., "GLBX.MDP3")
    std::string symbols;   // Symbol(s) to subscribe to (e.g., "ESZ4")
    std::string schema;    // Schema for data (e.g., "mbp-1")
};

// Default live gateway port
constexpr uint16_t kLivePort = 13000;

// LiveProtocol - ProtocolDriver implementation for live streaming
//
// Satisfies the Protocol concept. Uses CramAuth for authentication
// and subscription management.
//
// Chain: TcpSocket -> CramAuth -> DbnParserComponent -> Sink
//
// Live protocol is ready immediately on connect (OnConnect returns true).
// SendRequest subscribes to the dataset/symbols/schema and starts streaming.
struct LiveProtocol {
    using Request = LiveRequest;
    using SinkType = StreamRecordSink;

    // ChainType wraps the full pipeline including TcpSocket
    // Type-erased wrapper to avoid exposing template parameters
    struct ChainType {
        virtual ~ChainType() = default;

        // Network lifecycle
        virtual void Connect(const sockaddr_storage& addr) = 0;
        virtual void Close() = 0;

        // Ready callback - fires when chain is ready to send request
        virtual void SetReadyCallback(std::function<void()> cb) = 0;

        // Dataset - required for CRAM authentication
        virtual void SetDataset(const std::string& dataset) = 0;

        // Backpressure
        virtual void Suspend() = 0;
        virtual void Resume() = 0;
        virtual bool IsSuspended() const = 0;

        // Protocol-specific
        virtual void Subscribe(std::string symbols, std::string schema) = 0;
        virtual void StartStreaming() = 0;
    };

    // Concrete implementation of ChainType
    // TcpSocket is the head, wrapping the rest of the chain
    // Static dispatch within chain via template parameters
    struct ChainImpl : ChainType {
        using ParserType = DbnParserComponent<StreamRecordSink>;
        using CramType = CramAuth<ParserType>;
        using HeadType = TcpSocket<CramType>;

        ChainImpl(IEventLoop& loop, StreamRecordSink& sink, const std::string& api_key)
            : parser_(std::make_shared<ParserType>(sink))
            , cram_(CramType::Create(loop, parser_, api_key))
            , head_(HeadType::Create(loop, cram_))
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

        // Dataset - pass to CramAuth for CRAM authentication
        void SetDataset(const std::string& dataset) override {
            cram_->SetDataset(dataset);
        }

        // Backpressure - forward to head (TcpSocket)
        void Suspend() override { head_->Suspend(); }
        void Resume() override { head_->Resume(); }
        bool IsSuspended() const override { return head_->IsSuspended(); }

        // Protocol-specific - forward to CramAuth
        void Subscribe(std::string symbols, std::string schema) override {
            cram_->Subscribe(std::move(symbols), std::move(schema));
        }

        void StartStreaming() override {
            cram_->StartStreaming();
        }

    private:
        std::shared_ptr<ParserType> parser_;
        std::shared_ptr<CramType> cram_;
        std::shared_ptr<HeadType> head_;
        std::function<void()> ready_cb_;
    };

    // Build the component chain for live protocol
    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop& loop,
        StreamRecordSink& sink,
        const std::string& api_key
    ) {
        return std::make_shared<ChainImpl>(loop, sink, api_key);
    }

    // Send request - subscribe and start streaming
    // Note: SetDataset must be called before Connect to set the dataset for CRAM auth
    static void SendRequest(std::shared_ptr<ChainType>& chain, const Request& request) {
        if (chain) {
            chain->Subscribe(request.symbols, request.schema);
            chain->StartStreaming();
        }
    }

    // Teardown - close the chain
    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) {
            chain->Close();
        }
    }

    // Get gateway hostname from dataset (e.g., "GLBX.MDP3" -> "glbx-mdp3.lsg.databento.com")
    static std::string GetHostname(const Request& request) {
        std::string hostname;
        hostname.reserve(request.dataset.size() + 20);
        for (char c : request.dataset) {
            hostname += (c == '.') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        hostname += ".lsg.databento.com";
        return hostname;
    }

    // Get gateway port
    static uint16_t GetPort(const Request&) {
        return kLivePort;
    }
};

}  // namespace dbn_pipe
