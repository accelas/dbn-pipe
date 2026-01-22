// src/historical_protocol.hpp
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <spanstream>
#include <string>

#include "lib/stream/buffer_chain.hpp"
#include "lib/stream/http_request_builder.hpp"
#include "dbn_parser_component.hpp"
#include "lib/stream/event_loop.hpp"
#include "lib/stream/http_client.hpp"
#include "lib/stream/component.hpp"
#include "lib/stream/sink.hpp"
#include "lib/stream/tcp_socket.hpp"
#include "lib/stream/tls_transport.hpp"
#include "lib/stream/zstd_decompressor.hpp"

namespace dbn_pipe {

// HistoricalRequest - Parameters for historical data download
struct HistoricalRequest {
    std::string dataset;   // Dataset to query (e.g., "GLBX.MDP3")
    std::string symbols;   // Symbol(s) to query (e.g., "ESZ4")
    std::string schema;    // Schema for data (e.g., "mbp-1")
    uint64_t start;        // Start time in nanoseconds since Unix epoch
    uint64_t end;          // End time in nanoseconds since Unix epoch
    std::string stype_in;  // Input symbology type: "raw_symbol" (default), "parent", etc.
    std::string stype_out; // Output symbology type: triggers SymbolMappingMsg when set
};

// Historical gateway constants
constexpr const char* kHistoricalHostname = "hist.databento.com";
constexpr uint16_t kHistoricalPort = 443;

// HistoricalProtocol - ProtocolDriver implementation for historical downloads
//
// Satisfies the ProtocolDriver concept. Uses TLS -> HTTP -> Zstd -> DBN parser chain.
//
// Chain: TcpSocket -> TlsTransport -> HttpClient -> ZstdDecompressor -> DbnParserComponent -> Sink
//
// Historical protocol requires TLS handshake before sending HTTP request.
// OnConnect starts the handshake and returns false (not ready yet).
// OnRead returns true after handshake completes.
struct HistoricalProtocol {
    using Request = HistoricalRequest;
    using SinkType = StreamRecordSink;

    // ChainType wraps the full pipeline including TcpSocket
    // Type-erased wrapper to avoid exposing template parameters
    struct ChainType {
        virtual ~ChainType() = default;

        // Network lifecycle
        virtual void Connect(const sockaddr_storage& addr) = 0;
        virtual void Close() = 0;

        // Ready callback - fires when chain is ready to send request (after TLS handshake)
        virtual void SetReadyCallback(std::function<void()> cb) = 0;

        // Dataset - no-op for historical (HTTP basic auth doesn't need dataset)
        virtual void SetDataset(const std::string&) = 0;

        // Backpressure
        virtual void Suspend() = 0;
        virtual void Resume() = 0;
        virtual bool IsSuspended() const = 0;

        // Protocol-specific - for sending HTTP request
        virtual std::shared_ptr<Segment> GetRequestSegment() = 0;
        virtual void SendRequestSegment(std::shared_ptr<Segment> seg) = 0;
        virtual const std::string& GetApiKey() const = 0;
    };

    // Concrete implementation of ChainType
    // TcpSocket is the head, wrapping the rest of the chain
    // Static dispatch within chain via template parameters
    struct ChainImpl : ChainType {
        using ParserType = DbnParserComponent<StreamRecordSink>;
        using ZstdType = ZstdDecompressor<ParserType>;
        using HttpType = HttpClient<ZstdType>;
        using TlsType = TlsTransport<HttpType>;
        using HeadType = TcpSocket<TlsType>;

        ChainImpl(IEventLoop& loop, StreamRecordSink& sink, const std::string& api_key)
            : loop_(loop)
            , api_key_(api_key)
            , parser_(std::make_shared<ParserType>(sink))
            , zstd_(ZstdType::Create(loop, parser_))
            , http_(HttpType::Create(loop, zstd_))
            , tls_(TlsType::Create(loop, http_))
            , head_(HeadType::Create(loop, tls_))
        {
            // Wire up upstream pointers for backpressure propagation
            http_->SetUpstream(tls_.get());
            zstd_->SetUpstream(http_.get());

            // Set hostname for SNI
            tls_->SetHostname("hist.databento.com");

            // Wire connect callback to start TLS handshake
            head_->OnConnect([this]() {
                tls_->StartHandshake();
            });

            // Wire TLS handshake complete callback
            tls_->SetHandshakeCompleteCallback([this]() {
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

        // Dataset - no-op for historical (HTTP basic auth doesn't need dataset)
        void SetDataset(const std::string&) override {}

        // Backpressure - forward to head (TcpSocket)
        void Suspend() override { head_->Suspend(); }
        void Resume() override { head_->Resume(); }
        bool IsSuspended() const override { return head_->IsSuspended(); }

        // Protocol-specific - for sending HTTP request
        std::shared_ptr<Segment> GetRequestSegment() override {
            return std::make_shared<Segment>();
        }

        void SendRequestSegment(std::shared_ptr<Segment> seg) override {
            BufferChain chain;
            chain.Append(std::move(seg));
            tls_->Write(std::move(chain));
        }

        const std::string& GetApiKey() const override { return api_key_; }

    private:
        IEventLoop& loop_;
        std::string api_key_;
        std::shared_ptr<ParserType> parser_;
        std::shared_ptr<ZstdType> zstd_;
        std::shared_ptr<HttpType> http_;
        std::shared_ptr<TlsType> tls_;
        std::shared_ptr<HeadType> head_;
        std::function<void()> ready_cb_;
    };

    // Build the component chain for historical protocol
    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop& loop,
        StreamRecordSink& sink,
        const std::string& api_key
    ) {
        return std::make_shared<ChainImpl>(loop, sink, api_key);
    }

    // Send request - build and send HTTP GET request
    // Formats directly into segment buffer - zero intermediate copies
    static void SendRequest(std::shared_ptr<ChainType>& chain, const Request& request) {
        if (!chain) return;

        // Get segment and format directly into it
        auto seg = chain->GetRequestSegment();
        std::ospanstream out(std::span<char>(
            reinterpret_cast<char*>(seg->data.data()), Segment::kSize));

        // Build HTTP GET request for historical data API
        auto builder = HttpRequestBuilder(out)
            .Method("GET")
            .Path("/v0/timeseries.get_range")
            .QueryParam("dataset", request.dataset)
            .QueryParam("symbols", request.symbols)
            .QueryParam("schema", request.schema)
            .QueryParam("start", request.start)
            .QueryParam("end", request.end)
            .QueryParam("encoding", "dbn")
            .QueryParam("compression", "zstd");

        if (!request.stype_in.empty()) {
            builder.QueryParam("stype_in", request.stype_in);
        }
        if (!request.stype_out.empty()) {
            builder.QueryParam("stype_out", request.stype_out);
        }

        builder
            .Host(kHistoricalHostname)
            .BasicAuth(chain->GetApiKey())
            .Header("Accept", "application/octet-stream")
            .Header("Accept-Encoding", "zstd")
            .Header("Connection", "close")
            .Finish();

        seg->size = out.span().size();
        chain->SendRequestSegment(std::move(seg));
    }

    // Teardown - close the chain
    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) {
            chain->Close();
        }
    }

    // Get gateway hostname (static for historical)
    static std::string GetHostname(const Request&) {
        return kHistoricalHostname;
    }

    // Get gateway port
    static uint16_t GetPort(const Request&) {
        return kHistoricalPort;
    }
};

}  // namespace dbn_pipe
