// SPDX-License-Identifier: MIT

// src/historical_protocol.hpp
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "dbn_pipe/to_nanos.hpp"
#include "dbn_pipe/stream/buffer_chain.hpp"
#include "dbn_pipe/stream/http_request_builder.hpp"
#include "dbn_pipe/stream/segment_allocator.hpp"
#include "dbn_pipe/dbn_parser_component.hpp"
#include "dbn_pipe/stream/event_loop.hpp"
#include "dbn_pipe/stream/http_client.hpp"
#include "dbn_pipe/stream/component.hpp"
#include "dbn_pipe/stream/sink.hpp"
#include "dbn_pipe/stream/tcp_socket.hpp"
#include "dbn_pipe/stream/tls_transport.hpp"
#include "dbn_pipe/stream/zstd_decompressor.hpp"

namespace dbn_pipe {

/// Parameters for a historical data download request.
struct HistoricalRequest {
    std::string dataset;   ///< Dataset to query (e.g., "GLBX.MDP3").
    std::string symbols;   ///< Symbol(s) to query (e.g., "ESZ4").
    std::string schema;    ///< Schema for data (e.g., "mbp-1").
    Timestamp start;       ///< Start time (chrono time_point or raw nanoseconds).
    Timestamp end;         ///< End time (chrono time_point or raw nanoseconds).
    std::string stype_in;  ///< Input symbology type: "raw_symbol" (default), "parent", etc.
    std::string stype_out; ///< Output symbology type; triggers SymbolMappingMsg when set.
};

/// Hostname of the Databento historical data gateway.
constexpr const char* kHistoricalHostname = "hist.databento.com";
/// Port for the historical gateway (HTTPS).
constexpr uint16_t kHistoricalPort = 443;

/// ProtocolDriver implementation for historical data downloads.
///
/// Satisfies the ProtocolDriver concept. Uses TLS -> HTTP -> Zstd -> DBN parser chain.
///
/// Chain: TcpSocket -> TlsTransport -> HttpClient -> ZstdDecompressor -> DbnParserComponent -> Sink
///
/// The historical protocol requires a TLS handshake before the HTTP request
/// can be sent. OnConnect starts the handshake and returns false (not ready
/// yet); OnRead returns true after the handshake completes.
struct HistoricalProtocol {
    using Request = HistoricalRequest;   ///< Request type for this protocol.
    using SinkType = StreamRecordSink;   ///< Sink type that receives parsed records.

    /// @internal
    /// Type-erased interface for the full pipeline including TcpSocket.
    /// Hides concrete template parameters from callers.
    struct ChainType {
        virtual ~ChainType() = default;

        /// Initiate a TCP connection to the given address.
        virtual void Connect(const sockaddr_storage& addr) = 0;
        /// Close the connection and release resources.
        virtual void Close() = 0;

        /// Register a callback that fires when the chain is ready to send
        /// a request (i.e., after the TLS handshake completes).
        virtual void SetReadyCallback(std::function<void()> cb) = 0;

        /// Set the dataset (no-op for historical; HTTP basic auth does not need it).
        virtual void SetDataset(const std::string&) = 0;

        /// Pause reading from the socket (backpressure).
        virtual void Suspend() = 0;
        /// Resume reading from the socket.
        virtual void Resume() = 0;
        /// @return true if the chain is currently suspended.
        virtual bool IsSuspended() const = 0;

        /// Allocate a Segment for formatting an HTTP request.
        virtual std::shared_ptr<Segment> GetRequestSegment() = 0;
        /// Send a previously formatted request segment over the TLS connection.
        /// @param seg  Segment containing the raw HTTP request bytes.
        virtual void SendRequestSegment(std::shared_ptr<Segment> seg) = 0;
        /// @return Reference to the API key used for authentication.
        virtual const std::string& GetApiKey() const = 0;
    };

    /// @internal
    /// Concrete implementation of ChainType.
    /// TcpSocket is the head; static dispatch within the chain via template parameters.
    struct ChainImpl : ChainType {
        using ParserType = DbnParserComponent<StreamRecordSink>;
        using ZstdType = ZstdDecompressor<ParserType>;
        using HttpType = HttpClient<ZstdType>;
        using TlsType = TlsTransport<HttpType>;
        using HeadType = TcpSocket<TlsType>;

