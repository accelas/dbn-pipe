// src/historical_protocol.hpp
#pragma once

#include <cctype>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <memory>
#include <span>
#include <spanstream>
#include <string>

#include "buffer_chain.hpp"
#include "dbn_parser_component.hpp"
#include "http_client.hpp"
#include "pipeline_component.hpp"
#include "pipeline_sink.hpp"
#include "reactor.hpp"
#include "sink_adapter.hpp"
#include "tcp_socket.hpp"
#include "tls_transport.hpp"
#include "zstd_decompressor.hpp"

namespace databento_async {

// HistoricalRequest - Parameters for historical data download
struct HistoricalRequest {
    std::string dataset;   // Dataset to query (e.g., "GLBX.MDP3")
    std::string symbols;   // Symbol(s) to query (e.g., "ESZ4")
    std::string schema;    // Schema for data (e.g., "mbp-1")
    uint64_t start;        // Start time in nanoseconds since Unix epoch
    uint64_t end;          // End time in nanoseconds since Unix epoch
};

// HistoricalProtocol - ProtocolDriver implementation for historical downloads
//
// Satisfies the ProtocolDriver concept. Uses TLS -> HTTP -> Zstd -> DBN parser chain.
//
// Chain: TcpSocket -> TlsTransport -> HttpClient -> ZstdDecompressor -> DbnParserComponent -> SinkAdapter -> Sink
//
// Historical protocol requires TLS handshake before sending HTTP request.
// OnConnect starts the handshake and returns false (not ready yet).
// OnRead returns true after handshake completes.
struct HistoricalProtocol {
    using Request = HistoricalRequest;

    // URL encode helper - writes directly to output stream
    static void UrlEncode(std::ostream& out, const std::string& value) {
        auto flags = out.flags();
        out.fill('0');
        out << std::hex;

        for (char c : value) {
            // Keep alphanumeric and other accepted characters
            if (std::isalnum(static_cast<unsigned char>(c)) ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                out << c;
            } else {
                // Percent-encode the character
                out << '%' << std::setw(2) << std::uppercase
                    << static_cast<int>(static_cast<unsigned char>(c));
            }
        }

        out.flags(flags);  // Restore flags for subsequent output
    }

    // Base64 encode - writes directly to output stream
    static void Base64Encode(std::ostream& out, std::string_view input) {
        static const char* kBase64Chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        size_t i = 0;
        while (i + 2 < input.size()) {
            uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) |
                              (static_cast<uint8_t>(input[i + 1]) << 8) |
                              static_cast<uint8_t>(input[i + 2]);
            out << kBase64Chars[(triple >> 18) & 0x3F];
            out << kBase64Chars[(triple >> 12) & 0x3F];
            out << kBase64Chars[(triple >> 6) & 0x3F];
            out << kBase64Chars[triple & 0x3F];
            i += 3;
        }

        if (i + 1 == input.size()) {
            uint32_t val = static_cast<uint8_t>(input[i]) << 16;
            out << kBase64Chars[(val >> 18) & 0x3F];
            out << kBase64Chars[(val >> 12) & 0x3F];
            out << '=';
            out << '=';
        } else if (i + 2 == input.size()) {
            uint32_t val = (static_cast<uint8_t>(input[i]) << 16) |
                           (static_cast<uint8_t>(input[i + 1]) << 8);
            out << kBase64Chars[(val >> 18) & 0x3F];
            out << kBase64Chars[(val >> 12) & 0x3F];
            out << kBase64Chars[(val >> 6) & 0x3F];
            out << '=';
        }
    }

    // ChainType wraps the TLS -> HTTP -> Zstd -> DBN parser chain
    // Type-erased wrapper to avoid exposing the Record template
    struct ChainType {
        virtual ~ChainType() = default;
        virtual void Read(BufferChain data) = 0;
        virtual void OnError(const Error& e) = 0;
        virtual void OnDone() = 0;
        virtual void Close() = 0;
        virtual void SetWriteCallback(std::function<void(BufferChain)> cb) = 0;
        virtual void StartHandshake() = 0;
        virtual bool IsHandshakeComplete() const = 0;
        virtual std::shared_ptr<Segment> GetRequestSegment() = 0;
        virtual void SendRequestSegment(std::shared_ptr<Segment> seg) = 0;
        virtual const std::string& GetApiKey() const = 0;
    };

