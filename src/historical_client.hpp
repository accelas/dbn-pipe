// src/historical_client.hpp
#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>

#include "data_source.hpp"
#include "http_client.hpp"
#include "reactor.hpp"
#include "tcp_socket.hpp"
#include "tls_socket.hpp"
#include "zstd_decompressor.hpp"

namespace databento_async {

// Forward declaration
class HistoricalClient;

// ApplicationSink is the final downstream in the pipeline.
// It receives decompressed data and forwards it to HistoricalClient for parsing.
class ApplicationSink {
public:
    explicit ApplicationSink(HistoricalClient* client) : client_(client) {}

    // Invalidate the sink (called before client cleanup)
    void Invalidate() { valid_ = false; }

    // Downstream interface
    void Read(std::pmr::vector<std::byte> data);
    void OnError(const Error& e);
    void OnDone();

private:
    HistoricalClient* client_;
    bool valid_ = true;
};

// HistoricalClient fetches historical data from the Databento Historical API.
// Uses HTTPS (TLS) + HTTP + zstd decompression pipeline.
//
// Lifecycle:
// 1. Construct with reactor and API key
// 2. Call Request() to set dataset, symbols, schema, start/end timestamps
// 3. Call Connect() to initiate TLS connection
// 4. Call Start() to begin streaming data
// 5. Receive records via OnRecord() callback
// 6. Call Stop() to close connection
class HistoricalClient : public DataSource {
public:
    enum class State {
        Disconnected,       // Initial state, not connected
        Connecting,         // TCP connection in progress
        TlsHandshaking,     // TLS handshake in progress
        SendingRequest,     // Sending HTTP request
        ReceivingResponse,  // Receiving HTTP response headers
        Streaming,          // Receiving and parsing DBN data
        Complete,           // All data received successfully
        Error               // An error occurred
    };

    HistoricalClient(Reactor& reactor, std::string api_key);
    ~HistoricalClient() override;

    // Non-copyable, non-movable
    HistoricalClient(const HistoricalClient&) = delete;
    HistoricalClient& operator=(const HistoricalClient&) = delete;
    HistoricalClient(HistoricalClient&&) = delete;
    HistoricalClient& operator=(HistoricalClient&&) = delete;

    // Set request parameters (call before Connect)
    // start and end are Unix timestamps in nanoseconds
    void Request(std::string_view dataset,
                 std::string_view symbols,
                 std::string_view schema,
                 std::uint64_t start,
                 std::uint64_t end);

    // Connection (caller responsible for DNS resolution)
    void Connect(const sockaddr_storage& addr);

    // DataSource interface
    void Start() override;
    void Stop() override;

    // State accessor
    State GetState() const { return state_; }

private:
    friend class ApplicationSink;

    // Pipeline setup
    void BuildPipeline();

    // TCP socket callbacks
    void HandleTcpConnect();
    void HandleTcpRead(std::span<const std::byte> data);
    void HandleTcpError(std::error_code ec);

    // Pipeline event handlers
    void HandleDecompressedData(std::pmr::vector<std::byte> data);
    void HandlePipelineError(const Error& e);
    void HandlePipelineComplete();

    // HTTP request generation
    void SendHttpRequest();
    std::string BuildHttpRequest() const;

    Reactor& reactor_;
    std::string api_key_;
    State state_ = State::Disconnected;
    bool stopped_ = false;  // Guards against callbacks after Stop()

    // Request parameters
    std::string dataset_;
    std::string symbols_;
    std::string schema_;
    std::uint64_t start_ = 0;
    std::uint64_t end_ = 0;

    // Pipeline components (bottom-up: decompressor -> http -> tls -> tcp)
    // The sink receives decompressed data and forwards to HistoricalClient
    std::shared_ptr<ApplicationSink> sink_;

    // ZstdDecompressor receives HTTP body and decompresses
    std::shared_ptr<ZstdDecompressor<ApplicationSink>> decompressor_;

    // HttpClient parses HTTP response and extracts body
    std::shared_ptr<HttpClient<ZstdDecompressor<ApplicationSink>>> http_;

    // TlsSocket handles TLS encryption/decryption
    std::shared_ptr<TlsSocket<HttpClient<ZstdDecompressor<ApplicationSink>>>> tls_;

    // TcpSocket handles raw TCP I/O
    std::unique_ptr<TcpSocket> tcp_;

    // PMR pool for TLS write buffers
    std::pmr::unsynchronized_pool_resource pool_;
};

}  // namespace databento_async
