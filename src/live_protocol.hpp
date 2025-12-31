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

// LiveChain - The component chain entry point for live protocol
//
// This wraps CramAuth which is the first component in the live pipeline:
//   TcpSocket -> CramAuth -> DbnParserComponent -> SinkAdapter -> Sink
//
// CramAuth handles:
//   - CRAM authentication protocol
//   - Subscription (Subscribe method)
//   - Starting the stream (StartStreaming method)
//
// Template parameter Record is the record type for the Sink.
template <typename Record>
class LiveChain {
public:
    using ParserType = DbnParserComponent<SinkAdapter<Record>>;
    using CramType = CramAuth<ParserType>;

    LiveChain(Reactor& reactor, Sink<Record>& sink, const std::string& api_key)
        : sink_adapter_(sink)
        , parser_(sink_adapter_)
        , cram_(CramType::Create(reactor,
                                  std::make_shared<ParserType>(sink_adapter_),
                                  api_key))
    {}

    // Access to CramAuth for setting write callback and calling methods
    std::shared_ptr<CramType> GetCramAuth() { return cram_; }

    // Downstream interface - forward to CramAuth
    void Read(BufferChain data) {
        cram_->OnData(data);
    }

    void OnError(const Error& e) {
        cram_->OnError(e);
    }

    void OnDone() {
        cram_->OnDone();
    }

    // Close the chain
    void Close() {
        cram_->RequestClose();
    }

private:
    SinkAdapter<Record> sink_adapter_;
    ParserType parser_;  // Note: we use shared_ptr version passed to CramAuth
    std::shared_ptr<CramType> cram_;
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

    // ChainType wraps CramAuth for the specific Record type
    // We use a type-erased wrapper to avoid exposing the Record template
    struct ChainType {
        virtual ~ChainType() = default;
        virtual void Read(BufferChain data) = 0;
        virtual void OnError(const Error& e) = 0;
        virtual void OnDone() = 0;
        virtual void Close() = 0;
        virtual void SetWriteCallback(std::function<void(BufferChain)> cb) = 0;
        virtual void Subscribe(std::string dataset, std::string symbols, std::string schema) = 0;
        virtual void StartStreaming() = 0;
    };

    // Concrete implementation of ChainType for a specific Record type
    template <typename Record>
    struct ChainImpl : ChainType {
        using ParserType = DbnParserComponent<SinkAdapter<Record>>;
        using CramType = CramAuth<ParserType>;

        ChainImpl(Reactor& reactor, Sink<Record>& sink, const std::string& api_key)
            : sink_adapter_(std::make_unique<SinkAdapter<Record>>(sink))
            , parser_(std::make_shared<ParserType>(*sink_adapter_))
            , cram_(CramType::Create(reactor, parser_, api_key))
        {}

        void Read(BufferChain data) override {
            cram_->OnData(data);
        }

        void OnError(const Error& e) override {
            cram_->OnError(e);
        }

        void OnDone() override {
            cram_->OnDone();
        }

        void Close() override {
            cram_->RequestClose();
        }

        void SetWriteCallback(std::function<void(BufferChain)> cb) override {
            cram_->SetWriteCallback(std::move(cb));
        }

        void Subscribe(std::string dataset, std::string symbols, std::string schema) override {
            cram_->Subscribe(std::move(dataset), std::move(symbols), std::move(schema));
        }

        void StartStreaming() override {
            cram_->StartStreaming();
        }

    private:
        std::unique_ptr<SinkAdapter<Record>> sink_adapter_;
        std::shared_ptr<ParserType> parser_;
        std::shared_ptr<CramType> cram_;
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

    // Wire TCP socket write to chain
    static void WireTcp(TcpSocket& tcp, std::shared_ptr<ChainType>& chain) {
        chain->SetWriteCallback([&tcp](BufferChain data) {
            tcp.Write(std::move(data));
        });
    }

    // Handle TCP connect - live is ready immediately
    static bool OnConnect(std::shared_ptr<ChainType>& /*chain*/) {
        return true;  // Ready to send request on connect
    }

    // Handle TCP read - forward data to chain
    static bool OnRead(std::shared_ptr<ChainType>& chain, BufferChain data) {
        if (chain && !data.Empty()) {
            chain->Read(std::move(data));
        }
        return true;  // Always ready after connect
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