    // Concrete implementation of ChainType for a specific Record type
    template <typename Record>
    struct ChainImpl : ChainType {
        using SinkAdapterType = SinkAdapter<Record>;
        using ParserType = DbnParserComponent<SinkAdapterType>;
        using ZstdType = ZstdDecompressor<ParserType>;
        using HttpType = HttpClient<ZstdType>;
        using TlsType = TlsTransport<HttpType>;

        ChainImpl(Reactor& reactor, Sink<Record>& sink, const std::string& api_key)
            : api_key_(api_key)
            , sink_adapter_(std::make_unique<SinkAdapterType>(sink))
            , parser_(std::make_shared<ParserType>(*sink_adapter_))
            , zstd_(ZstdType::Create(reactor, parser_))
            , http_(HttpType::Create(reactor, zstd_))
            , tls_(TlsType::Create(reactor, http_))
        {
            // Wire up upstream pointers for backpressure
            http_->SetUpstream(tls_.get());
            zstd_->SetUpstream(http_.get());
        }

        void Read(BufferChain data) override {
            tls_->Read(std::move(data));
        }

        void OnError(const Error& e) override {
            // Forward error through chain
            http_->OnError(e);
        }

        void OnDone() override {
            // Forward done through chain
            http_->OnDone();
        }

        void Close() override {
            tls_->RequestClose();
        }

        void SetWriteCallback(std::function<void(BufferChain)> cb) override {
            tls_->SetUpstreamWriteCallback(std::move(cb));
        }

        void StartHandshake() override {
            tls_->StartHandshake();
        }

        bool IsHandshakeComplete() const override {
            return tls_->IsHandshakeComplete();
        }

        std::shared_ptr<Segment> GetRequestSegment() override {
            return std::make_shared<Segment>();
        }

        void SendRequestSegment(std::shared_ptr<Segment> seg) override {
            BufferChain chain;
            chain.Append(std::move(seg));
            tls_->Write(std::move(chain));
        }

        const std::string& GetApiKey() const override {
            return api_key_;
        }

    private:
        std::string api_key_;
        std::unique_ptr<SinkAdapterType> sink_adapter_;
        std::shared_ptr<ParserType> parser_;
        std::shared_ptr<ZstdType> zstd_;
        std::shared_ptr<HttpType> http_;
        std::shared_ptr<TlsType> tls_;
    };

    // Build the component chain for historical protocol
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

    // Handle TCP connect - start TLS handshake
    // Returns false because we need to wait for handshake completion
    static bool OnConnect(std::shared_ptr<ChainType>& chain) {
        if (chain) {
            chain->StartHandshake();
        }
        return false;  // Not ready yet - wait for TLS handshake
    }

    // Handle TCP read - forward data to chain, return handshake status
    static bool OnRead(std::shared_ptr<ChainType>& chain, BufferChain data) {
        if (chain && !data.Empty()) {
            chain->Read(std::move(data));
        }
        // Return true when handshake is complete (ready to send request)
        return chain && chain->IsHandshakeComplete();
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
        out << "GET /v0/timeseries.get_range?dataset=";
        UrlEncode(out, request.dataset);
        out << "&symbols=";
        UrlEncode(out, request.symbols);
        out << "&schema=";
        UrlEncode(out, request.schema);
        out << "&start=" << request.start
            << "&end=" << request.end
            << "&encoding=dbn"
            << "&compression=zstd"
            << " HTTP/1.1\r\n"
            << "Host: hist.databento.com\r\n"
            << "Authorization: Basic ";
        Base64Encode(out, chain->GetApiKey() + ":");
        out << "\r\n"
            << "Accept: application/octet-stream\r\n"
            << "Accept-Encoding: zstd\r\n"
            << "Connection: close\r\n"
            << "\r\n";

        seg->size = out.span().size();
        chain->SendRequestSegment(std::move(seg));
    }

    // Teardown - close the chain
    static void Teardown(std::shared_ptr<ChainType>& chain) {
        if (chain) {
            chain->Close();
        }
    }
};

}  // namespace databento_async