        ChainImpl(IEventLoop& loop, StreamRecordSink& sink, const std::string& api_key,
                  SegmentAllocator* alloc = nullptr)
            : allocator_(alloc ? *alloc : SegmentAllocator{})
            , api_key_(api_key)
            , parser_(std::make_shared<ParserType>(sink))
            , zstd_(ZstdType::Create(parser_))
            , http_(HttpType::Create(zstd_))
            , tls_(TlsType::Create(http_))
            , head_(HeadType::Create(loop, tls_))
        {
            // Wire defer callback from event loop
            auto defer = [&loop](auto fn) { loop.Defer(std::move(fn)); };
            tls_->SetDefer(defer);
            http_->SetDefer(defer);
            zstd_->SetDefer(defer);

            // Wire allocator to all components
            head_->SetAllocator(&allocator_);
            tls_->SetAllocator(&allocator_);
            http_->SetAllocator(&allocator_);
            zstd_->SetAllocator(&allocator_);
            parser_->SetAllocator(&allocator_);

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

        /// @copydoc ChainType::Connect
        void Connect(const sockaddr_storage& addr) override {
            head_->Connect(addr);
        }

        /// @copydoc ChainType::Close
        void Close() override {
            head_->Close();
        }

        /// @copydoc ChainType::SetReadyCallback
        void SetReadyCallback(std::function<void()> cb) override {
            ready_cb_ = std::move(cb);
        }

        /// @copydoc ChainType::SetDataset
        void SetDataset(const std::string&) override {}

        /// @copydoc ChainType::Suspend
        void Suspend() override { head_->Suspend(); }
        /// @copydoc ChainType::Resume
        void Resume() override { head_->Resume(); }
        /// @copydoc ChainType::IsSuspended
        bool IsSuspended() const override { return head_->IsSuspended(); }

        /// @copydoc ChainType::GetRequestSegment
        std::shared_ptr<Segment> GetRequestSegment() override {
            return allocator_.Allocate();
        }

        /// @copydoc ChainType::SendRequestSegment
        void SendRequestSegment(std::shared_ptr<Segment> seg) override {
            BufferChain chain;
            chain.Append(std::move(seg));
            tls_->Write(std::move(chain));
        }

        /// @copydoc ChainType::GetApiKey
        const std::string& GetApiKey() const override { return api_key_; }

    private:
        SegmentAllocator allocator_;
        std::string api_key_;
        std::shared_ptr<ParserType> parser_;
        std::shared_ptr<ZstdType> zstd_;
        std::shared_ptr<HttpType> http_;
        std::shared_ptr<TlsType> tls_;
        std::shared_ptr<HeadType> head_;
        std::function<void()> ready_cb_;
    };

    /// Build the component chain for the historical protocol.
    /// @param loop     Event loop that drives I/O.
    /// @param sink     Destination for parsed record batches.
    /// @param api_key  Databento API key for HTTP basic-auth.
    /// @return Owning pointer to the newly created chain.
    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop& loop,
        StreamRecordSink& sink,
        const std::string& api_key
    ) {
        return std::make_shared<ChainImpl>(loop, sink, api_key);
    }

    /// Build the component chain with an explicit allocator.
    /// @param loop     Event loop that drives I/O.
    /// @param sink     Destination for parsed record batches.
    /// @param api_key  Databento API key for HTTP basic-auth.
    /// @param alloc    Allocator to copy into the chain; all components share it.
    /// @return Owning pointer to the newly created chain.
    static std::shared_ptr<ChainType> BuildChain(
        IEventLoop& loop,
        StreamRecordSink& sink,
        const std::string& api_key,
        SegmentAllocator* alloc
    ) {
        return std::make_shared<ChainImpl>(loop, sink, api_key, alloc);
    }

    /// Build and send an HTTP GET request for the given historical parameters.
    /// Formats directly into a segment buffer with zero intermediate copies.
    /// @param chain    Active chain (must be connected and ready).
    /// @param request  Parameters describing the historical query.
    static void SendRequest(std::shared_ptr<ChainType>& chain, const Request& request) {
        if (!chain) return;

        // Get segment and format directly into it using char* as output iterator
        auto seg = chain->GetRequestSegment();
        char* buf = reinterpret_cast<char*>(seg->data.data());
        char* out = buf;

        // Build HTTP GET request for historical data API
        auto builder = HttpRequestBuilder(out)
            .Method("GET")
            .Path("/v0/timeseries.get_range")
            .QueryParam("dataset", request.dataset)
            .QueryParam("symbols", request.symbols)
            .QueryParam("schema", request.schema)
            .QueryParam("start", request.start.nanos)
            .QueryParam("end", request.end.nanos)
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

        seg->size = static_cast<size_t>(builder.GetIterator() - buf);
        chain->SendRequestSegment(std::move(seg));
    }

    /// Close the chain and release its resources.
    /// @param chain  Chain to tear down; may be null.
    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) {
            chain->Close();
        }
    }

    /// @return The gateway hostname (always kHistoricalHostname for historical).
    static std::string GetHostname(const Request&) {
        return kHistoricalHostname;
    }

    /// @return The gateway port (always kHistoricalPort).
    static uint16_t GetPort(const Request&) {
        return kHistoricalPort;
    }
};

}  // namespace dbn_pipe
